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
#include <stdexcept>
#include <string>

namespace {
namespace g = woby::graphics;
constexpr g::ViewId uiView = 200;
struct Options {
    bool smoke = false;
    std::filesystem::path screenshot;
    mesh_lab::Node node = mesh_lab::Node::pack;
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
            std::puts("mesh_memory_lab: one internal folded-sheet example\n"
                "  [--screenshot absolute.png] [--smoke] [--frames N]\n"
                "  [--node source|parse|attributes|triangulate|corners|pack|mesh|upload|gpu]\n"
                "  [--triangle N] [--corner N] [--width 1600] [--height 1000]");
            std::exit(0);
        } else { throw std::runtime_error("Unknown argument: " + arg); }
    }
    if (result.width < 1200 || result.width > 4096 || result.height < 860 || result.height > 2160) {
        throw std::runtime_error("Inspector dimensions must be 1200..4096 x 860..2160.");
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
    mesh_lab::GpuCapture capture;
    mesh_lab::Viewport viewport;
    g::TextureHandle output;
    g::FrameBufferHandle outputTarget;
    int exitCode = 0;
    try {
        const auto config = options(argc, argv);
        const bool headless = config.smoke || !config.screenshot.empty();
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
        mesh_lab::initViewport(viewport, assets);
        if (config.smoke) {
            mesh_lab::resizeViewport(viewport, 320, 240);
            {
                const auto trace = mesh_lab::internalExample();
                mesh_lab::upload(capture, trace); finishReadback(capture);
                const mesh_lab::UiState state;
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
                std::printf("PASS internal example: %zu vertices, %zu triangles, %zu verified GPU bytes\n",
                    trace.mesh.vertices.size(), trace.triangles.size(), capture.vertexReadback.size()+capture.indexReadback.size());
            }
        } else {
            const auto trace = std::make_shared<const mesh_lab::Trace>(mesh_lab::internalExample());
            mesh_lab::UiState state;
            mesh_lab::selectNode(state, config.node);
            mesh_lab::selectTriangle(state, *trace, config.triangle); mesh_lab::selectCorner(state, config.corner);
            mesh_lab::upload(capture, *trace);
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
                mesh_lab::drawUi(ui, state, *trace, capture, viewport);
                mesh_lab::renderViewport(viewport, capture, *trace, state);
                ImGui::Render(); woby::imgui_graphics::render(ImGui::GetDrawData());
                mesh_lab::pollReadback(capture, g::frame());
                ++frames;
                if (headless && frames >= 8 && capture.complete) {
                    if (!capture.matches) { throw std::runtime_error("Screenshot capture has a GPU byte mismatch."); }
                    savePng(config.screenshot, output, config.width, config.height);
                    std::printf("Saved %s; GPU buffers verified.\n", woby::pathToUtf8(config.screenshot).c_str());
                    running = false;
                }
                if (config.frameLimit > 0 && frames >= config.frameLimit && capture.complete) {
                    if (!capture.matches) { throw std::runtime_error("Native presentation has a GPU byte mismatch."); }
                    std::printf("Presented %d frames; GPU buffers verified.\n", frames);
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
        mesh_lab::destroy(viewport); mesh_lab::destroy(capture);
        if (g::isValid(outputTarget)) { g::destroy(outputTarget); }
        if (g::isValid(output)) { g::destroy(output); }
        g::shutdown();
    }
    if (window) { SDL_DestroyWindow(window); }
    if (sdlInitialized) { SDL_Quit(); }
    return exitCode;
}
