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
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t resetFlags = BGFX_RESET_VSYNC;
constexpr bgfx::ViewId sceneView = 0;
constexpr bgfx::ViewId imguiView = 255;
constexpr float minVertexPointSize = 1.0f;
constexpr float maxVertexPointSize = 40.0f;
constexpr float minVertexSizeScale = 0.1f;
constexpr float maxVertexSizeScale = 10.0f;
constexpr float minGroupScale = 0.01f;
constexpr float maxGroupScale = 20.0f;
constexpr float minGroupOpacity = 0.0f;
constexpr float maxGroupOpacity = 1.0f;

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

bgfx::VertexLayout pointSpriteVertexLayout()
{
    bgfx::VertexLayout layout;
    layout
        .begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    return layout;
}

std::array<float, 4> objGroupColor(size_t groupIndex)
{
    constexpr std::array<std::array<float, 4>, 12> palette = {{
        {0.90f, 0.22f, 0.24f, 1.0f},
        {0.13f, 0.58f, 0.95f, 1.0f},
        {0.20f, 0.72f, 0.38f, 1.0f},
        {1.00f, 0.70f, 0.18f, 1.0f},
        {0.67f, 0.42f, 0.95f, 1.0f},
        {0.06f, 0.76f, 0.78f, 1.0f},
        {0.96f, 0.36f, 0.67f, 1.0f},
        {0.58f, 0.72f, 0.16f, 1.0f},
        {0.98f, 0.48f, 0.16f, 1.0f},
        {0.40f, 0.55f, 1.00f, 1.0f},
        {0.74f, 0.62f, 0.34f, 1.0f},
        {0.72f, 0.78f, 0.86f, 1.0f},
    }};

    return palette[groupIndex % palette.size()];
}

std::array<float, 4> scaledRgbColor(const std::array<float, 4>& color, float scale)
{
    return {
        std::clamp(color[0] * scale, 0.0f, 1.0f),
        std::clamp(color[1] * scale, 0.0f, 1.0f),
        std::clamp(color[2] * scale, 0.0f, 1.0f),
        color[3],
    };
}

struct GpuNodeRange {
    uint32_t triangleIndexOffset = 0;
    uint32_t triangleIndexCount = 0;
    uint32_t lineIndexOffset = 0;
    uint32_t lineIndexCount = 0;
    uint32_t pointIndexOffset = 0;
    uint32_t pointIndexCount = 0;
    uint32_t pointSpriteIndexOffset = 0;
    uint32_t pointSpriteIndexCount = 0;
};

struct GpuMesh {
    bgfx::VertexBufferHandle vertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle pointSpriteVertexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle triangleIndexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle lineIndexBuffer = BGFX_INVALID_HANDLE;
    bgfx::IndexBufferHandle pointSpriteIndexBuffer = BGFX_INVALID_HANDLE;
    std::vector<GpuNodeRange> nodeRanges;
};

struct PointSpriteVertex {
    std::array<float, 3> position{};
    std::array<float, 2> corner{};
};

struct GroupRenderSettings {
    bool visible = true;
    bool showSolidMesh = true;
    bool showTriangles = true;
    bool showVertices = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    float vertexSizeScale = 1.0f;
    std::array<float, 3> center{};
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
    std::array<float, 4> color{};
};

std::array<float, 3> nodeCenter(const woby::ObjMesh& mesh, const woby::ObjNode& node)
{
    if (node.indexCount == 0u || mesh.indices.empty() || mesh.vertices.empty()) {
        return mesh.bounds.center;
    }

    std::array<float, 3> minPosition = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> maxPosition = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    const uint32_t endIndex = node.indexOffset + node.indexCount;
    for (uint32_t index = node.indexOffset; index < endIndex; ++index) {
        const auto& position = mesh.vertices[mesh.indices[index]].position;
        for (size_t axis = 0; axis < 3u; ++axis) {
            minPosition[axis] = std::min(minPosition[axis], position[axis]);
            maxPosition[axis] = std::max(maxPosition[axis], position[axis]);
        }
    }

    return {
        (minPosition[0] + maxPosition[0]) * 0.5f,
        (minPosition[1] + maxPosition[1]) * 0.5f,
        (minPosition[2] + maxPosition[2]) * 0.5f,
    };
}

std::vector<GroupRenderSettings> createGroupRenderSettings(const woby::ObjMesh& mesh)
{
    std::vector<GroupRenderSettings> settings;
    settings.reserve(mesh.nodes.size());

    for (size_t groupIndex = 0; groupIndex < mesh.nodes.size(); ++groupIndex) {
        GroupRenderSettings group;
        group.color = objGroupColor(groupIndex);
        group.center = nodeCenter(mesh, mesh.nodes[groupIndex]);
        settings.push_back(group);
    }

    return settings;
}

size_t countEnabledGroupSettings(
    const std::vector<GroupRenderSettings>& settings,
    bool GroupRenderSettings::*field)
{
    size_t enabledCount = 0;
    for (const auto& group : settings) {
        if (group.*field) {
            ++enabledCount;
        }
    }

    return enabledCount;
}

void setAllGroupSettings(
    std::vector<GroupRenderSettings>& settings,
    bool GroupRenderSettings::*field,
    bool enabled)
{
    for (auto& group : settings) {
        group.*field = enabled;
    }
}

void resetGroupTransform(GroupRenderSettings& settings)
{
    settings.scale = 1.0f;
    settings.opacity = 1.0f;
    settings.translation = {};
    settings.rotationDegrees = {};
}

std::array<float, 4> groupColor(const GroupRenderSettings& settings, float rgbScale)
{
    auto color = scaledRgbColor(settings.color, rgbScale);
    color[3] = std::clamp(settings.opacity, minGroupOpacity, maxGroupOpacity);
    return color;
}

void groupTransform(const GroupRenderSettings& settings, float* model)
{
    float toOrigin[16];
    float transformed[16];
    bx::mtxTranslate(
        toOrigin,
        -settings.center[0],
        -settings.center[1],
        -settings.center[2]);
    bx::mtxSRT(
        transformed,
        settings.scale,
        settings.scale,
        settings.scale,
        bx::toRad(settings.rotationDegrees[0]),
        bx::toRad(settings.rotationDegrees[1]),
        bx::toRad(settings.rotationDegrees[2]),
        settings.center[0] + settings.translation[0],
        settings.center[1] + settings.translation[1],
        settings.center[2] + settings.translation[2]);
    bx::mtxMul(model, transformed, toOrigin);
}

std::vector<uint32_t> buildLineIndices(const std::vector<uint32_t>& triangleIndices)
{
    std::vector<uint32_t> lineIndices;
    lineIndices.reserve((triangleIndices.size() / 3u) * 6u);

    for (size_t index = 0; index + 2u < triangleIndices.size(); index += 3u) {
        const uint32_t a = triangleIndices[index + 0u];
        const uint32_t b = triangleIndices[index + 1u];
        const uint32_t c = triangleIndices[index + 2u];
        lineIndices.push_back(a);
        lineIndices.push_back(b);
        lineIndices.push_back(b);
        lineIndices.push_back(c);
        lineIndices.push_back(c);
        lineIndices.push_back(a);
    }

    return lineIndices;
}

uint32_t appendPointIndicesForRange(
    const std::vector<uint32_t>& triangleIndices,
    uint32_t triangleIndexOffset,
    uint32_t triangleIndexCount,
    std::vector<uint32_t>& pointIndices)
{
    const uint32_t pointOffset = static_cast<uint32_t>(pointIndices.size());
    std::unordered_set<uint32_t> seen;
    seen.reserve(triangleIndexCount);

    const uint32_t endIndex = triangleIndexOffset + triangleIndexCount;
    for (uint32_t index = triangleIndexOffset; index < endIndex; ++index) {
        const uint32_t vertexIndex = triangleIndices[index];
        if (seen.insert(vertexIndex).second) {
            pointIndices.push_back(vertexIndex);
        }
    }

    return static_cast<uint32_t>(pointIndices.size()) - pointOffset;
}

void buildPointSprites(
    const woby::ObjMesh& mesh,
    const std::vector<uint32_t>& pointIndices,
    std::vector<PointSpriteVertex>& vertices,
    std::vector<uint32_t>& indices)
{
    constexpr std::array<std::array<float, 2>, 4> corners = {{
        {-0.5f, -0.5f},
        {0.5f, -0.5f},
        {0.5f, 0.5f},
        {-0.5f, 0.5f},
    }};

    vertices.reserve(pointIndices.size() * corners.size());
    indices.reserve(pointIndices.size() * 6u);

    for (uint32_t pointIndex : pointIndices) {
        const uint32_t vertexBase = static_cast<uint32_t>(vertices.size());
        const auto& sourceVertex = mesh.vertices[pointIndex];
        for (const auto& corner : corners) {
            PointSpriteVertex vertex;
            vertex.position = sourceVertex.position;
            vertex.corner = corner;
            vertices.push_back(vertex);
        }

        indices.push_back(vertexBase + 0u);
        indices.push_back(vertexBase + 1u);
        indices.push_back(vertexBase + 2u);
        indices.push_back(vertexBase + 0u);
        indices.push_back(vertexBase + 2u);
        indices.push_back(vertexBase + 3u);
    }
}

GpuMesh createGpuMesh(
    const woby::ObjMesh& mesh,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout)
{
    GpuMesh gpuMesh;

    gpuMesh.vertexBuffer = bgfx::createVertexBuffer(
        bgfx::copy(mesh.vertices.data(), static_cast<uint32_t>(mesh.vertices.size() * sizeof(woby::Vertex))),
        meshLayout);

    gpuMesh.triangleIndexBuffer = bgfx::createIndexBuffer(
        bgfx::copy(mesh.indices.data(), static_cast<uint32_t>(mesh.indices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    const std::vector<uint32_t> lineIndices = buildLineIndices(mesh.indices);
    gpuMesh.lineIndexBuffer = bgfx::createIndexBuffer(
        bgfx::copy(lineIndices.data(), static_cast<uint32_t>(lineIndices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    std::vector<uint32_t> pointIndices;
    const auto& nodes = mesh.nodes;
    gpuMesh.nodeRanges.reserve(nodes.size());
    for (const auto& node : nodes) {
        GpuNodeRange range;
        range.triangleIndexOffset = node.indexOffset;
        range.triangleIndexCount = node.indexCount;
        range.lineIndexOffset = (node.indexOffset / 3u) * 6u;
        range.lineIndexCount = (node.indexCount / 3u) * 6u;
        range.pointIndexOffset = static_cast<uint32_t>(pointIndices.size());
        range.pointIndexCount = appendPointIndicesForRange(
            mesh.indices,
            node.indexOffset,
            node.indexCount,
            pointIndices);
        range.pointSpriteIndexOffset = range.pointIndexOffset * 6u;
        range.pointSpriteIndexCount = range.pointIndexCount * 6u;
        gpuMesh.nodeRanges.push_back(range);
    }

    std::vector<PointSpriteVertex> pointSpriteVertices;
    std::vector<uint32_t> pointSpriteIndices;
    buildPointSprites(mesh, pointIndices, pointSpriteVertices, pointSpriteIndices);

    gpuMesh.pointSpriteVertexBuffer = bgfx::createVertexBuffer(
        bgfx::copy(
            pointSpriteVertices.data(),
            static_cast<uint32_t>(pointSpriteVertices.size() * sizeof(PointSpriteVertex))),
        pointSpriteLayout);
    gpuMesh.pointSpriteIndexBuffer = bgfx::createIndexBuffer(
        bgfx::copy(
            pointSpriteIndices.data(),
            static_cast<uint32_t>(pointSpriteIndices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    return gpuMesh;
}

void destroyGpuMesh(GpuMesh& mesh)
{
    if (bgfx::isValid(mesh.pointSpriteIndexBuffer)) {
        bgfx::destroy(mesh.pointSpriteIndexBuffer);
    }
    if (bgfx::isValid(mesh.lineIndexBuffer)) {
        bgfx::destroy(mesh.lineIndexBuffer);
    }
    if (bgfx::isValid(mesh.triangleIndexBuffer)) {
        bgfx::destroy(mesh.triangleIndexBuffer);
    }
    if (bgfx::isValid(mesh.vertexBuffer)) {
        bgfx::destroy(mesh.vertexBuffer);
    }
    if (bgfx::isValid(mesh.pointSpriteVertexBuffer)) {
        bgfx::destroy(mesh.pointSpriteVertexBuffer);
    }

    mesh.pointSpriteIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.lineIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.triangleIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.vertexBuffer = BGFX_INVALID_HANDLE;
    mesh.pointSpriteVertexBuffer = BGFX_INVALID_HANDLE;
    mesh.nodeRanges.clear();
}

uint64_t renderState(
    uint64_t depthTest,
    bool writeDepth,
    const std::array<float, 4>& color,
    uint64_t primitiveState)
{
    uint64_t state = BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | depthTest
        | BGFX_STATE_MSAA
        | primitiveState;

    if (writeDepth && color[3] >= 0.999f) {
        state |= BGFX_STATE_WRITE_Z;
    }
    if (color[3] < 0.999f) {
        state |= BGFX_STATE_BLEND_FUNC(
            BGFX_STATE_BLEND_SRC_ALPHA,
            BGFX_STATE_BLEND_INV_SRC_ALPHA);
    }

    return state;
}

uint32_t vertexPointSize(float masterSize, float groupScale)
{
    const float scaledSize = std::clamp(
        masterSize * groupScale,
        minVertexPointSize,
        maxVertexPointSize);
    return static_cast<uint32_t>(std::lround(scaledSize));
}

void setLastItemTooltip(const char* text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

ImVec4 toImVec4(const std::array<float, 4>& color)
{
    return ImVec4(color[0], color[1], color[2], color[3]);
}

void drawMixedCheckboxMark()
{
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const float squareSize = ImGui::GetFrameHeight();
    const float padding = std::max(2.0f, squareSize * 0.25f);
    const float centerY = itemMin.y + squareSize * 0.5f;
    const ImVec2 lineStart(itemMin.x + padding, centerY);
    const ImVec2 lineEnd(itemMin.x + squareSize - padding, centerY);
    ImGui::GetWindowDrawList()->AddLine(
        lineStart,
        lineEnd,
        ImGui::GetColorU32(ImGuiCol_CheckMark),
        2.0f);
}

bool drawTriStateMasterCheckbox(const char* label, size_t enabledCount, size_t totalCount)
{
    const bool allEnabled = totalCount > 0u && enabledCount == totalCount;
    const bool mixed = enabledCount > 0u && enabledCount < totalCount;
    bool checkboxValue = allEnabled;

    if (totalCount == 0u) {
        ImGui::BeginDisabled();
    }
    const bool changed = ImGui::Checkbox(label, &checkboxValue);
    if (mixed) {
        drawMixedCheckboxMark();
    }
    if (totalCount == 0u) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s enabled for %zu of %zu groups", label, enabledCount, totalCount);
    }

    return changed;
}

void submitTriangleRange(
    const GpuMesh& mesh,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const float* model,
    const std::array<float, 4>& color,
    uint32_t indexOffset,
    uint32_t indexCount)
{
    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(mesh.triangleIndexBuffer) || indexCount == 0) {
        return;
    }

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.triangleIndexBuffer, indexOffset, indexCount);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_LESS, true, color, 0u));
    bgfx::submit(sceneView, program);
}

void submitColorRange(
    const GpuMesh& mesh,
    bgfx::IndexBufferHandle indexBuffer,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const float* model,
    const std::array<float, 4>& color,
    uint64_t primitiveState,
    uint32_t indexOffset,
    uint32_t indexCount)
{
    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(indexBuffer) || indexCount == 0) {
        return;
    }

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(indexBuffer, indexOffset, indexCount);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_LEQUAL, false, color, primitiveState));
    bgfx::submit(sceneView, program);
}

void submitPointSpriteRange(
    const GpuMesh& mesh,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
    const float* model,
    const std::array<float, 4>& color,
    float pointSize,
    uint32_t viewWidth,
    uint32_t viewHeight,
    uint32_t indexOffset,
    uint32_t indexCount)
{
    if (!bgfx::isValid(mesh.pointSpriteVertexBuffer)
        || !bgfx::isValid(mesh.pointSpriteIndexBuffer)
        || indexCount == 0) {
        return;
    }

    const std::array<float, 4> pointParams = {
        pointSize,
        static_cast<float>(std::max(viewWidth, 1u)),
        static_cast<float>(std::max(viewHeight, 1u)),
        0.0f,
    };

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setUniform(pointParamsUniform, pointParams.data());
    bgfx::setVertexBuffer(0, mesh.pointSpriteVertexBuffer);
    bgfx::setIndexBuffer(mesh.pointSpriteIndexBuffer, indexOffset, indexCount);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_LEQUAL, true, color, 0u));
    bgfx::submit(sceneView, program);
}

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

        const auto cpuMesh = woby::loadObjMesh(modelPath);
        const auto layout = meshVertexLayout();
        const auto pointLayout = pointSpriteVertexLayout();
        GpuMesh gpuMesh = createGpuMesh(cpuMesh, layout, pointLayout);
        bgfx::ProgramHandle meshProgram = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
        bgfx::ProgramHandle colorProgram = woby::loadProgram(assets, "vs_color.bin", "fs_color.bin");
        bgfx::ProgramHandle pointSpriteProgram = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_point_sprite.bin");
        bgfx::UniformHandle colorUniform = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
        bgfx::UniformHandle pointParamsUniform = bgfx::createUniform("u_pointParams", bgfx::UniformType::Vec4);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForOther(window.get())) {
            throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed.");
        }
        woby::imgui_bgfx::init(assets, imguiView);

        bool running = true;
        std::vector<GroupRenderSettings> groupSettings = createGroupRenderSettings(cpuMesh);
        float masterVertexPointSize = 4.0f;
        woby::SceneCamera camera = woby::frameCameraBounds(cpuMesh.bounds);
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
                    camera = woby::frameCameraBounds(cpuMesh.bounds);
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

            const auto& bounds = cpuMesh.bounds;
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

            for (size_t nodeIndex = 0; nodeIndex < gpuMesh.nodeRanges.size(); ++nodeIndex) {
                const auto& settings = groupSettings[nodeIndex];
                if (!settings.visible) {
                    continue;
                }

                float model[16];
                groupTransform(settings, model);
                const auto& range = gpuMesh.nodeRanges[nodeIndex];
                if (settings.showSolidMesh) {
                    submitTriangleRange(
                        gpuMesh,
                        meshProgram,
                        colorUniform,
                        model,
                        groupColor(settings, 1.0f),
                        range.triangleIndexOffset,
                        range.triangleIndexCount);
                }
                if (settings.showTriangles) {
                    submitColorRange(
                        gpuMesh,
                        gpuMesh.lineIndexBuffer,
                        colorProgram,
                        colorUniform,
                        model,
                        groupColor(settings, 1.25f),
                        BGFX_STATE_PT_LINES,
                        range.lineIndexOffset,
                        range.lineIndexCount);
                }
                if (settings.showVertices) {
                    const uint32_t pointSize = vertexPointSize(
                        masterVertexPointSize,
                        settings.vertexSizeScale);
                    submitPointSpriteRange(
                        gpuMesh,
                        pointSpriteProgram,
                        colorUniform,
                        pointParamsUniform,
                        model,
                        groupColor(settings, 1.5f),
                        static_cast<float>(pointSize),
                        width,
                        height,
                        range.pointSpriteIndexOffset,
                        range.pointSpriteIndexCount);
                }
            }

            bgfx::dbgTextClear();

            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::Begin("Viewer");
            ImGui::Text("Renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
            ImGui::Text("File: %s", modelPath.string().c_str());
            ImGui::Text("FPS: %.1f", fps);
            ImGui::Text("Vertices: %zu", cpuMesh.vertices.size());
            ImGui::Text("Triangles: %zu", cpuMesh.indices.size() / 3u);
            const size_t groupCount = groupSettings.size();
            const size_t solidMeshCount = countEnabledGroupSettings(
                groupSettings,
                &GroupRenderSettings::showSolidMesh);
            if (drawTriStateMasterCheckbox("Solid mesh", solidMeshCount, groupCount)) {
                setAllGroupSettings(
                    groupSettings,
                    &GroupRenderSettings::showSolidMesh,
                    solidMeshCount != groupCount);
            }
            const size_t triangleCount = countEnabledGroupSettings(
                groupSettings,
                &GroupRenderSettings::showTriangles);
            if (drawTriStateMasterCheckbox("Triangles", triangleCount, groupCount)) {
                setAllGroupSettings(
                    groupSettings,
                    &GroupRenderSettings::showTriangles,
                    triangleCount != groupCount);
            }
            const size_t vertexCount = countEnabledGroupSettings(
                groupSettings,
                &GroupRenderSettings::showVertices);
            if (drawTriStateMasterCheckbox("Vertices", vertexCount, groupCount)) {
                setAllGroupSettings(
                    groupSettings,
                    &GroupRenderSettings::showVertices,
                    vertexCount != groupCount);
            }
            ImGui::SetNextItemWidth(260.0f);
            ImGui::SliderFloat(
                "Vertex size",
                &masterVertexPointSize,
                minVertexPointSize,
                maxVertexPointSize,
                "%.0f px");
            setLastItemTooltip("Base vertex point size for all groups");
            const float translationSpeed = std::max(cpuMesh.bounds.radius * 0.005f, 0.01f);
            if (ImGui::TreeNode("Groups")) {
                for (size_t nodeIndex = 0; nodeIndex < cpuMesh.nodes.size(); ++nodeIndex) {
                    const auto& node = cpuMesh.nodes[nodeIndex];
                    auto& settings = groupSettings[nodeIndex];
                    const std::string label = node.name + "##node_" + std::to_string(nodeIndex);
                    ImGui::Checkbox(label.c_str(), &settings.visible);
                    setLastItemTooltip("Show group");
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%u triangles)", node.indexCount / 3u);
                    ImGui::SameLine();
                    ImGui::Checkbox(("S##solid_node_" + std::to_string(nodeIndex)).c_str(), &settings.showSolidMesh);
                    setLastItemTooltip("Solid mesh for this group");
                    ImGui::SameLine();
                    ImGui::Checkbox(("T##triangles_node_" + std::to_string(nodeIndex)).c_str(), &settings.showTriangles);
                    setLastItemTooltip("Triangles for this group");
                    ImGui::SameLine();
                    ImGui::Checkbox(("V##vertices_node_" + std::to_string(nodeIndex)).c_str(), &settings.showVertices);
                    setLastItemTooltip("Vertices for this group");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140.0f);
                    ImGui::DragFloat(
                        ("##vertex_size_node_" + std::to_string(nodeIndex)).c_str(),
                        &settings.vertexSizeScale,
                        0.02f,
                        minVertexSizeScale,
                        maxVertexSizeScale,
                        "%.2fx");
                    setLastItemTooltip("Vertex size multiplier for this group");
                    ImGui::SameLine();
                    const std::string colorButtonId = "##color_node_" + std::to_string(nodeIndex);
                    const std::string colorPopupId = "Color##color_popup_node_" + std::to_string(nodeIndex);
                    if (ImGui::ColorButton(
                            colorButtonId.c_str(),
                            toImVec4(groupColor(settings, 1.0f)),
                            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
                            ImVec2(18.0f, 18.0f))) {
                        ImGui::OpenPopup(colorPopupId.c_str());
                    }
                    setLastItemTooltip("Color for this group");
                    if (ImGui::BeginPopup(colorPopupId.c_str())) {
                        ImGui::TextUnformatted(node.name.c_str());
                        ImGui::ColorPicker3(
                            ("##color_picker_node_" + std::to_string(nodeIndex)).c_str(),
                            settings.color.data());
                        if (ImGui::Button(("Reset##reset_color_node_" + std::to_string(nodeIndex)).c_str())) {
                            settings.color = objGroupColor(nodeIndex);
                        }
                        ImGui::EndPopup();
                    }
                    if (ImGui::TreeNode(("Transform##transform_node_" + std::to_string(nodeIndex)).c_str())) {
                        ImGui::SetNextItemWidth(220.0f);
                        ImGui::DragFloat(
                            ("Scale##scale_node_" + std::to_string(nodeIndex)).c_str(),
                            &settings.scale,
                            0.01f,
                            minGroupScale,
                            maxGroupScale,
                            "%.2fx");
                        setLastItemTooltip("Uniform scale for this group");
                        ImGui::SetNextItemWidth(260.0f);
                        ImGui::DragFloat3(
                            ("Move##translation_node_" + std::to_string(nodeIndex)).c_str(),
                            settings.translation.data(),
                            translationSpeed);
                        setLastItemTooltip("Position offset for this group");
                        ImGui::SetNextItemWidth(260.0f);
                        ImGui::DragFloat3(
                            ("Rotate##rotation_node_" + std::to_string(nodeIndex)).c_str(),
                            settings.rotationDegrees.data(),
                            1.0f,
                            -180.0f,
                            180.0f,
                            "%.0f deg");
                        setLastItemTooltip("Rotation in degrees for this group");
                        ImGui::SetNextItemWidth(220.0f);
                        ImGui::SliderFloat(
                            ("Opacity##opacity_node_" + std::to_string(nodeIndex)).c_str(),
                            &settings.opacity,
                            minGroupOpacity,
                            maxGroupOpacity,
                            "%.2f");
                        setLastItemTooltip("Opacity for this group");
                        if (ImGui::Button(("Reset##reset_transform_node_" + std::to_string(nodeIndex)).c_str())) {
                            resetGroupTransform(settings);
                        }
                        ImGui::TreePop();
                    }
                }
                ImGui::TreePop();
            }
            ImGui::End();
            ImGui::Render();
            woby::imgui_bgfx::render(ImGui::GetDrawData());

            bgfx::frame();
        }

        woby::imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        bgfx::destroy(pointParamsUniform);
        bgfx::destroy(colorUniform);
        bgfx::destroy(pointSpriteProgram);
        bgfx::destroy(colorProgram);
        bgfx::destroy(meshProgram);
        destroyGpuMesh(gpuMesh);
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
