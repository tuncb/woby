#include "ui.h"
#include "console.h"
#include "imgui_graphics.h"
#include "utf8_path.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui_impl_sdl3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <future>
#include <chrono>
#include <stdexcept>
#include <string>

namespace {
namespace g = woby::graphics;
constexpr g::ViewId uiView = 200;
struct Options {
    bool smoke = false;
    std::filesystem::path screenshot, workflowsDirectory;
    std::string workflow, comparison;
    std::optional<size_t> inspectPane;
    mesh_lab::InspectorTab inspectorTab = mesh_lab::InspectorTab::stage;
    bool hideLibrary = false, hideInspectors = false;
    std::optional<mesh_lab::Node> node;
    size_t triangle = 2, corner = 0;
    int width = 1600, height = 1000, frameLimit = 0;
};
Options options(int argc, char** argv)
{
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (++i >= argc) { throw std::runtime_error("Missing value for " + arg); }
            return argv[i];
        };
        if (arg == "--smoke") { result.smoke = true; }
        else if (arg == "--screenshot") { result.screenshot = std::filesystem::absolute(woby::pathFromUtf8(value())); }
        else if (arg == "--workflows-dir") { result.workflowsDirectory = std::filesystem::absolute(woby::pathFromUtf8(value())); }
        else if (arg == "--workflow") { result.workflow = value(); }
        else if (arg == "--compare") { result.comparison = value(); }
        else if (arg == "--hide-library") { result.hideLibrary = true; }
        else if (arg == "--hide-inspectors") { result.hideInspectors = true; }
        else if (arg == "--inspector-tab") {
            const auto tab = value();
            if (tab == "stage") { result.inspectorTab = mesh_lab::InspectorTab::stage; }
            else if (tab == "mesh") { result.inspectorTab = mesh_lab::InspectorTab::mesh; }
            else if (tab == "bytes") { result.inspectorTab = mesh_lab::InspectorTab::bytes; }
            else { throw std::runtime_error("--inspector-tab expects stage, mesh or bytes."); }
        }
        else if (arg == "--inspect") {
            const auto pane = value();
            if (pane != "top" && pane != "bottom") { throw std::runtime_error("--inspect expects top or bottom."); }
            result.inspectPane = pane == "top" ? 0 : 1;
        }
        else if (arg == "--triangle") { result.triangle = std::stoull(value()); }
        else if (arg == "--corner") { result.corner = std::stoull(value()); }
        else if (arg == "--width") { result.width = std::stoi(value()); }
        else if (arg == "--height") { result.height = std::stoi(value()); }
        else if (arg == "--frames") { result.frameLimit = std::stoi(value()); }
        else if (arg == "--node") {
            const auto key = value(); bool found = false;
            for (const auto& node : mesh_lab::pipeline) {
                if (key == node.key) { result.node = node.id; found = true; break; }
            }
            if (!found) { throw std::runtime_error("Unknown pipeline node: " + key); }
        } else if (arg == "--help") {
            std::puts("mesh_memory_lab: saved workflow diagrams and live mesh inspection\n"
                "  [--workflows-dir folder] [--workflow commit/file.meshflow]\n"
                "  [--compare commit/file.meshflow] [--inspect top|bottom]\n"
                "  [--inspector-tab stage|mesh|bytes] [--hide-library] [--hide-inspectors]\n"
                "  [--screenshot absolute.png] [--smoke] [--frames N]\n"
                "  [--node source|parse|attributes|triangulate|corners|pack|mesh|upload|gpu]\n"
                "  [--triangle N] [--corner N] [--width 1600] [--height 1000]");
            std::exit(0);
        } else { throw std::runtime_error("Unknown argument: " + arg); }
    }
    if (result.width < 1200 || result.width > 4096 || result.height < 860 || result.height > 2160) {
        throw std::runtime_error("Inspector dimensions must be 1200..4096 x 860..2160.");
    }
    if (result.inspectPane == 1 && result.comparison.empty()) {
        throw std::runtime_error("--inspect bottom requires --compare.");
    }
    return result;
}
void finishReadback(mesh_lab::GpuCapture& capture)
{
    for (int i = 0; i < 16 && !capture.complete; ++i) { mesh_lab::pollReadback(capture, g::frame()); }
    if (!capture.complete || !capture.matches) { throw std::runtime_error("GPU upload/readback verification failed."); }
}
void savePng(const std::filesystem::path& path, g::TextureHandle image, int width, int height)
{
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    const auto ready = g::readTexture(image, pixels.data());
    while (g::frame() < ready) {}
    SDL_Surface* surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels.data(), width * 4);
    if (!surface) { throw std::runtime_error(SDL_GetError()); }
    const auto output = woby::pathToUtf8(path);
    const bool saved = SDL_SavePNG(surface, output.c_str());
    SDL_DestroySurface(surface);
    if (!saved) { throw std::runtime_error(SDL_GetError()); }
}
}

int main(int argc, char** argv)
{
    woby::initializeConsole();
    SDL_Window* window = nullptr;
    bool sdlInitialized = false, graphicsInitialized = false, imguiInitialized = false, platformInitialized = false, uiRendererInitialized = false;
    std::array<mesh_lab::GpuCapture, 2> captures;
    auto& capture = captures[0];
    std::array<mesh_lab::Viewport, 2> viewports;
    auto& viewport = viewports[0];
    g::TextureHandle output;
    g::FrameBufferHandle outputTarget;
    int exitCode = 0;
    try {
        const auto config = options(argc, argv);
        const bool headless = config.smoke || !config.screenshot.empty();
        const auto workflowDirectory = config.workflowsDirectory.empty()
            ? std::filesystem::path(SDL_GetBasePath()) / "workflows" : config.workflowsDirectory;
        auto library = std::async(std::launch::async, [workflowDirectory] {
            return mesh_lab::loadWorkflowLibrary(workflowDirectory);
        }).get();
        mesh_lab::WorkspaceState workspace;
        auto& state = workspace.panes[0];
        const auto initialWorkflow = mesh_lab::findWorkflow(library,config.workflow);
        mesh_lab::openWorkflowPane(workspace,library,initialWorkflow,0);
        if (!config.comparison.empty()) {
            mesh_lab::openWorkflowPane(workspace,library,mesh_lab::findWorkflow(library,config.comparison),1);
        }
        mesh_lab::focusWorkflowPane(workspace,config.inspectPane.value_or(0));
        mesh_lab::setWorkflowInspectorVisible(workspace,!config.hideInspectors);
        mesh_lab::setWorkflowLibraryVisible(workspace,!config.hideLibrary);
        for (auto& pane : workspace.panes) {
            const bool hasMesh = pane.workflow < library.entries.size() && library.entries[pane.workflow].trace;
            mesh_lab::selectInspectorTab(pane,config.inspectorTab,hasMesh);
        }
        if (!SDL_Init(headless ? 0 : SDL_INIT_VIDEO)) { throw std::runtime_error(SDL_GetError()); }
        sdlInitialized = true;
        if (!headless) {
            window = SDL_CreateWindow("Mesh memory lab", config.width, config.height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
#if defined(__APPLE__)
                SDL_WINDOW_METAL
#else
                SDL_WINDOW_VULKAN
#endif
            );
            if (!window) { throw std::runtime_error(SDL_GetError()); }
            SDL_SetWindowMinimumSize(window, 1200, 860);
        }
        g::Init init;
        init.platformData.window = window;
        init.resolution.width = static_cast<uint32_t>(config.width);
        init.resolution.height = static_cast<uint32_t>(config.height);
        init.resolution.reset = headless ? WOBY_GPU_RESET_NONE : WOBY_GPU_RESET_VSYNC;
        if (!g::init(init)) { throw std::runtime_error(g::initializationError()); }
        graphicsInitialized = true;
        const std::filesystem::path assets = std::filesystem::path(SDL_GetBasePath()) / "assets";
        for (auto& view : viewports) { mesh_lab::initViewport(view, assets); }
        if (config.smoke) {
            mesh_lab::resizeViewport(viewport, 320, 240);
            if (library.entries.empty()) { throw std::runtime_error("No workflows to validate."); }
            for (size_t index = 0; index < library.entries.size(); ++index) {
                const auto& entry = library.entries[index];
                if (!entry.document) { throw std::runtime_error(woby::pathToUtf8(entry.path.filename()) + ": " + entry.error); }
                if (!entry.trace) { std::printf("PASS diagram: %s\n",entry.document->title.c_str()); continue; }
                const auto& trace = *entry.trace;
                mesh_lab::selectWorkflow(state,library,index);
                mesh_lab::upload(capture, trace); finishReadback(capture);
                const auto matrices = mesh_lab::viewMatrices(trace, state, 4.0f / 3.0f, g::getCaps()->homogeneousDepth);
                woby::Vertex center; center.position = trace.mesh.bounds.center;
                const auto projected = mesh_lab::projectVertex(matrices, center);
                if (std::abs(projected[0] - .5f) > .001f || std::abs(projected[1] - .5f) > .001f) {
                    throw std::runtime_error("Projection does not center the camera target.");
                }
                mesh_lab::renderViewport(viewport, capture, trace, state);
                std::vector<uint8_t> pixels(320 * 240 * 4);
                const auto ready = g::readTexture(viewport.color, pixels.data());
                while (g::frame() < ready) {}
                size_t shaded = 0;
                for (size_t p = 0; p < pixels.size(); p += 4) {
                    if (pixels[p] != 0x11 || pixels[p+1] != 0x19 || pixels[p+2] != 0x20) { ++shaded; }
                }
                if (shaded < 100) { throw std::runtime_error("GPU viewport did not rasterize the mesh."); }
                // Both targets must match isolated rendering, even when submitted together.
                // This catches reused view IDs/framebuffers and camera state leaking across panes.
                auto other = state; mesh_lab::orbit(other,.8f,.2f,1);
                mesh_lab::resizeViewport(viewports[1],320,240);
                mesh_lab::renderViewport(viewports[1],capture,trace,other,1);
                std::vector<uint8_t> expectedBottom(pixels.size());
                auto bottomReady = g::readTexture(viewports[1].color,expectedBottom.data());
                while (g::frame() < bottomReady) {}
                mesh_lab::renderViewport(viewport,capture,trace,state,0);
                mesh_lab::renderViewport(viewports[1],capture,trace,other,1);
                std::vector<uint8_t> topPixels(pixels.size()), bottomPixels(pixels.size());
                const auto topReady = g::readTexture(viewport.color,topPixels.data());
                bottomReady = g::readTexture(viewports[1].color,bottomPixels.data());
                while (g::frame() < std::max(topReady,bottomReady)) {}
                if (topPixels != pixels || bottomPixels != expectedBottom) {
                    throw std::runtime_error("Simultaneous mesh viewports differ from isolated rendering.");
                }
                std::printf("PASS %s: %zu vertices, %zu triangles, %zu verified GPU bytes\n",
                    entry.document->title.c_str(), trace.mesh.vertices.size(), trace.triangles.size(), capture.vertexReadback.size()+capture.indexReadback.size());
            }
        } else {
            const auto currentTrace = [&](size_t pane) -> const mesh_lab::Trace* {
                const auto index = workspace.panes[pane].workflow;
                return index < library.entries.size() ? library.entries[index].trace.get() : nullptr;
            };
            const auto uploadWorkspace = [&](const mesh_lab::WorkflowLibrary& source, const mesh_lab::WorkspaceState& selection) {
                for (size_t pane = 0; pane < captures.size(); ++pane) {
                    const auto index = selection.panes[pane].workflow;
                    if (pane < selection.paneCount && index < source.entries.size() && source.entries[index].trace) {
                        mesh_lab::upload(captures[pane],*source.entries[index].trace);
                    } else { mesh_lab::destroy(captures[pane]); }
                }
            };
            const auto readbacksComplete = [&]() {
                return std::all_of(captures.begin(),captures.end(),[](const auto& gpu) {
                    return !g::isValid(gpu.vertices) || gpu.complete;
                });
            };
            const auto verifyCaptures = [&]() {
                for (const auto& gpu : captures) {
                    if (g::isValid(gpu.vertices) && !gpu.matches) { throw std::runtime_error("Workflow capture has a GPU byte mismatch."); }
                }
            };
            auto& initial = workspace.panes[workspace.activePane];
            if (initial.workflow < library.entries.size()) {
                if (config.node) {
                    const auto& workflow = *library.entries[initial.workflow].document;
                    for (size_t i = 0; i < workflow.nodes.size(); ++i) {
                        if (workflow.nodes[i].inspector == config.node) { mesh_lab::selectWorkflowNode(initial,workflow,i); break; }
                    }
                }
                if (const auto* trace = currentTrace(workspace.activePane)) {
                    mesh_lab::selectTriangle(initial,*trace,config.triangle); mesh_lab::selectCorner(initial,config.corner);
                }
            }
            uploadWorkspace(library,workspace);
            struct LibraryResult {
                mesh_lab::WorkflowLibrary library;
                std::optional<std::filesystem::path> selected;
                size_t pane = 0;
            };
            std::future<LibraryResult> libraryJob;
            mesh_lab::UiActions pending;
            IMGUI_CHECKVERSION(); ImGui::CreateContext(); imguiInitialized = true;
            mesh_lab::UiRuntime ui;
            mesh_lab::configureStyle(ui, assets);
            if (window) {
                if (!ImGui_ImplSDL3_InitForOther(window)) { throw std::runtime_error("ImGui SDL3 initialization failed."); }
                platformInitialized = true;
            }
            woby::imgui_graphics::init(assets, uiView); uiRendererInitialized = true;
            if (headless) {
                output = g::createTexture2D(static_cast<uint16_t>(config.width), static_cast<uint16_t>(config.height), false, 1,
                    g::TextureFormat::RGBA8, WOBY_GPU_TEXTURE_RT | WOBY_GPU_TEXTURE_READ_BACK);
                outputTarget = g::createFrameBuffer(1, &output);
                if (!g::isValid(outputTarget)) { throw std::runtime_error("Screenshot allocation failed."); }
                g::setViewFrameBuffer(uiView, outputTarget);
            }
            bool running = true;
            int frames = 0, drawableWidth = config.width, drawableHeight = config.height;
            while (running) {
                const bool canSwitch = readbacksComplete();
                if (canSwitch && libraryJob.valid() && libraryJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    std::optional<LibraryResult> result;
                    try { result = libraryJob.get(); }
                    catch (const std::exception& error) { ui.message = error.what(); }
                    ui.loading = false;
                    if (result) {
                        auto restored = mesh_lab::reconcileWorkspace(workspace,library,result->library);
                        if (result->selected) {
                            mesh_lab::openWorkflowPane(restored,result->library,
                                mesh_lab::findWorkflow(result->library,woby::pathToUtf8(*result->selected)),result->pane);
                        }
                        // GPU allocation failures leave the frame loop before any
                        // inspector can pair old bytes with the new CPU snapshots.
                        uploadWorkspace(result->library,restored);
                        library = std::move(result->library); workspace = restored;
                        ui.message = result->selected ? "Workflow copy saved." : "Workflow folder loaded.";
                    }
                }
                if (canSwitch && pending.close) {
                    const auto pane = *pending.close; pending.close.reset();
                    if (workspace.paneCount == 2 && pane < 2) {
                        mesh_lab::destroy(captures[pane]);
                        if (pane == 0) { captures[0] = std::move(captures[1]); captures[1] = {}; }
                        mesh_lab::closeWorkflowPane(workspace,pane);
                    }
                }
                if (canSwitch && pending.open) {
                    const auto request = *pending.open; pending.open.reset();
                    const auto index = request.workflow;
                    const auto pane = request.pane == 1 && workspace.panes[0].workflow == mesh_lab::noWorkflow ? 0 : request.pane;
                    if (pane < captures.size() && index < library.entries.size() && library.entries[index].document) {
                        if (library.entries[index].trace) { mesh_lab::upload(captures[pane],*library.entries[index].trace); }
                        else { mesh_lab::destroy(captures[pane]); }
                        mesh_lab::openWorkflowPane(workspace,library,index,pane);
                    }
                }
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (platformInitialized) { ImGui_ImplSDL3_ProcessEvent(&event); }
                    if (event.type == SDL_EVENT_QUIT) { running = false; }
                }
                if (window) {
                    int width = 0, height = 0; SDL_GetWindowSizeInPixels(window, &width, &height);
                    if (width > 0 && height > 0 && (width != drawableWidth || height != drawableHeight)) {
                        drawableWidth = width; drawableHeight = height;
                        g::reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height), WOBY_GPU_RESET_VSYNC);
                    }
                    ImGui_ImplSDL3_NewFrame();
                } else {
                    auto& io = ImGui::GetIO(); io.DisplaySize = {static_cast<float>(config.width), static_cast<float>(config.height)};
                    io.DeltaTime = 1.0f / 60.0f;
                }
                ImGui::NewFrame();
                mesh_lab::UiActions actions;
                mesh_lab::drawUi(ui,workspace,library,captures,viewports,actions);
                for (size_t pane = 0; pane < workspace.paneCount; ++pane) {
                    if (const auto* trace = currentTrace(pane); trace && ui.viewportVisible[pane]) {
                        mesh_lab::renderViewport(viewports[pane],captures[pane],*trace,workspace.panes[pane],static_cast<g::ViewId>(pane));
                    }
                }
                if (actions.open) { pending.open = actions.open; }
                if (actions.close) { pending.close = actions.close; }
                if ((actions.reload || actions.saveCopy) && !libraryJob.valid()) {
                    const auto directory = library.directory;
                    const auto pane = actions.saveCopy.value_or(workspace.activePane);
                    const auto index = workspace.panes[pane].workflow;
                    const auto copy = actions.saveCopy && index < library.entries.size()
                        ? std::optional<mesh_lab::WorkflowEntry>{library.entries[index]} : std::nullopt;
                    pending = {}; ui.loading = true; ui.message.clear();
                    libraryJob = std::async(std::launch::async,[directory,pane,copy] {
                        std::optional<std::filesystem::path> selected;
                        if (copy) { selected = mesh_lab::saveWorkflowCopy(*copy).lexically_relative(directory); }
                        return LibraryResult{mesh_lab::loadWorkflowLibrary(directory),selected,pane};
                    });
                }
                ImGui::Render(); woby::imgui_graphics::render(ImGui::GetDrawData());
                const auto frame = g::frame();
                for (auto& gpu : captures) {
                    if (g::isValid(gpu.vertices)) { mesh_lab::pollReadback(gpu,frame); }
                }
                const bool ready = readbacksComplete();
                ++frames;
                if (headless && frames >= 8 && ready) {
                    verifyCaptures();
                    savePng(config.screenshot, output, config.width, config.height);
                    std::printf("Saved %s; workflow rendered.\n", woby::pathToUtf8(config.screenshot).c_str());
                    running = false;
                }
                if (config.frameLimit > 0 && frames >= config.frameLimit && ready) {
                    verifyCaptures();
                    std::printf("Presented %d frames; workflow rendered.\n", frames);
                    running = false;
                }
            }
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mesh memory lab: %s\n", error.what()); exitCode = 1;
    }
    if (graphicsInitialized) {
        // Readback destinations must outlive completion, including exceptional exits.
        if (g::isValid(viewport.presentation)) {
            try { for (int i = 0; i < 8; ++i) { g::frame(); } }
            catch (const std::exception& error) { std::fprintf(stderr, "Frame drain failed: %s\n", error.what()); exitCode = 1; }
        }
        if (uiRendererInitialized) { woby::imgui_graphics::shutdown(); }
        if (platformInitialized) { ImGui_ImplSDL3_Shutdown(); }
        if (imguiInitialized) { ImGui::DestroyContext(); }
        for (auto& view : viewports) { mesh_lab::destroy(view); }
        for (auto& gpu : captures) { mesh_lab::destroy(gpu); }
        if (g::isValid(outputTarget)) { g::destroy(outputTarget); }
        if (g::isValid(output)) { g::destroy(output); }
        g::shutdown();
    }
    if (window) { SDL_DestroyWindow(window); }
    if (sdlInitialized) { SDL_Quit(); }
    return exitCode;
}
