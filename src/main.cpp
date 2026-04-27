#include "bgfx_helpers.h"
#include "camera.h"
#include "imgui_bgfx.h"
#include "obj_mesh.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr uint32_t resetFlags = BGFX_RESET_VSYNC;
constexpr bgfx::ViewId sceneView = 0;
constexpr bgfx::ViewId imguiView = 255;

bgfx::PlatformData platformDataFromSdlWindow(SDL_Window* window)
{
    bgfx::PlatformData platformData{};
    const SDL_PropertiesID properties = SDL_GetWindowProperties(window);

#if defined(_WIN32)
    platformData.nwh = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#elif defined(__APPLE__)
    platformData.nwh = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
#elif defined(__linux__)
    platformData.ndt = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    const Sint64 x11Window = SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (platformData.ndt != nullptr && x11Window != 0) {
        platformData.nwh = reinterpret_cast<void*>(static_cast<uintptr_t>(x11Window));
    } else {
        platformData.ndt = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        platformData.nwh = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    }
#endif

    if (platformData.nwh == nullptr) {
        throw std::runtime_error("Failed to get a native window handle from SDL3.");
    }

    return platformData;
}

std::filesystem::path assetRoot()
{
    const char* basePath = SDL_GetBasePath();
    if (basePath == nullptr) {
        return std::filesystem::current_path() / "assets";
    }

    return std::filesystem::path(basePath) / "assets";
}

void getDrawableSize(SDL_Window* window, uint32_t& width, uint32_t& height)
{
    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    width = static_cast<uint32_t>(std::max(pixelWidth, 1));
    height = static_cast<uint32_t>(std::max(pixelHeight, 1));
}

bgfx::VertexLayout meshVertexLayout()
{
    bgfx::VertexLayout layout;
    layout
        .begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    return layout;
}

class GpuMesh {
public:
    GpuMesh() = default;

    GpuMesh(const woby::ObjMesh& mesh, const bgfx::VertexLayout& layout)
    {
        vertexBuffer_ = bgfx::createVertexBuffer(
            bgfx::copy(mesh.vertices().data(), static_cast<uint32_t>(mesh.vertices().size() * sizeof(woby::Vertex))),
            layout);

        indexBuffer_ = bgfx::createIndexBuffer(
            bgfx::copy(mesh.indices().data(), static_cast<uint32_t>(mesh.indices().size() * sizeof(uint32_t))),
            BGFX_BUFFER_INDEX32);

        indexCount_ = static_cast<uint32_t>(mesh.indices().size());
    }

    GpuMesh(const GpuMesh&) = delete;
    GpuMesh& operator=(const GpuMesh&) = delete;

    GpuMesh(GpuMesh&& other) noexcept
    {
        *this = std::move(other);
    }

    GpuMesh& operator=(GpuMesh&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        destroy();
        vertexBuffer_ = other.vertexBuffer_;
        indexBuffer_ = other.indexBuffer_;
        indexCount_ = other.indexCount_;
        other.vertexBuffer_ = BGFX_INVALID_HANDLE;
        other.indexBuffer_ = BGFX_INVALID_HANDLE;
        other.indexCount_ = 0;
        return *this;
    }

    ~GpuMesh()
    {
        destroy();
    }

    void submit(bgfx::ProgramHandle program) const
    {
        if (!bgfx::isValid(vertexBuffer_) || !bgfx::isValid(indexBuffer_) || indexCount_ == 0) {
            return;
        }

        bgfx::setVertexBuffer(0, vertexBuffer_);
        bgfx::setIndexBuffer(indexBuffer_, 0, indexCount_);
        bgfx::setState(
            BGFX_STATE_WRITE_RGB
            | BGFX_STATE_WRITE_A
            | BGFX_STATE_WRITE_Z
            | BGFX_STATE_DEPTH_TEST_LESS
            | BGFX_STATE_MSAA);
        bgfx::submit(sceneView, program);
    }

private:
    void destroy()
    {
        if (bgfx::isValid(indexBuffer_)) {
            bgfx::destroy(indexBuffer_);
        }
        if (bgfx::isValid(vertexBuffer_)) {
            bgfx::destroy(vertexBuffer_);
        }
        vertexBuffer_ = BGFX_INVALID_HANDLE;
        indexBuffer_ = BGFX_INVALID_HANDLE;
        indexCount_ = 0;
    }

    bgfx::VertexBufferHandle vertexBuffer_ = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle indexBuffer_ = BGFX_INVALID_HANDLE;
    uint32_t indexCount_ = 0;
};

struct SdlDeleter {
    void operator()(SDL_Window* window) const noexcept
    {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
};

struct CommandLineOptions {
    bool showVersion = false;
    std::filesystem::path modelPath;
};

CommandLineOptions parseCommandLine(int argc, char** argv)
{
    CommandLineOptions options;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--version") {
            options.showVersion = true;
            continue;
        }

        if (argument == "--file") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--file requires an OBJ filename.");
            }

            options.modelPath = argv[++index];
            continue;
        }

        if (argument.rfind("--", 0) == 0) {
            throw std::runtime_error("Unknown option: " + argument);
        }

        throw std::runtime_error("Unexpected argument: " + argument);
    }

    return options;
}

} // namespace

int main(int argc, char** argv)
{
    bool sdlInitialized = false;
    bool bgfxInitialized = false;

    try {
        const auto commandLine = parseCommandLine(argc, argv);
        if (commandLine.showVersion) {
            std::printf("%s\n", WOBY_VERSION);
            return 0;
        }

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
        sdlInitialized = true;

        SDL_Window* rawWindow = SDL_CreateWindow("woby OBJ Viewer", 1280, 720, SDL_WINDOW_RESIZABLE);
        if (rawWindow == nullptr) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        std::unique_ptr<SDL_Window, SdlDeleter> window(rawWindow);

        uint32_t width = 0;
        uint32_t height = 0;
        getDrawableSize(window.get(), width, height);

        bgfx::Init init;
        init.type = bgfx::RendererType::Count;
        init.platformData = platformDataFromSdlWindow(window.get());
        init.resolution.width = width;
        init.resolution.height = height;
        init.resolution.reset = resetFlags;

        if (!bgfx::init(init)) {
            throw std::runtime_error("bgfx::init failed.");
        }
        bgfxInitialized = true;

        bgfx::setViewClear(sceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20242aff, 1.0f, 0);
        bgfx::setDebug(BGFX_DEBUG_TEXT);

        const auto assets = assetRoot();
        const auto modelPath = !commandLine.modelPath.empty()
            ? commandLine.modelPath
            : assets / "models" / "cube.obj";

        const auto cpuMesh = woby::ObjMesh::load(modelPath);
        const auto layout = meshVertexLayout();
        GpuMesh gpuMesh(cpuMesh, layout);
        bgfx::ProgramHandle meshProgram = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForOther(window.get())) {
            throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed.");
        }
        woby::imgui_bgfx::init(assets, imguiView);

        bool running = true;
        woby::SceneCamera camera = woby::frameCameraBounds(cpuMesh.bounds());
        woby::CameraInput cameraInput;
        auto previousFrame = std::chrono::steady_clock::now();
        auto fpsWindowStart = previousFrame;
        int fpsFrameCount = 0;
        float fps = 0.0f;

        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);

                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    running = false;
                }
                if (event.type == SDL_EVENT_KEY_DOWN
                    && event.key.key == SDLK_R
                    && !ImGui::GetIO().WantCaptureKeyboard) {
                    camera = woby::frameCameraBounds(cpuMesh.bounds());
                }
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    getDrawableSize(window.get(), width, height);
                    bgfx::reset(width, height, resetFlags);
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !ImGui::GetIO().WantCaptureMouse) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        cameraInput.orbiting = true;
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        cameraInput.panning = true;
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        cameraInput.orbiting = false;
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        cameraInput.panning = false;
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (cameraInput.orbiting) {
                        woby::orbitCamera(camera, event.motion.xrel, event.motion.yrel);
                    }
                    if (cameraInput.panning) {
                        woby::panCamera(camera, event.motion.xrel, event.motion.yrel, static_cast<float>(height));
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL && !ImGui::GetIO().WantCaptureMouse) {
                    const float wheelY = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                        ? -event.wheel.y
                        : event.wheel.y;
                    woby::dollyCamera(camera, -wheelY * 0.12f);
                }
            }

            getDrawableSize(window.get(), width, height);

            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - previousFrame).count();
            previousFrame = now;
            ++fpsFrameCount;
            const float fpsWindowSeconds = std::chrono::duration<float>(now - fpsWindowStart).count();
            if (fpsWindowSeconds >= 1.0f) {
                fps = static_cast<float>(fpsFrameCount) / fpsWindowSeconds;
                fpsFrameCount = 0;
                fpsWindowStart = now;
            }

            const auto& bounds = cpuMesh.bounds();
            woby::updateCameraFromKeyboard(camera, bounds, deltaSeconds);

            bgfx::setViewRect(sceneView, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::touch(sceneView);

            float view[16];
            float projection[16];
            bx::mtxLookAt(view, woby::cameraEye(camera), woby::cameraLookAt(camera));
            bx::mtxProj(
                projection,
                camera.verticalFovDegrees,
                static_cast<float>(width) / static_cast<float>(height),
                camera.nearPlane,
                woby::cameraFarPlane(camera, bounds),
                bgfx::getCaps()->homogeneousDepth);
            bgfx::setViewTransform(sceneView, view, projection);

            float model[16];
            bx::mtxIdentity(model);
            bgfx::setTransform(model);
            gpuMesh.submit(meshProgram);

            bgfx::dbgTextClear();
            bgfx::dbgTextPrintf(0, 1, 0x4f, "Renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
            bgfx::dbgTextPrintf(0, 2, 0x6f, "Model: %s", modelPath.filename().string().c_str());
            bgfx::dbgTextPrintf(0, 3, 0x2f, "FPS: %.1f", fps);

            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::Begin("Viewer");
            ImGui::Text("Model: %s", modelPath.string().c_str());
            ImGui::Text("FPS: %.1f", fps);
            ImGui::Text("Vertices: %zu", cpuMesh.vertices().size());
            ImGui::Text("Indices: %zu", cpuMesh.indices().size());
            ImGui::End();
            ImGui::Render();
            woby::imgui_bgfx::render(ImGui::GetDrawData());

            bgfx::frame();
        }

        woby::imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        bgfx::destroy(meshProgram);
        gpuMesh = GpuMesh();
        bgfx::shutdown();
        bgfxInitialized = false;
        window.reset();
        SDL_Quit();
        sdlInitialized = false;

        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        if (bgfxInitialized) {
            bgfx::shutdown();
        }
        if (sdlInitialized) {
            SDL_Quit();
        }
        return 1;
    }
}
