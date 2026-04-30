#include "bgfx_helpers.h"
#include "camera.h"
#include "command_line.h"
#include "file_discovery.h"
#include "imgui_bgfx.h"
#include "obj_mesh.h"
#include "scene_file.h"
#include "ui_operations.h"
#include "ui_state.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_main.h>
#include <bgfx/bgfx.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t resetFlags = BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4;
constexpr bgfx::ViewId sceneView = 0;
constexpr bgfx::ViewId helperView = 1;
constexpr bgfx::ViewId imguiView = 255;
constexpr float defaultScenePaneHeight = 185.0f;
constexpr float minSceneViewportWidth = 160.0f;
constexpr float viewerPaneBackgroundRed = 0.20f;
constexpr float viewerPaneBackgroundGreen = 0.21f;
constexpr float viewerPaneBackgroundBlue = 0.22f;
constexpr float viewerPaneBackgroundAlpha = 0.76f;
constexpr float popupBackgroundRed = 0.20f;
constexpr float popupBackgroundGreen = 0.21f;
constexpr float popupBackgroundBlue = 0.22f;
constexpr float popupBackgroundAlpha = 0.88f;
constexpr float appFontSize = 15.0f;
constexpr float vertexHoverMinRadius = 3.0f;
constexpr float vertexHoverEpsilon = 0.000001f;
constexpr const char* appFontFilename = "RobotoMonoNerdFont-Regular.ttf";
constexpr ImWchar appFontGlyphRanges[] = {
    0x0020,
    0x00ff,
    0xf04b,
    0xf04b,
    0xf00d,
    0xf00d,
    0xf192,
    0xf192,
    0xf068,
    0xf068,
    0xf06e,
    0xf070,
    0xf0b2,
    0xf0b2,
    0xf1b2,
    0xf1b2,
    0xf02c1,
    0xf02c1,
    0xf0b43,
    0xf0b43,
    0xf0930,
    0xf0930,
    0xeba0,
    0xeba0,
    0xed75,
    0xed75,
    0xed95,
    0xed95,
    0xea7f,
    0xea7f,
    0,
};
constexpr const char* addObjFileIcon = "\xee\xa9\xbf";
constexpr const char* openSceneIcon = "\xee\xb6\x95";
constexpr const char* saveSceneIcon = "\xee\xb5\xb5";
constexpr const char* removeFileIcon = "\xef\x80\x8d";
constexpr const char* solidMeshIcon = "\xef\x86\xb2";
constexpr const char* trianglesIcon = "\xef\x81\x8b";
constexpr const char* verticesIcon = "\xef\x86\x92";
constexpr const char* transformIcon = "\xef\x82\xb2";
constexpr const char* visibleIcon = "\xef\x81\xae";
constexpr const char* hiddenIcon = "\xef\x81\xb0";
constexpr const char* mixedStateIcon = "\xef\x81\xa8";
constexpr const char* originIcon = "\xf3\xb0\xad\x83";
constexpr const char* gridIcon = "\xf3\xb0\x8b\x81";
constexpr const char* viewerPanePinnedIcon = "\xee\xae\xa0";
constexpr const char* viewerPaneUnpinnedIcon = "\xf3\xb0\xa4\xb0";
constexpr float renderModeButtonSize = 26.0f;
constexpr float groupVertexSizeControlWidth = 70.0f;
constexpr float viewerPaneWidthPadding = 20.0f;
constexpr float viewerPaneTogglePaneMargin = 6.0f;
constexpr float toastDurationSeconds = 3.0f;
constexpr float toastMargin = 12.0f;

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

void loadAppFont(const std::filesystem::path& assets)
{
    const std::filesystem::path fontPath = assets / "fonts" / appFontFilename;
    if (!std::filesystem::exists(fontPath)) {
        throw std::runtime_error("App font not found: " + fontPath.string());
    }

    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = io.Fonts->AddFontFromFileTTF(
        fontPath.string().c_str(),
        appFontSize,
        nullptr,
        appFontGlyphRanges);
    if (font == nullptr) {
        throw std::runtime_error("Failed to load app font: " + fontPath.string());
    }

    io.FontDefault = font;
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

bgfx::VertexLayout helperLineVertexLayout()
{
    bgfx::VertexLayout layout;
    layout
        .begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .end();
    return layout;
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
    std::vector<uint32_t> pointVertexIndices;
};

struct PointSpriteVertex {
    std::array<float, 3> position{};
    std::array<float, 2> corner{};
};

struct HelperLineVertex {
    std::array<float, 3> position{};
};

using GroupRenderSettings = woby::UiGroupState;
using FileRenderSettings = woby::UiFileSettings;
using LoadedObjFile = woby::UiFileState;

struct LoadedObjRuntime {
    GpuMesh gpuMesh;
};

struct LoadedObjFileWithRuntime {
    LoadedObjFile file;
    LoadedObjRuntime runtime;
};

struct ObjFileDialogState {
    std::mutex mutex;
    std::vector<std::filesystem::path> pendingPaths;
    std::string status;
    uint64_t statusVersion = 0;
    bool open = false;
};

struct SceneFileDialogState {
    std::mutex mutex;
    std::optional<std::filesystem::path> pendingOpenPath;
    std::optional<std::filesystem::path> pendingSavePath;
    std::string status;
    uint64_t statusVersion = 0;
    bool openDialogOpen = false;
    bool saveDialogOpen = false;
};

struct DragDropState {
    std::vector<std::filesystem::path> batchPaths;
    std::vector<std::filesystem::path> pendingPaths;
    bool active = false;
};

struct ToastMessage {
    std::string text;
    std::chrono::steady_clock::time_point startedAt{};
};

struct MousePosition {
    float x = 0.0f;
    float y = 0.0f;
};

struct ProjectedVertex {
    bool visible = false;
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
};

struct HoveredVertex {
    std::array<float, 3> localPosition{};
    std::array<float, 3> transformedPosition{};
    float depth = 0.0f;
    float distanceSquared = 0.0f;
};

std::array<float, 4> groupColor(
    const GroupRenderSettings& settings,
    float rgbScale,
    float opacityScale = 1.0f)
{
    auto color = scaledRgbColor(settings.color, rgbScale);
    color[3] = std::clamp(settings.opacity * opacityScale, woby::minGroupOpacity, woby::maxGroupOpacity);
    return color;
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
    gpuMesh.pointVertexIndices = std::move(pointIndices);

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
    mesh.pointVertexIndices.clear();
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
        woby::minVertexPointSize,
        woby::maxVertexPointSize);
    return static_cast<uint32_t>(std::lround(scaledSize));
}

MousePosition mousePositionInPixels(SDL_Window* window)
{
    float mouseWindowX = 0.0f;
    float mouseWindowY = 0.0f;
    SDL_GetMouseState(&mouseWindowX, &mouseWindowY);

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);

    uint32_t drawableWidth = 0;
    uint32_t drawableHeight = 0;
    getDrawableSize(window, drawableWidth, drawableHeight);

    const float widthScale = static_cast<float>(drawableWidth) / static_cast<float>(std::max(windowWidth, 1));
    const float heightScale = static_cast<float>(drawableHeight) / static_cast<float>(std::max(windowHeight, 1));
    return {
        mouseWindowX * widthScale,
        mouseWindowY * heightScale,
    };
}

std::array<float, 4> transformPoint4(const float* matrix, const std::array<float, 4>& point)
{
    return {
        matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12] * point[3],
        matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13] * point[3],
        matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14] * point[3],
        matrix[3] * point[0] + matrix[7] * point[1] + matrix[11] * point[2] + matrix[15] * point[3],
    };
}

std::array<float, 3> transformPosition(const float* matrix, const std::array<float, 3>& position)
{
    const auto transformed = transformPoint4(matrix, {position[0], position[1], position[2], 1.0f});
    if (std::abs(transformed[3]) <= vertexHoverEpsilon) {
        return {transformed[0], transformed[1], transformed[2]};
    }

    return {
        transformed[0] / transformed[3],
        transformed[1] / transformed[3],
        transformed[2] / transformed[3],
    };
}

ProjectedVertex projectPosition(
    const std::array<float, 3>& position,
    const float* model,
    const float* view,
    const float* projection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth)
{
    const auto world = transformPoint4(model, {position[0], position[1], position[2], 1.0f});
    const auto eye = transformPoint4(view, world);
    const auto clip = transformPoint4(projection, eye);
    if (clip[3] <= vertexHoverEpsilon) {
        return {};
    }

    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    const float ndcZ = clip[2] / clip[3];
    const float minDepth = homogeneousDepth ? -1.0f : 0.0f;
    if (ndcX < -1.0f
        || ndcX > 1.0f
        || ndcY < -1.0f
        || ndcY > 1.0f
        || ndcZ < minDepth
        || ndcZ > 1.0f) {
        return {};
    }

    ProjectedVertex projected;
    projected.visible = true;
    projected.x = (ndcX * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
    projected.y = (0.5f - ndcY * 0.5f) * static_cast<float>(viewportHeight);
    projected.depth = ndcZ;
    return projected;
}

void updateHoveredVertexCandidate(
    const std::array<float, 3>& localPosition,
    const std::array<float, 3>& transformedPosition,
    const ProjectedVertex& projected,
    const MousePosition& mouse,
    float hoverRadius,
    std::optional<HoveredVertex>& hoveredVertex)
{
    const float deltaX = projected.x - mouse.x;
    const float deltaY = projected.y - mouse.y;
    const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
    if (distanceSquared > hoverRadius * hoverRadius) {
        return;
    }

    if (hoveredVertex.has_value()
        && (projected.depth > hoveredVertex->depth
            || (projected.depth == hoveredVertex->depth
                && distanceSquared >= hoveredVertex->distanceSquared))) {
        return;
    }

    hoveredVertex = HoveredVertex{
        localPosition,
        transformedPosition,
        projected.depth,
        distanceSquared,
    };
}

std::optional<HoveredVertex> findHoveredVertex(
    const std::vector<LoadedObjFile>& files,
    const std::vector<LoadedObjRuntime>& runtimes,
    const MousePosition& mouse,
    float masterVertexPointSize,
    const float* view,
    const float* projection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth)
{
    std::optional<HoveredVertex> hoveredVertex;
    const size_t fileCount = std::min(files.size(), runtimes.size());
    for (size_t fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
        const auto& file = files[fileIndex];
        const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
        if (!file.fileSettings.visible || file.fileSettings.opacity <= vertexHoverEpsilon) {
            continue;
        }

        float fileModel[16];
        woby::fileTransformMatrix(file.fileSettings, fileModel);
        for (size_t nodeIndex = 0; nodeIndex < gpuMesh.nodeRanges.size(); ++nodeIndex) {
            const auto& settings = file.groupSettings[nodeIndex];
            if (!settings.visible
                || !settings.showVertices
                || settings.opacity <= vertexHoverEpsilon) {
                continue;
            }

            float groupModel[16];
            float model[16];
            woby::groupTransformMatrix(settings, groupModel);
            bx::mtxMul(model, fileModel, groupModel);

            const uint32_t pointSize = vertexPointSize(
                masterVertexPointSize,
                file.vertexSizeScale * settings.vertexSizeScale);
            const float hoverRadius = std::max(
                static_cast<float>(pointSize) * 0.5f,
                vertexHoverMinRadius);
            const auto& range = gpuMesh.nodeRanges[nodeIndex];
            const uint32_t endIndex = std::min(
                range.pointIndexOffset + range.pointIndexCount,
                static_cast<uint32_t>(gpuMesh.pointVertexIndices.size()));
            for (uint32_t index = range.pointIndexOffset; index < endIndex; ++index) {
                const uint32_t vertexIndex = gpuMesh.pointVertexIndices[index];
                if (vertexIndex >= file.mesh.vertices.size()) {
                    continue;
                }

                const auto& localPosition = file.mesh.vertices[vertexIndex].position;
                const auto projected = projectPosition(
                    localPosition,
                    model,
                    view,
                    projection,
                    viewportWidth,
                    viewportHeight,
                    homogeneousDepth);
                if (!projected.visible) {
                    continue;
                }

                updateHoveredVertexCandidate(
                    localPosition,
                    transformPosition(model, localPosition),
                    projected,
                    mouse,
                    hoverRadius,
                    hoveredVertex);
            }
        }
    }

    return hoveredVertex;
}

std::string fileDisplayName(const std::filesystem::path& path)
{
    const auto filename = path.filename().string();
    if (!filename.empty()) {
        return filename;
    }

    return path.string();
}

std::filesystem::path normalizedPath(const std::filesystem::path& path)
{
    return std::filesystem::absolute(path).lexically_normal();
}

std::string appWindowTitle(
    const std::optional<std::filesystem::path>& currentScenePath,
    bool isDirty)
{
    std::string title = "woby - ";
    title += currentScenePath.has_value()
        ? fileDisplayName(currentScenePath.value())
        : "untitled";
    if (isDirty) {
        title += '*';
    }

    return title;
}

void updateAppWindowTitle(
    SDL_Window* window,
    const std::optional<std::filesystem::path>& currentScenePath,
    bool isDirty)
{
    SDL_SetWindowTitle(window, appWindowTitle(currentScenePath, isDirty).c_str());
}

void setLastItemTooltip(const char* text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

void drawClippedTextItem(const char* id, const char* text, float width)
{
    const float itemWidth = std::max(width, 1.0f);
    const float itemHeight = ImGui::GetFrameHeight();
    ImGui::InvisibleButton(id, ImVec2(itemWidth, itemHeight));

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    const ImVec2 textPosition(itemMin.x, itemMin.y + style.FramePadding.y);
    const ImVec4 clipRect(itemMin.x, itemMin.y, itemMax.x, itemMax.y);

    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        ImGui::GetFontSize(),
        textPosition,
        ImGui::GetColorU32(ImGuiCol_Text),
        text,
        nullptr,
        0.0f,
        &clipRect);
}

ImVec4 toImVec4(const std::array<float, 4>& color)
{
    return ImVec4(color[0], color[1], color[2], color[3]);
}

void setStyleColor(ImGuiCol colorIndex, float red, float green, float blue, float alpha)
{
    ImVec4 color = ImGui::GetStyleColorVec4(colorIndex);
    color.x = red;
    color.y = green;
    color.z = blue;
    color.w = alpha;
    ImGui::GetStyle().Colors[colorIndex] = color;
}

void configureAppStyle()
{
    ImGui::StyleColorsDark();
    setStyleColor(
        ImGuiCol_WindowBg,
        viewerPaneBackgroundRed,
        viewerPaneBackgroundGreen,
        viewerPaneBackgroundBlue,
        viewerPaneBackgroundAlpha);
    setStyleColor(
        ImGuiCol_ChildBg,
        viewerPaneBackgroundRed,
        viewerPaneBackgroundGreen,
        viewerPaneBackgroundBlue,
        viewerPaneBackgroundAlpha);
    setStyleColor(
        ImGuiCol_PopupBg,
        popupBackgroundRed,
        popupBackgroundGreen,
        popupBackgroundBlue,
        popupBackgroundAlpha);
}

enum class RenderModeState {
    off,
    mixed,
    on,
};

RenderModeState renderModeState(size_t enabledCount, size_t totalCount)
{
    if (totalCount > 0u && enabledCount == totalCount) {
        return RenderModeState::on;
    }
    if (enabledCount > 0u && enabledCount < totalCount) {
        return RenderModeState::mixed;
    }
    return RenderModeState::off;
}

void pushRenderModeButtonColors(RenderModeState state)
{
    const ImVec4 buttonColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const ImVec4 activeColor = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    const ImVec4 activeHoveredColor = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    const ImVec4 mixedColor(0.55f, 0.40f, 0.14f, 0.75f);
    const ImVec4 mixedHoveredColor(0.70f, 0.50f, 0.18f, 0.90f);
    const ImVec4 offTextColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec4 onTextColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    if (state == RenderModeState::on) {
        ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_Text, onTextColor);
        return;
    }
    if (state == RenderModeState::mixed) {
        ImGui::PushStyleColor(ImGuiCol_Button, mixedColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixedHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixedHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_Text, onTextColor);
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_Text, offTextColor);
}

void drawMixedRenderModeMark()
{
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    const float fontSize = ImGui::GetFontSize() * 0.72f;
    const ImVec2 textSize = ImGui::CalcTextSize(mixedStateIcon);
    const ImVec2 position(
        buttonMax.x - textSize.x - 4.0f,
        buttonMax.y - fontSize - 3.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        fontSize,
        position,
        ImGui::GetColorU32(ImGuiCol_Text),
        mixedStateIcon);
}

bool drawRenderModeIconButton(
    const char* id,
    const char* icon,
    const char* tooltip,
    RenderModeState state,
    bool disabled)
{
    const std::string label = std::string(icon) + "##" + id;

    if (disabled) {
        ImGui::BeginDisabled();
    }
    pushRenderModeButtonColors(state);
    const bool changed = ImGui::Button(
        label.c_str(),
        ImVec2(renderModeButtonSize, renderModeButtonSize));
    ImGui::PopStyleColor(4);
    if (state == RenderModeState::mixed) {
        drawMixedRenderModeMark();
    }
    if (disabled) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    return changed;
}

bool drawTriStateMasterIconButton(
    const char* id,
    const char* icon,
    const char* label,
    size_t enabledCount,
    size_t totalCount)
{
    const RenderModeState state = renderModeState(enabledCount, totalCount);
    const std::string tooltip = std::string(label)
        + " enabled for "
        + std::to_string(enabledCount)
        + " of "
        + std::to_string(totalCount)
        + " groups";

    return drawRenderModeIconButton(
        id,
        icon,
        tooltip.c_str(),
        state,
        totalCount == 0u);
}

bool drawVisibilityIconButton(
    const char* id,
    RenderModeState state,
    const char* tooltip,
    bool disabled)
{
    const char* icon = state == RenderModeState::off ? hiddenIcon : visibleIcon;
    const std::string label = std::string("##") + id;

    if (disabled) {
        ImGui::BeginDisabled();
    }
    pushRenderModeButtonColors(state);
    const bool changed = ImGui::Button(
        label.c_str(),
        ImVec2(renderModeButtonSize, renderModeButtonSize));
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    const ImVec2 iconPosition(
        std::floor(buttonMin.x + (buttonMax.x - buttonMin.x - iconSize.x) * 0.5f - 1.0f),
        std::floor(buttonMin.y + (buttonMax.y - buttonMin.y - iconSize.y) * 0.5f));
    ImGui::GetWindowDrawList()->AddText(
        iconPosition,
        ImGui::GetColorU32(ImGuiCol_Text),
        icon);
    ImGui::PopStyleColor(4);
    if (state == RenderModeState::mixed) {
        drawMixedRenderModeMark();
    }
    if (disabled) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    return changed;
}

bool drawTriStateVisibilityButton(
    const char* id,
    const char* label,
    size_t visibleCount,
    size_t totalCount)
{
    const RenderModeState state = renderModeState(visibleCount, totalCount);
    const std::string tooltip = std::string(label)
        + " visibility: "
        + std::to_string(visibleCount)
        + " of "
        + std::to_string(totalCount)
        + " groups shown";

    return drawVisibilityIconButton(
        id,
        state,
        tooltip.c_str(),
        totalCount == 0u);
}

bool drawVisibilityButton(const char* id, bool visible, const char* itemName)
{
    const std::string tooltip = std::string(visible ? "Hide " : "Show ")
        + itemName;

    return drawVisibilityIconButton(
        id,
        visible ? RenderModeState::on : RenderModeState::off,
        tooltip.c_str(),
        false);
}

float renderModeButtonRowWidth()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return renderModeButtonSize * 3.0f + style.ItemSpacing.x * 2.0f;
}

float groupControlStartOffset()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return renderModeButtonRowWidth() * 2.0f
        + style.ItemSpacing.x
        + renderModeButtonSize
        + style.ItemSpacing.x;
}

float transformControlStartOffset()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return groupControlStartOffset()
        + renderModeButtonRowWidth()
        + groupVertexSizeControlWidth
        + style.ItemSpacing.x
        + renderModeButtonSize
        + style.ItemSpacing.x;
}

float minimumViewerPaneWidth()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float groupControlWidth = renderModeButtonRowWidth()
        + groupVertexSizeControlWidth
        + style.ItemSpacing.x
        + renderModeButtonSize
        + style.ItemSpacing.x
        + renderModeButtonSize;

    return groupControlStartOffset()
        + groupControlWidth
        + style.WindowPadding.x * 2.0f
        + style.ScrollbarSize
        + viewerPaneWidthPadding;
}

void drawViewerPaneTogglePane(woby::UiState& state, float windowWidth)
{
    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGui::SetNextWindowPos(
        ImVec2(windowWidth - viewerPaneTogglePaneMargin, viewerPaneTogglePaneMargin),
        ImGuiCond_Always,
        ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    if (ImGui::Begin(
            "##ViewerPaneToggle",
            nullptr,
            ImGuiWindowFlags_NoDecoration
                | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoFocusOnAppearing)) {
        const char* icon = state.viewerPaneVisible ? viewerPanePinnedIcon : viewerPaneUnpinnedIcon;
        const char* tooltip = state.viewerPaneVisible
            ? "Hide left panes (Ctrl+B)"
            : "Show left panes (Ctrl+B)";
        if (drawRenderModeIconButton(
                "toggle_viewer_pane",
                icon,
                tooltip,
                state.viewerPaneVisible ? RenderModeState::on : RenderModeState::off,
                false)) {
            woby::toggleViewerPaneVisible(state);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void pushRenderModeControlHeight()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float paddingY = std::max(
        (renderModeButtonSize - ImGui::GetFontSize()) * 0.5f,
        0.0f);
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(style.FramePadding.x, paddingY));
}

std::string meshCountLine(size_t vertexCount, size_t triangleCount)
{
    return "Vertices: " + std::to_string(vertexCount)
        + "  Triangles: " + std::to_string(triangleCount);
}

void drawMeshCountLine(size_t vertexCount, size_t triangleCount)
{
    const std::string countLine = meshCountLine(vertexCount, triangleCount);
    ImGui::TextUnformatted(countLine.c_str());
}

void drawGroupMasterControls(std::vector<GroupRenderSettings>& settings)
{
    const size_t groupCount = settings.size();
    const size_t solidMeshCount = woby::countEnabledGroupRenderMode(
        settings,
        woby::UiRenderMode::solidMesh);
    if (drawTriStateMasterIconButton(
            "solid_mesh",
            solidMeshIcon,
            "Solid mesh",
            solidMeshCount,
            groupCount)) {
        woby::setAllGroupRenderModes(
            settings,
            woby::UiRenderMode::solidMesh,
            solidMeshCount != groupCount);
    }
    ImGui::SameLine();
    const size_t triangleCount = woby::countEnabledGroupRenderMode(
        settings,
        woby::UiRenderMode::triangles);
    if (drawTriStateMasterIconButton(
            "triangles",
            trianglesIcon,
            "Triangles",
            triangleCount,
            groupCount)) {
        woby::setAllGroupRenderModes(
            settings,
            woby::UiRenderMode::triangles,
            triangleCount != groupCount);
    }
    ImGui::SameLine();
    const size_t vertexCount = woby::countEnabledGroupRenderMode(
        settings,
        woby::UiRenderMode::vertices);
    if (drawTriStateMasterIconButton(
            "vertices",
            verticesIcon,
            "Vertices",
            vertexCount,
            groupCount)) {
        woby::setAllGroupRenderModes(
            settings,
            woby::UiRenderMode::vertices,
            vertexCount != groupCount);
    }
}

void drawGroupControls(
    const woby::ObjNode& node,
    const GpuNodeRange& range,
    LoadedObjFile& file,
    size_t nodeIndex,
    size_t colorIndex,
    float translationSpeed)
{
    ImGui::PushID(static_cast<int>(nodeIndex));
    auto& settings = file.groupSettings[nodeIndex];
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowStartX = ImGui::GetCursorPosX();
    const float controlsStartX = rowStartX + groupControlStartOffset();
    if (drawVisibilityButton("visible", settings.visible, "group")) {
        woby::toggleGroupVisible(file, settings);
    }
    ImGui::SameLine();
    const float textStartX = ImGui::GetCursorPosX();
    const float nameWidth = controlsStartX - textStartX - style.ItemSpacing.x;
    drawClippedTextItem("##name", node.name.c_str(), nameWidth);
    const std::string groupTooltip = node.name
        + "\n"
        + meshCountLine(range.pointIndexCount, node.indexCount / 3u);
    setLastItemTooltip(groupTooltip.c_str());
    ImGui::SameLine(controlsStartX, 0.0f);
    if (drawRenderModeIconButton(
            "solid_mesh",
            solidMeshIcon,
            "Solid mesh for this group",
            settings.showSolidMesh ? RenderModeState::on : RenderModeState::off,
            false)) {
        woby::toggleGroupRenderMode(settings, woby::UiRenderMode::solidMesh);
    }
    ImGui::SameLine();
    if (drawRenderModeIconButton(
            "triangles",
            trianglesIcon,
            "Triangles for this group",
            settings.showTriangles ? RenderModeState::on : RenderModeState::off,
            false)) {
        woby::toggleGroupRenderMode(settings, woby::UiRenderMode::triangles);
    }
    ImGui::SameLine();
    if (drawRenderModeIconButton(
            "vertices",
            verticesIcon,
            "Vertices for this group",
            settings.showVertices ? RenderModeState::on : RenderModeState::off,
            false)) {
        woby::toggleGroupRenderMode(settings, woby::UiRenderMode::vertices);
    }
    ImGui::SameLine(0.0f, 0.0f);
    float vertexSizeScale = settings.vertexSizeScale;
    ImGui::SetNextItemWidth(groupVertexSizeControlWidth);
    pushRenderModeControlHeight();
    if (ImGui::DragFloat(
        "##vertex_size",
        &vertexSizeScale,
        0.02f,
        woby::minVertexSizeScale,
        woby::maxVertexSizeScale,
        "%.2fx")) {
        woby::setGroupVertexSizeScale(settings, vertexSizeScale);
    }
    ImGui::PopStyleVar();
    setLastItemTooltip("Vertex size multiplier for this group");
    ImGui::SameLine();
    if (ImGui::ColorButton(
            "##color",
            toImVec4(groupColor(settings, 1.0f)),
            ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip,
            ImVec2(renderModeButtonSize, renderModeButtonSize))) {
        ImGui::OpenPopup("Color");
    }
    setLastItemTooltip("Color for this group");
    ImGui::SameLine();
    const RenderModeState transformState = woby::groupTransformIsDefault(settings)
        ? RenderModeState::off
        : RenderModeState::on;
    if (drawRenderModeIconButton(
            "transform",
            transformIcon,
            "Transform geometry",
            transformState,
            false)) {
        ImGui::OpenPopup("Transform");
    }
    if (ImGui::BeginPopup("Color")) {
        ImGui::TextUnformatted(node.name.c_str());
        std::array<float, 4> color = settings.color;
        if (ImGui::ColorPicker3("##color_picker", color.data())) {
            woby::setGroupColor(settings, color);
        }
        if (ImGui::Button("Reset")) {
            woby::resetGroupColor(settings, colorIndex);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Transform")) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Transform geometry");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            woby::resetGroupTransform(settings);
        }
        std::array<float, 3> translation = settings.translation;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragFloat3(
            "Move",
            translation.data(),
            translationSpeed)) {
            woby::setGroupTranslation(settings, translation);
        }
        setLastItemTooltip("Position offset for this group");
        std::array<float, 3> rotationDegrees = settings.rotationDegrees;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragFloat3(
            "Rotate",
            rotationDegrees.data(),
            1.0f,
            -180.0f,
            180.0f,
            "%.0f deg")) {
            woby::setGroupRotationDegrees(settings, rotationDegrees);
        }
        setLastItemTooltip("Rotation in degrees for this group");
        float scale = settings.scale;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragFloat(
            "Scale",
            &scale,
            0.01f,
            woby::minGroupScale,
            woby::maxGroupScale,
            "%.2fx")) {
            woby::setGroupScale(settings, scale);
        }
        setLastItemTooltip("Uniform scale for this group");
        float opacity = settings.opacity;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderFloat(
            "Opacity",
            &opacity,
            woby::minGroupOpacity,
            woby::maxGroupOpacity,
            "%.2f")) {
            woby::setGroupOpacity(settings, opacity);
        }
        setLastItemTooltip("Opacity for this group");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void drawFileTransformControls(FileRenderSettings& settings, float translationSpeed)
{
    ImGui::SameLine();
    const RenderModeState transformState = woby::fileTransformIsDefault(settings)
        ? RenderModeState::off
        : RenderModeState::on;
    if (drawRenderModeIconButton(
            "transform",
            transformIcon,
            "Transform geometry",
            transformState,
            false)) {
        ImGui::OpenPopup("Transform");
    }
    if (ImGui::BeginPopup("Transform")) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Transform geometry");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            woby::resetFileTransform(settings);
        }
        std::array<float, 3> translation = settings.translation;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragFloat3(
            "Move",
            translation.data(),
            translationSpeed)) {
            woby::setFileTranslation(settings, translation);
        }
        setLastItemTooltip("Position offset for this file");
        std::array<float, 3> rotationDegrees = settings.rotationDegrees;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragFloat3(
            "Rotate",
            rotationDegrees.data(),
            1.0f,
            -180.0f,
            180.0f,
            "%.0f deg")) {
            woby::setFileRotationDegrees(settings, rotationDegrees);
        }
        setLastItemTooltip("Rotation in degrees for this file");
        float scale = settings.scale;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::DragFloat(
            "Scale",
            &scale,
            0.01f,
            woby::minGroupScale,
            woby::maxGroupScale,
            "%.2fx")) {
            woby::setFileScale(settings, scale);
        }
        setLastItemTooltip("Uniform scale for this file");
        float opacity = settings.opacity;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderFloat(
            "Opacity",
            &opacity,
            woby::minGroupOpacity,
            woby::maxGroupOpacity,
            "%.2f")) {
            woby::setFileOpacity(settings, opacity);
        }
        setLastItemTooltip("Opacity for this file");
        ImGui::EndPopup();
    }
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
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_ALWAYS, false, color, primitiveState));
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

void submitSceneFiles(
    const std::vector<LoadedObjFile>& files,
    const std::vector<LoadedObjRuntime>& runtimes,
    float masterVertexPointSize,
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight)
{
    const size_t renderFileCount = std::min(files.size(), runtimes.size());
    for (size_t fileIndex = 0; fileIndex < renderFileCount; ++fileIndex) {
        const auto& file = files[fileIndex];
        const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
        if (!file.fileSettings.visible) {
            continue;
        }

        float fileModel[16];
        woby::fileTransformMatrix(file.fileSettings, fileModel);
        for (size_t nodeIndex = 0; nodeIndex < gpuMesh.nodeRanges.size(); ++nodeIndex) {
            const auto& settings = file.groupSettings[nodeIndex];
            if (!settings.visible) {
                continue;
            }

            float groupModel[16];
            float model[16];
            woby::groupTransformMatrix(settings, groupModel);
            bx::mtxMul(model, fileModel, groupModel);
            const auto& range = gpuMesh.nodeRanges[nodeIndex];
            if (settings.showSolidMesh) {
                submitTriangleRange(
                    gpuMesh,
                    meshProgram,
                    colorUniform,
                    model,
                    groupColor(settings, 1.0f, file.fileSettings.opacity),
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
                    groupColor(settings, 1.25f, file.fileSettings.opacity),
                    BGFX_STATE_PT_LINES,
                    range.lineIndexOffset,
                    range.lineIndexCount);
            }
            if (settings.showVertices) {
                const uint32_t pointSize = vertexPointSize(
                    masterVertexPointSize,
                    file.vertexSizeScale * settings.vertexSizeScale);
                submitPointSpriteRange(
                    gpuMesh,
                    pointSpriteProgram,
                    colorUniform,
                    pointParamsUniform,
                    model,
                    groupColor(settings, 1.5f, file.fileSettings.opacity),
                    static_cast<float>(pointSize),
                    sceneViewportWidth,
                    viewportHeight,
                    range.pointSpriteIndexOffset,
                    range.pointSpriteIndexCount);
            }
        }
    }
}

float helperGridExtent(const woby::Bounds& bounds)
{
    const float extent = std::max({
        std::abs(bounds.min[0]),
        std::abs(bounds.max[0]),
        std::abs(bounds.min[1]),
        std::abs(bounds.max[1]),
        woby::defaultDisplayBoundsMax,
    });
    return std::max(extent, 0.001f);
}

float helperGridSpacing(float extent)
{
    constexpr float targetIntervals = 20.0f;
    const float rawSpacing = std::max((extent * 2.0f) / targetIntervals, 0.001f);
    const float magnitude = std::pow(10.0f, std::floor(std::log10(rawSpacing)));
    const float normalized = rawSpacing / magnitude;
    if (normalized <= 1.0f) {
        return magnitude;
    }
    if (normalized <= 2.0f) {
        return 2.0f * magnitude;
    }
    if (normalized <= 5.0f) {
        return 5.0f * magnitude;
    }

    return 10.0f * magnitude;
}

void appendHelperLine(
    std::vector<HelperLineVertex>& vertices,
    const std::array<float, 3>& start,
    const std::array<float, 3>& end)
{
    vertices.push_back({start});
    vertices.push_back({end});
}

void submitHelperLines(
    const std::vector<HelperLineVertex>& vertices,
    const bgfx::VertexLayout& layout,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const std::array<float, 4>& color)
{
    if (vertices.empty() || vertices.size() % 2u != 0u) {
        return;
    }

    bgfx::TransientVertexBuffer vertexBuffer;
    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    if (bgfx::getAvailTransientVertexBuffer(vertexCount, layout) < vertexCount) {
        return;
    }
    bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, layout);

    auto* destination = reinterpret_cast<HelperLineVertex*>(vertexBuffer.data);
    std::copy(vertices.begin(), vertices.end(), destination);

    float model[16];
    bx::mtxIdentity(model);
    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, &vertexBuffer);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_ALWAYS, false, color, BGFX_STATE_PT_LINES));
    bgfx::submit(helperView, program);
}

void submitSceneHelpers(
    const woby::UiState& state,
    const bgfx::VertexLayout& layout,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform)
{
    const float extent = helperGridExtent(state.sceneBounds);
    const float spacing = helperGridSpacing(extent);
    const int lineRadius = std::max(1, static_cast<int>(std::ceil(extent / spacing)));
    const float snappedExtent = static_cast<float>(lineRadius) * spacing;

    if (state.showGrid) {
        std::vector<HelperLineVertex> gridLines;
        gridLines.reserve(static_cast<size_t>(lineRadius * 4 + 2) * 2u);
        for (int line = -lineRadius; line <= lineRadius; ++line) {
            const float offset = static_cast<float>(line) * spacing;
            appendHelperLine(gridLines, {offset, -snappedExtent, 0.0f}, {offset, snappedExtent, 0.0f});
            appendHelperLine(gridLines, {-snappedExtent, offset, 0.0f}, {snappedExtent, offset, 0.0f});
        }
        submitHelperLines(gridLines, layout, program, colorUniform, {0.72f, 0.74f, 0.78f, 0.42f});
    }

    if (state.showOrigin) {
        const float axisLength = std::max(spacing * 2.0f, snappedExtent * 0.18f);

        std::vector<HelperLineVertex> axisLine;
        axisLine.reserve(2u);

        appendHelperLine(axisLine, {0.0f, 0.0f, 0.0f}, {axisLength, 0.0f, 0.0f});
        submitHelperLines(axisLine, layout, program, colorUniform, {1.0f, 0.20f, 0.20f, 1.0f});

        axisLine.clear();
        appendHelperLine(axisLine, {0.0f, 0.0f, 0.0f}, {0.0f, axisLength, 0.0f});
        submitHelperLines(axisLine, layout, program, colorUniform, {0.20f, 0.85f, 0.35f, 1.0f});

        axisLine.clear();
        appendHelperLine(axisLine, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, axisLength});
        submitHelperLines(axisLine, layout, program, colorUniform, {0.30f, 0.55f, 1.0f, 1.0f});
    }
}

struct SdlDeleter {
    void operator()(SDL_Window* window) const noexcept
    {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
};

void SDLCALL objFileDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<ObjFileDialogState*>(userdata);
    std::vector<std::filesystem::path> selectedPaths;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Open dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Open canceled";
        showStatus = true;
    } else {
        for (size_t index = 0; filelist[index] != nullptr; ++index) {
            selectedPaths.emplace_back(filelist[index]);
        }
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingPaths.insert(
        state->pendingPaths.end(),
        selectedPaths.begin(),
        selectedPaths.end());
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->open = false;
}

void showObjFileDialog(SDL_Window* window, ObjFileDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"Wavefront OBJ", "obj"},
        {"All files", "*"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.open) {
            return;
        }
        state.open = true;
    }

    SDL_ShowOpenFileDialog(
        objFileDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr,
        true);
}

std::vector<std::filesystem::path> takePendingObjPaths(ObjFileDialogState& state)
{
    std::vector<std::filesystem::path> paths;
    std::lock_guard<std::mutex> lock(state.mutex);
    paths.swap(state.pendingPaths);
    return paths;
}

void setObjFileDialogStatus(ObjFileDialogState& state, std::string status)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    ++state.statusVersion;
}

std::string objFileDialogStatus(ObjFileDialogState& state, uint64_t& statusVersion)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.statusVersion == statusVersion) {
        return {};
    }

    statusVersion = state.statusVersion;
    return state.status;
}

bool objFileDialogIsOpen(ObjFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.open;
}

void SDLCALL openSceneDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<SceneFileDialogState*>(userdata);
    std::optional<std::filesystem::path> selectedPath;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Open scene dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Open scene canceled";
        showStatus = true;
    } else {
        selectedPath = std::filesystem::path(filelist[0]);
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingOpenPath = std::move(selectedPath);
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->openDialogOpen = false;
}

void SDLCALL saveSceneDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<SceneFileDialogState*>(userdata);
    std::optional<std::filesystem::path> selectedPath;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Save scene dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Save scene canceled";
        showStatus = true;
    } else {
        selectedPath = std::filesystem::path(filelist[0]);
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingSavePath = std::move(selectedPath);
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->saveDialogOpen = false;
}

void showOpenSceneDialog(SDL_Window* window, SceneFileDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"woby scene", "woby"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.openDialogOpen) {
            return;
        }
        state.openDialogOpen = true;
    }

    SDL_ShowOpenFileDialog(
        openSceneDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr,
        false);
}

void showSaveSceneDialog(SDL_Window* window, SceneFileDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"woby scene", "woby"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.saveDialogOpen) {
            return;
        }
        state.saveDialogOpen = true;
    }

    SDL_ShowSaveFileDialog(
        saveSceneDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr);
}

std::optional<std::filesystem::path> takePendingOpenScenePath(SceneFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    std::optional<std::filesystem::path> path = std::move(state.pendingOpenPath);
    state.pendingOpenPath.reset();
    return path;
}

std::optional<std::filesystem::path> takePendingSaveScenePath(SceneFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    std::optional<std::filesystem::path> path = std::move(state.pendingSavePath);
    state.pendingSavePath.reset();
    return path;
}

void setSceneFileDialogStatus(SceneFileDialogState& state, std::string status)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    ++state.statusVersion;
}

std::string sceneFileDialogStatus(SceneFileDialogState& state, uint64_t& statusVersion)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.statusVersion == statusVersion) {
        return {};
    }

    statusVersion = state.statusVersion;
    return state.status;
}

bool sceneFileDialogIsOpen(SceneFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.openDialogOpen || state.saveDialogOpen;
}

void appendFolderObjPaths(
    const std::filesystem::path& folder,
    std::vector<std::filesystem::path>& modelPaths)
{
    const std::vector<std::filesystem::path> folderPaths = woby::collectObjPathsRecursive(folder);
    if (folderPaths.empty()) {
        throw std::runtime_error("--folder did not contain OBJ files recursively: " + folder.string());
    }

    modelPaths.insert(modelPaths.end(), folderPaths.begin(), folderPaths.end());
}

spdlog::level::level_enum toSpdlogLevel(woby::LogLevel level)
{
    switch (level) {
    case woby::LogLevel::off:
        return spdlog::level::off;
    case woby::LogLevel::trace:
        return spdlog::level::trace;
    case woby::LogLevel::debug:
        return spdlog::level::debug;
    case woby::LogLevel::info:
        return spdlog::level::info;
    case woby::LogLevel::warn:
        return spdlog::level::warn;
    case woby::LogLevel::error:
        return spdlog::level::err;
    case woby::LogLevel::critical:
        return spdlog::level::critical;
    }

    throw std::runtime_error("Unsupported log level.");
}

void initializeLogging(const woby::AppArguments& arguments)
{
    if (arguments.logLevel == woby::LogLevel::off) {
        spdlog::set_level(spdlog::level::off);
        return;
    }

    const spdlog::level::level_enum level = toSpdlogLevel(arguments.logLevel);
    const auto logger = spdlog::basic_logger_mt("woby", arguments.logFile.value().string());
    logger->set_level(level);
    logger->flush_on(level);
    spdlog::set_default_logger(logger);
    spdlog::set_level(level);
    spdlog::info("woby version {}", WOBY_VERSION);
}

std::vector<std::filesystem::path> resolveModelPaths(const woby::AppArguments& options)
{
    std::vector<std::filesystem::path> modelPaths;

    for (const auto& inputPath : options.inputPaths) {
        if (inputPath.folder) {
            appendFolderObjPaths(inputPath.path, modelPaths);
            continue;
        }

        modelPaths.push_back(inputPath.path);
    }

    return modelPaths;
}

LoadedObjFileWithRuntime loadObjFile(
    const std::filesystem::path& modelPath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    size_t firstColorIndex)
{
    LoadedObjFileWithRuntime loaded;
    loaded.file = woby::createUiFileState(modelPath, woby::loadObjMesh(modelPath), firstColorIndex);
    loaded.runtime.gpuMesh = createGpuMesh(loaded.file.mesh, meshLayout, pointSpriteLayout);

    return loaded;
}

void destroyObjRuntimes(std::vector<LoadedObjRuntime>& runtimes);

std::vector<LoadedObjFile> loadObjFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    std::vector<LoadedObjRuntime>& runtimes,
    size_t firstColorIndex = 0)
{
    std::vector<LoadedObjFile> files;
    files.reserve(modelPaths.size());
    runtimes.reserve(modelPaths.size());
    size_t colorIndex = firstColorIndex;

    try {
        for (const auto& modelPath : modelPaths) {
            LoadedObjFileWithRuntime loaded = loadObjFile(modelPath, meshLayout, pointSpriteLayout, colorIndex);
            colorIndex += loaded.file.groupSettings.size();
            files.push_back(std::move(loaded.file));
            runtimes.push_back(std::move(loaded.runtime));
        }
    } catch (...) {
        destroyObjRuntimes(runtimes);
        throw;
    }

    return files;
}

void appendInitialObjFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes)
{
    if (modelPaths.empty()) {
        return;
    }

    std::vector<LoadedObjRuntime> loadedRuntimes;
    std::vector<LoadedObjFile> loadedFiles = loadObjFiles(
        modelPaths,
        meshLayout,
        pointSpriteLayout,
        loadedRuntimes,
        woby::totalGroupCount(state));

    state.files.insert(
        state.files.end(),
        std::make_move_iterator(loadedFiles.begin()),
        std::make_move_iterator(loadedFiles.end()));
    runtimes.insert(
        runtimes.end(),
        std::make_move_iterator(loadedRuntimes.begin()),
        std::make_move_iterator(loadedRuntimes.end()));
    woby::recalculateSceneBounds(state);
    woby::frameCameraToScene(state);
    woby::markSceneDirty(state);
}

void removeObjFile(
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes,
    size_t fileIndex)
{
    if (fileIndex >= runtimes.size()) {
        return;
    }

    destroyGpuMesh(runtimes[fileIndex].gpuMesh);
    runtimes.erase(runtimes.begin() + static_cast<std::ptrdiff_t>(fileIndex));
    (void)woby::removeFileFromState(state, fileIndex);
}

bool appendObjFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes,
    std::string& status)
{
    size_t addedCount = 0;
    size_t skippedCount = 0;
    size_t failedCount = 0;
    std::string lastError;
    size_t colorIndex = woby::totalGroupCount(state);
    state.files.reserve(state.files.size() + modelPaths.size());
    runtimes.reserve(runtimes.size() + modelPaths.size());

    for (const auto& modelPath : modelPaths) {
        if (!woby::isObjPath(modelPath)) {
            ++skippedCount;
            continue;
        }

        try {
            LoadedObjFileWithRuntime loaded = loadObjFile(modelPath, meshLayout, pointSpriteLayout, colorIndex);
            colorIndex += loaded.file.groupSettings.size();
            state.files.push_back(std::move(loaded.file));
            runtimes.push_back(std::move(loaded.runtime));
            ++addedCount;
        } catch (const std::exception& exception) {
            ++failedCount;
            lastError = exception.what();
        }
    }

    status = "Added " + std::to_string(addedCount) + " OBJ file";
    if (addedCount != 1u) {
        status += "s";
    }
    if (skippedCount > 0u) {
        status += ", skipped " + std::to_string(skippedCount) + " non-OBJ";
    }
    if (failedCount > 0u) {
        status += ", failed " + std::to_string(failedCount);
        if (!lastError.empty()) {
            status += ": " + lastError;
        }
    }

    if (addedCount > 0u) {
        woby::recalculateSceneBounds(state);
        woby::frameCameraToScene(state);
        woby::markSceneDirty(state);
    }

    return addedCount > 0u;
}

void pushDroppedPath(DragDropState& state, const char* data)
{
    if (data == nullptr) {
        return;
    }

    if (state.active) {
        state.batchPaths.emplace_back(data);
    } else {
        state.pendingPaths.emplace_back(data);
    }
}

void finishDropBatch(DragDropState& state)
{
    state.pendingPaths.insert(
        state.pendingPaths.end(),
        state.batchPaths.begin(),
        state.batchPaths.end());
    state.batchPaths.clear();
    state.active = false;
}

std::vector<std::filesystem::path> takePendingDropPaths(DragDropState& state)
{
    std::vector<std::filesystem::path> paths;
    paths.swap(state.pendingPaths);
    return paths;
}

struct DroppedPathClassification {
    std::vector<std::filesystem::path> objPaths;
    std::vector<std::filesystem::path> scenePaths;
    size_t unsupportedCount = 0;
    size_t emptyFolderCount = 0;
    size_t failedFolderCount = 0;
    std::string lastFolderError;
};

DroppedPathClassification classifyDroppedPaths(
    const std::vector<std::filesystem::path>& paths)
{
    DroppedPathClassification classification;

    for (const auto& path : paths) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            try {
                const std::vector<std::filesystem::path> folderObjPaths =
                    woby::collectObjPathsRecursive(path);
                if (folderObjPaths.empty()) {
                    ++classification.emptyFolderCount;
                } else {
                    classification.objPaths.insert(
                        classification.objPaths.end(),
                        folderObjPaths.begin(),
                        folderObjPaths.end());
                }
            } catch (const std::exception& exception) {
                ++classification.failedFolderCount;
                classification.lastFolderError = exception.what();
            }
            continue;
        }

        if (woby::isObjPath(path)) {
            classification.objPaths.push_back(path);
            continue;
        }

        if (woby::isWobyPath(path)) {
            classification.scenePaths.push_back(path);
            continue;
        }

        ++classification.unsupportedCount;
    }

    return classification;
}

void appendDropClassificationStatus(
    std::string& status,
    const DroppedPathClassification& classification)
{
    if (classification.unsupportedCount > 0u) {
        status += ", skipped " + std::to_string(classification.unsupportedCount) + " unsupported";
    }
    if (classification.emptyFolderCount > 0u) {
        status += ", skipped " + std::to_string(classification.emptyFolderCount) + " empty folder";
        if (classification.emptyFolderCount != 1u) {
            status += "s";
        }
    }
    if (classification.failedFolderCount > 0u) {
        status += ", failed " + std::to_string(classification.failedFolderCount) + " folder";
        if (classification.failedFolderCount != 1u) {
            status += "s";
        }
        if (!classification.lastFolderError.empty()) {
            status += ": " + classification.lastFolderError;
        }
    }
}

void destroyObjRuntimes(std::vector<LoadedObjRuntime>& runtimes)
{
    for (auto& runtime : runtimes) {
        destroyGpuMesh(runtime.gpuMesh);
    }
    runtimes.clear();
}

std::vector<LoadedObjFile> loadSceneFiles(
    const std::filesystem::path& scenePath,
    const woby::SceneDocument& document,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    std::vector<LoadedObjRuntime>& runtimes)
{
    std::vector<LoadedObjFile> loadedFiles;
    std::vector<LoadedObjRuntime> loadedRuntimes;
    loadedFiles.reserve(document.files.size());
    loadedRuntimes.reserve(document.files.size());
    size_t colorIndex = 0;

    try {
        for (const auto& record : document.files) {
            const std::filesystem::path modelPath = woby::sceneAbsolutePath(scenePath, record.path);
            LoadedObjFileWithRuntime loaded = loadObjFile(modelPath, meshLayout, pointSpriteLayout, colorIndex);
            woby::applySceneFileRecord(loaded.file, record);
            colorIndex += loaded.file.groupSettings.size();
            loadedRuntimes.push_back(std::move(loaded.runtime));
            loadedFiles.push_back(std::move(loaded.file));
        }
    } catch (...) {
        destroyObjRuntimes(loadedRuntimes);
        throw;
    }

    runtimes = std::move(loadedRuntimes);
    return loadedFiles;
}

void loadScene(
    const std::filesystem::path& scenePath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes)
{
    const woby::SceneDocument document = woby::readSceneDocument(scenePath);
    std::vector<LoadedObjRuntime> loadedRuntimes;
    std::vector<LoadedObjFile> loadedFiles = loadSceneFiles(
        scenePath,
        document,
        meshLayout,
        pointSpriteLayout,
        loadedRuntimes);

    destroyObjRuntimes(runtimes);
    runtimes = std::move(loadedRuntimes);
    state.files = std::move(loadedFiles);
    woby::setShowOrigin(state, document.showOrigin);
    woby::setShowGrid(state, document.showGrid);
    woby::setMasterVertexPointSize(state, document.masterVertexPointSize);
    woby::recalculateSceneBounds(state);
    state.camera = woby::frameCameraBounds(state.sceneBounds);
}

std::filesystem::path saveScene(
    const std::filesystem::path& requestedScenePath,
    const woby::UiState& state)
{
    const std::filesystem::path scenePath = woby::sceneSavePathWithExtension(requestedScenePath);
    woby::writeSceneDocument(scenePath, woby::createSceneDocument(state));
    return scenePath;
}

void loadSceneFromPath(
    const std::filesystem::path& requestedScenePath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument)
{
    const std::filesystem::path scenePath = normalizedPath(requestedScenePath);
    loadScene(scenePath, meshLayout, pointSpriteLayout, state, runtimes);
    currentScenePath = scenePath;
    cleanSceneDocument = woby::createSceneDocument(state);
    woby::clearSceneDirty(state);
}

std::filesystem::path saveSceneToPath(
    const std::filesystem::path& requestedScenePath,
    woby::UiState& state,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument)
{
    const std::filesystem::path scenePath = normalizedPath(saveScene(requestedScenePath, state));
    currentScenePath = scenePath;
    cleanSceneDocument = woby::createSceneDocument(state);
    woby::clearSceneDirty(state);
    return scenePath;
}

void setToastMessage(ToastMessage& toast, std::string text)
{
    toast.text = std::move(text);
    toast.startedAt = std::chrono::steady_clock::now();
}

void openSceneOrRequestDirtyWarning(
    const std::filesystem::path& scenePath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument,
    SceneFileDialogState& sceneFileDialogState,
    ToastMessage& toast,
    std::optional<std::filesystem::path>& pendingDirtyOpenScenePath,
    bool& requestDirtyOpenWarning)
{
    if (state.isDirty) {
        pendingDirtyOpenScenePath = normalizedPath(scenePath);
        requestDirtyOpenWarning = true;
        return;
    }

    try {
        loadSceneFromPath(
            scenePath,
            meshLayout,
            pointSpriteLayout,
            state,
            runtimes,
            currentScenePath,
            cleanSceneDocument);
        setToastMessage(toast, "Opened scene " + fileDisplayName(scenePath));
    } catch (const std::exception& exception) {
        setSceneFileDialogStatus(
            sceneFileDialogState,
            std::string("Open scene failed: ") + exception.what());
    }
}

void processDroppedPaths(
    const std::vector<std::filesystem::path>& paths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedObjRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument,
    SceneFileDialogState& sceneFileDialogState,
    ToastMessage& toast,
    std::optional<std::filesystem::path>& pendingDirtyOpenScenePath,
    bool& requestDirtyOpenWarning)
{
    const DroppedPathClassification classification = classifyDroppedPaths(paths);
    const size_t extraPathCount = classification.objPaths.size()
        + classification.unsupportedCount
        + classification.emptyFolderCount
        + classification.failedFolderCount;

    if (!classification.scenePaths.empty()) {
        if (classification.scenePaths.size() != 1u || extraPathCount > 0u) {
            setToastMessage(toast, "Drop one .woby scene file by itself");
            return;
        }

        openSceneOrRequestDirtyWarning(
            classification.scenePaths[0],
            meshLayout,
            pointSpriteLayout,
            state,
            runtimes,
            currentScenePath,
            cleanSceneDocument,
            sceneFileDialogState,
            toast,
            pendingDirtyOpenScenePath,
            requestDirtyOpenWarning);
        return;
    }

    std::string status;
    if (classification.objPaths.empty()) {
        status = "No OBJ files added";
    } else {
        (void)appendObjFiles(
            classification.objPaths,
            meshLayout,
            pointSpriteLayout,
            state,
            runtimes,
            status);
    }
    appendDropClassificationStatus(status, classification);
    setToastMessage(toast, std::move(status));
}

void drawToastMessage(const ToastMessage& toast, uint32_t width)
{
    if (toast.text.empty()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const float elapsedSeconds = std::chrono::duration<float>(now - toast.startedAt).count();
    if (elapsedSeconds >= toastDurationSeconds) {
        return;
    }

    const float alpha = std::clamp(
        (toastDurationSeconds - elapsedSeconds) / 0.35f,
        0.0f,
        1.0f);
    ImGui::SetNextWindowBgAlpha(0.86f * alpha);
    ImGui::SetNextWindowPos(
        ImVec2(static_cast<float>(width) - toastMargin, toastMargin),
        ImGuiCond_Always,
        ImVec2(1.0f, 0.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
    if (ImGui::Begin(
            "##ToastMessage",
            nullptr,
            ImGuiWindowFlags_NoDecoration
                | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoFocusOnAppearing
                | ImGuiWindowFlags_NoInputs)) {
        ImGui::TextUnformatted(toast.text.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

void drawHoveredVertexOverlay(const std::optional<HoveredVertex>& hoveredVertex, uint32_t width, uint32_t height)
{
    if (!hoveredVertex.has_value()) {
        return;
    }

    ImGui::SetNextWindowBgAlpha(0.86f);
    ImGui::SetNextWindowPos(
        ImVec2(
            static_cast<float>(width) - toastMargin,
            static_cast<float>(height) - toastMargin),
        ImGuiCond_Always,
        ImVec2(1.0f, 1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 8.0f));
    if (ImGui::Begin(
            "##HoveredVertex",
            nullptr,
            ImGuiWindowFlags_NoDecoration
                | ImGuiWindowFlags_AlwaysAutoResize
                | ImGuiWindowFlags_NoMove
                | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoFocusOnAppearing
                | ImGuiWindowFlags_NoInputs)) {
        const auto& local = hoveredVertex->localPosition;
        const auto& transformed = hoveredVertex->transformedPosition;
        ImGui::Text(
            "Local: %.4f, %.4f, %.4f",
            static_cast<double>(local[0]),
            static_cast<double>(local[1]),
            static_cast<double>(local[2]));
        ImGui::Text(
            "Transformed: %.4f, %.4f, %.4f",
            static_cast<double>(transformed[0]),
            static_cast<double>(transformed[1]),
            static_cast<double>(transformed[2]));
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

} // namespace

int main(int argc, char** argv)
{
    bool sdlInitialized = false;
    bool bgfxInitialized = false;

    try {
        const auto commandLine = woby::parseCommandLine(argc, argv);
        initializeLogging(commandLine);
        if (commandLine.showVersion) {
            std::printf("%s\n", WOBY_VERSION);
            spdlog::shutdown();
            return 0;
        }

        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
        sdlInitialized = true;

        SDL_Window* rawWindow = SDL_CreateWindow("woby", 1280, 720, SDL_WINDOW_RESIZABLE);
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
        bgfx::setViewClear(helperView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
        bgfx::setDebug(BGFX_DEBUG_TEXT);

        const auto assets = assetRoot();
        const auto modelPaths = resolveModelPaths(commandLine);
        const auto layout = meshVertexLayout();
        const auto pointLayout = pointSpriteVertexLayout();
        const auto helperLayout = helperLineVertexLayout();
        woby::UiState ui;
        std::vector<LoadedObjRuntime> runtimes;
        std::optional<std::filesystem::path> currentScenePath;
        woby::SceneDocument cleanSceneDocument;

        if (commandLine.scenePath.has_value()) {
            loadSceneFromPath(
                commandLine.scenePath.value(),
                layout,
                pointLayout,
                ui,
                runtimes,
                currentScenePath,
                cleanSceneDocument);
            appendInitialObjFiles(modelPaths, layout, pointLayout, ui, runtimes);
        } else {
            ui.files = loadObjFiles(modelPaths, layout, pointLayout, runtimes);
            woby::recalculateSceneBounds(ui);
            woby::updateSceneDirty(ui, cleanSceneDocument);
        }

        bgfx::ProgramHandle meshProgram = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
        bgfx::ProgramHandle colorProgram = woby::loadProgram(assets, "vs_color.bin", "fs_color.bin");
        bgfx::ProgramHandle pointSpriteProgram = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_point_sprite.bin");
        bgfx::UniformHandle colorUniform = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
        bgfx::UniformHandle pointParamsUniform = bgfx::createUniform("u_pointParams", bgfx::UniformType::Vec4);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        loadAppFont(assets);
        configureAppStyle();

        if (!ImGui_ImplSDL3_InitForOther(window.get())) {
            throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed.");
        }
        woby::imgui_bgfx::init(assets, imguiView);

        ui.camera = woby::frameCameraBounds(ui.sceneBounds);
        ui.viewerPaneWidth = minimumViewerPaneWidth();
        auto& running = ui.running;
        auto& files = ui.files;
        auto& sceneBounds = ui.sceneBounds;
        auto& camera = ui.camera;
        auto& cameraInput = ui.cameraInput;
        auto& masterVertexPointSize = ui.masterVertexPointSize;
        auto& viewerPaneWidth = ui.viewerPaneWidth;
        static ObjFileDialogState objFileDialogState;
        static SceneFileDialogState sceneFileDialogState;
        DragDropState dragDropState;
        std::optional<std::filesystem::path> pendingDirtyOpenScenePath;
        bool requestDirtyOpenWarning = false;
        bool requestDirtyQuitWarning = false;
        ToastMessage toast;
        uint64_t observedObjFileDialogStatusVersion = 0;
        uint64_t observedSceneFileDialogStatusVersion = 0;
        auto previousFrame = std::chrono::steady_clock::now();
        auto fpsWindowStart = previousFrame;
        int fpsFrameCount = 0;
        float fps = 0.0f;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);

                if (event.type == SDL_EVENT_QUIT) {
                    if (ui.isDirty) {
                        requestDirtyQuitWarning = true;
                    } else {
                        woby::requestQuit(ui);
                    }
                }
                if (event.type == SDL_EVENT_DROP_BEGIN) {
                    dragDropState.batchPaths.clear();
                    dragDropState.active = true;
                }
                if (event.type == SDL_EVENT_DROP_FILE) {
                    pushDroppedPath(dragDropState, event.drop.data);
                }
                if (event.type == SDL_EVENT_DROP_COMPLETE) {
                    finishDropBatch(dragDropState);
                }
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    if (ui.isDirty) {
                        requestDirtyQuitWarning = true;
                    } else {
                        woby::requestQuit(ui);
                    }
                }
                if (event.type == SDL_EVENT_KEY_DOWN
                    && event.key.key == SDLK_R
                    && !ImGui::GetIO().WantCaptureKeyboard) {
                    woby::frameCameraToScene(ui);
                }
                if (event.type == SDL_EVENT_KEY_DOWN
                    && event.key.key == SDLK_B
                    && (event.key.mod & SDL_KMOD_CTRL) != 0u
                    && !event.key.repeat
                    && !ImGui::GetIO().WantCaptureKeyboard) {
                    woby::toggleViewerPaneVisible(ui);
                }
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    getDrawableSize(window.get(), width, height);
                    bgfx::reset(width, height, resetFlags);
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !ImGui::GetIO().WantCaptureMouse) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        woby::setCameraOrbiting(ui, true);
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        woby::setCameraPanning(ui, true);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        woby::setCameraOrbiting(ui, false);
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        woby::setCameraPanning(ui, false);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (cameraInput.orbiting) {
                        woby::orbitUiCamera(ui, event.motion.xrel, event.motion.yrel);
                    }
                    if (cameraInput.panning) {
                        woby::panUiCamera(ui, event.motion.xrel, event.motion.yrel, static_cast<float>(height));
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL && !ImGui::GetIO().WantCaptureMouse) {
                    const float wheelY = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED
                        ? -event.wheel.y
                        : event.wheel.y;
                    woby::dollyUiCamera(ui, -wheelY * 0.12f);
                }
            }

            getDrawableSize(window.get(), width, height);

            const auto pendingObjPaths = takePendingObjPaths(objFileDialogState);
            if (!pendingObjPaths.empty()) {
                std::string status;
                (void)appendObjFiles(pendingObjPaths, layout, pointLayout, ui, runtimes, status);
                setObjFileDialogStatus(objFileDialogState, std::move(status));
            }
            const std::string objDialogStatus = objFileDialogStatus(
                objFileDialogState,
                observedObjFileDialogStatusVersion);
            if (!objDialogStatus.empty()) {
                setToastMessage(toast, objDialogStatus);
            }

            const auto pendingOpenScenePath = takePendingOpenScenePath(sceneFileDialogState);
            if (pendingOpenScenePath.has_value()) {
                openSceneOrRequestDirtyWarning(
                    pendingOpenScenePath.value(),
                    layout,
                    pointLayout,
                    ui,
                    runtimes,
                    currentScenePath,
                    cleanSceneDocument,
                    sceneFileDialogState,
                    toast,
                    pendingDirtyOpenScenePath,
                    requestDirtyOpenWarning);
            }
            const auto pendingSaveScenePath = takePendingSaveScenePath(sceneFileDialogState);
            if (pendingSaveScenePath.has_value()) {
                try {
                    const auto scenePath = saveSceneToPath(
                        pendingSaveScenePath.value(),
                        ui,
                        currentScenePath,
                        cleanSceneDocument);
                    setToastMessage(toast, "Saved scene " + fileDisplayName(scenePath));
                } catch (const std::exception& exception) {
                    setSceneFileDialogStatus(
                        sceneFileDialogState,
                        std::string("Save scene failed: ") + exception.what());
                }
            }
            const std::string sceneDialogStatus = sceneFileDialogStatus(
                sceneFileDialogState,
                observedSceneFileDialogStatusVersion);
            if (!sceneDialogStatus.empty()) {
                setToastMessage(toast, sceneDialogStatus);
            }

            const auto droppedPaths = takePendingDropPaths(dragDropState);
            if (!droppedPaths.empty()) {
                processDroppedPaths(
                    droppedPaths,
                    layout,
                    pointLayout,
                    ui,
                    runtimes,
                    currentScenePath,
                    cleanSceneDocument,
                    sceneFileDialogState,
                    toast,
                    pendingDirtyOpenScenePath,
                    requestDirtyOpenWarning);
            }
            woby::updateSceneDirty(ui, cleanSceneDocument);
            updateAppWindowTitle(window.get(), currentScenePath, ui.isDirty);

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

            const auto& bounds = sceneBounds;
            woby::updateCameraFromKeyboard(camera, bounds, deltaSeconds);

            const float minViewerPaneWidth = minimumViewerPaneWidth();
            const float maxViewerPaneWidth = std::max(
                minViewerPaneWidth,
                static_cast<float>(width) - minSceneViewportWidth);
            woby::setViewerPaneWidth(ui, viewerPaneWidth, minViewerPaneWidth, maxViewerPaneWidth);

            bgfx::dbgTextClear();

            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (requestDirtyOpenWarning) {
                ImGui::OpenPopup("Unsaved scene changes");
                requestDirtyOpenWarning = false;
            }
            if (ImGui::BeginPopupModal(
                    "Unsaved scene changes",
                    nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("The current scene has unsaved changes.");
                if (pendingDirtyOpenScenePath.has_value()) {
                    ImGui::Text(
                        "Open %s and discard them?",
                        fileDisplayName(pendingDirtyOpenScenePath.value()).c_str());
                }
                if (ImGui::Button("Open")) {
                    if (pendingDirtyOpenScenePath.has_value()) {
                        try {
                            const auto scenePath = pendingDirtyOpenScenePath.value();
                            loadSceneFromPath(
                                scenePath,
                                layout,
                                pointLayout,
                                ui,
                                runtimes,
                                currentScenePath,
                                cleanSceneDocument);
                            setToastMessage(toast, "Opened scene " + fileDisplayName(scenePath));
                        } catch (const std::exception& exception) {
                            setSceneFileDialogStatus(
                                sceneFileDialogState,
                                std::string("Open scene failed: ") + exception.what());
                        }
                    }
                    pendingDirtyOpenScenePath.reset();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    pendingDirtyOpenScenePath.reset();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (requestDirtyQuitWarning) {
                ImGui::OpenPopup("Unsaved scene changes##quit");
                requestDirtyQuitWarning = false;
            }
            if (ImGui::BeginPopupModal(
                    "Unsaved scene changes##quit",
                    nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("The current scene has unsaved changes.");
                ImGui::TextUnformatted("Exit and discard them?");
                if (ImGui::Button("Exit")) {
                    woby::requestQuit(ui);
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            const float activeViewerPaneWidth = ui.viewerPaneVisible ? viewerPaneWidth : 0.0f;
            if (ui.viewerPaneVisible) {
                const float availableHeight = static_cast<float>(height);
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(
                    ImVec2(viewerPaneWidth, availableHeight),
                    ImGuiCond_Always);
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(minViewerPaneWidth, availableHeight),
                    ImVec2(maxViewerPaneWidth, availableHeight));
                const bool showViewerContent = ImGui::Begin(
                    "##ViewerPane",
                    nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
                if (showViewerContent) {
                    woby::setViewerPaneWidth(
                        ui,
                        ImGui::GetWindowSize().x,
                        minViewerPaneWidth,
                        maxViewerPaneWidth);

                    const bool scenePaneOpen = ImGui::CollapsingHeader(
                        "Scene",
                        ImGuiTreeNodeFlags_DefaultOpen);
                if (scenePaneOpen) {
                    const float sceneContentHeight = std::max(
                        defaultScenePaneHeight - ImGui::GetFrameHeightWithSpacing(),
                        ImGui::GetFrameHeight());
                    if (ImGui::BeginChild(
                            "SceneContent",
                            ImVec2(0.0f, sceneContentHeight),
                            ImGuiChildFlags_None)) {
                        const bool fileDialogOpen = objFileDialogIsOpen(objFileDialogState);
                        const bool sceneDialogOpen = sceneFileDialogIsOpen(sceneFileDialogState);
                        const bool anyFileDialogOpen = fileDialogOpen || sceneDialogOpen;
                        if (anyFileDialogOpen) {
                            ImGui::BeginDisabled();
                        }
                        if (ImGui::Button(
                                std::string(addObjFileIcon).append("##add_obj_file").c_str(),
                                ImVec2(renderModeButtonSize, renderModeButtonSize))) {
                            showObjFileDialog(window.get(), objFileDialogState);
                        }
                        if (anyFileDialogOpen) {
                            ImGui::EndDisabled();
                        }
                        setLastItemTooltip("Add OBJ files");
                        ImGui::SameLine();
                        if (anyFileDialogOpen) {
                            ImGui::BeginDisabled();
                        }
                        if (ImGui::Button(
                                std::string(openSceneIcon).append("##open_scene").c_str(),
                                ImVec2(renderModeButtonSize, renderModeButtonSize))) {
                            showOpenSceneDialog(window.get(), sceneFileDialogState);
                        }
                        if (anyFileDialogOpen) {
                            ImGui::EndDisabled();
                        }
                        setLastItemTooltip("Open scene");
                        ImGui::SameLine();
                        if (anyFileDialogOpen) {
                            ImGui::BeginDisabled();
                        }
                        if (ImGui::Button(
                                std::string(saveSceneIcon).append("##save_scene").c_str(),
                                ImVec2(renderModeButtonSize, renderModeButtonSize))) {
                            if (currentScenePath.has_value()) {
                                try {
                                    const auto scenePath = saveSceneToPath(
                                        currentScenePath.value(),
                                        ui,
                                        currentScenePath,
                                        cleanSceneDocument);
                                    setToastMessage(toast, "Saved scene " + fileDisplayName(scenePath));
                                } catch (const std::exception& exception) {
                                    setSceneFileDialogStatus(
                                        sceneFileDialogState,
                                        std::string("Save scene failed: ") + exception.what());
                                }
                            } else {
                                showSaveSceneDialog(window.get(), sceneFileDialogState);
                            }
                        }
                        if (anyFileDialogOpen) {
                            ImGui::EndDisabled();
                        }
                        setLastItemTooltip("Save scene");
                        ImGui::Text("Renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
                        ImGui::Text("FPS: %.1f", fps);
                        size_t vertexCountTotal = 0;
                        size_t triangleCountTotal = 0;
                        for (const auto& file : files) {
                            vertexCountTotal += file.mesh.vertices.size();
                            triangleCountTotal += file.mesh.indices.size() / 3u;
                        }
                        drawMeshCountLine(vertexCountTotal, triangleCountTotal);
                        if (drawRenderModeIconButton(
                                "origin",
                                originIcon,
                                ui.showOrigin ? "Hide origin axes" : "Show origin axes",
                                ui.showOrigin ? RenderModeState::on : RenderModeState::off,
                                false)) {
                            woby::toggleShowOrigin(ui);
                        }
                        ImGui::SameLine();
                        if (drawRenderModeIconButton(
                                "grid",
                                gridIcon,
                                ui.showGrid ? "Hide XY grid" : "Show XY grid",
                                ui.showGrid ? RenderModeState::on : RenderModeState::off,
                                false)) {
                            woby::toggleShowGrid(ui);
                        }
                        const size_t groupCount = woby::totalGroupCount(ui);
                        const size_t visibleCount = woby::countVisibleSceneGroups(ui);
                        if (drawTriStateVisibilityButton(
                                "visible",
                                "Scene",
                                visibleCount,
                                groupCount)) {
                            woby::setAllSceneVisible(ui, visibleCount != groupCount);
                        }
                        ImGui::SameLine();
                        const size_t solidMeshCount = woby::countEnabledSceneRenderMode(
                            ui,
                            woby::UiRenderMode::solidMesh);
                        if (drawTriStateMasterIconButton(
                                "solid_mesh",
                                solidMeshIcon,
                                "Solid mesh",
                                solidMeshCount,
                                groupCount)) {
                            woby::setAllSceneRenderModes(
                                ui,
                                woby::UiRenderMode::solidMesh,
                                solidMeshCount != groupCount);
                        }
                        ImGui::SameLine();
                        const size_t triangleCount = woby::countEnabledSceneRenderMode(
                            ui,
                            woby::UiRenderMode::triangles);
                        if (drawTriStateMasterIconButton(
                                "triangles",
                                trianglesIcon,
                                "Triangles",
                                triangleCount,
                                groupCount)) {
                            woby::setAllSceneRenderModes(
                                ui,
                                woby::UiRenderMode::triangles,
                                triangleCount != groupCount);
                        }
                        ImGui::SameLine();
                        const size_t vertexCount = woby::countEnabledSceneRenderMode(
                            ui,
                            woby::UiRenderMode::vertices);
                        if (drawTriStateMasterIconButton(
                                "vertices",
                                verticesIcon,
                                "Vertices",
                                vertexCount,
                                groupCount)) {
                            woby::setAllSceneRenderModes(
                                ui,
                                woby::UiRenderMode::vertices,
                                vertexCount != groupCount);
                        }
                        ImGui::SameLine(0.0f, 0.0f);
                        float editedMasterVertexPointSize = masterVertexPointSize;
                        ImGui::SetNextItemWidth(renderModeButtonRowWidth());
                        pushRenderModeControlHeight();
                        if (ImGui::DragFloat(
                            "##vertex_size",
                            &editedMasterVertexPointSize,
                            0.2f,
                            woby::minVertexPointSize,
                            woby::maxVertexPointSize,
                            "%.0f px")) {
                            woby::setMasterVertexPointSize(ui, editedMasterVertexPointSize);
                        }
                        ImGui::PopStyleVar();
                        setLastItemTooltip("Base vertex point size for all groups");
                    }
                    ImGui::EndChild();
                }

                const std::string filesPaneTitle = "Files (" + std::to_string(files.size()) + ")##Files";
                const bool filesPaneOpen = ImGui::CollapsingHeader(
                    filesPaneTitle.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen);
                if (filesPaneOpen) {
                    const float filesContentHeight = std::max(
                        ImGui::GetContentRegionAvail().y,
                        ImGui::GetFrameHeight());
                    if (ImGui::BeginChild(
                            "FilesContent",
                            ImVec2(0.0f, filesContentHeight),
                            ImGuiChildFlags_None)) {
                        size_t colorIndex = 0;
                        std::optional<size_t> removeFileIndex;
                        for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
                            auto& file = files[fileIndex];
                            ImGui::PushID(static_cast<int>(fileIndex));
                            const ImGuiStyle& style = ImGui::GetStyle();
                            const float rowStartX = ImGui::GetCursorPosX();
                            const float removeControlStartX = rowStartX
                                + style.IndentSpacing
                                + transformControlStartOffset();
                            const std::string label = fileDisplayName(file.path)
                                + "##file_" + std::to_string(fileIndex);
                            const size_t fileGroupCount = file.groupSettings.size();
                            const size_t fileVisibleCount = woby::countVisibleFileGroups(file);
                            if (drawTriStateVisibilityButton(
                                    "visible",
                                    "File",
                                    fileVisibleCount,
                                    fileGroupCount)) {
                                woby::setFileVisible(file, fileVisibleCount != fileGroupCount);
                            }
                            ImGui::SameLine();
                            const std::string tooltipText = file.path.string()
                                + "\n"
                                + meshCountLine(
                                    file.mesh.vertices.size(),
                                    file.mesh.indices.size() / 3u);
                            const bool fileTreeOpen = ImGui::TreeNode(label.c_str());
                            setLastItemTooltip(tooltipText.c_str());
                            ImGui::SameLine(removeControlStartX, 0.0f);
                            if (drawRenderModeIconButton(
                                    "remove",
                                    removeFileIcon,
                                    "Remove file from scene",
                                    RenderModeState::off,
                                    false)) {
                                removeFileIndex = fileIndex;
                            }
                            if (fileTreeOpen) {
                                drawGroupMasterControls(file.groupSettings);
                                ImGui::SameLine(0.0f, 0.0f);
                                const float translationSpeed = std::max(file.mesh.bounds.radius * 0.005f, 0.01f);
                                float fileVertexSizeScale = file.vertexSizeScale;
                                ImGui::SetNextItemWidth(renderModeButtonRowWidth());
                                pushRenderModeControlHeight();
                                if (ImGui::DragFloat(
                                    "##vertex_size",
                                    &fileVertexSizeScale,
                                    0.02f,
                                    woby::minVertexSizeScale,
                                    woby::maxVertexSizeScale,
                                    "%.2fx")) {
                                    woby::setFileVertexSizeScale(file, fileVertexSizeScale);
                                }
                                ImGui::PopStyleVar();
                                setLastItemTooltip("Vertex size multiplier for this file");
                                drawFileTransformControls(file.fileSettings, translationSpeed);
                                for (size_t nodeIndex = 0; nodeIndex < file.mesh.nodes.size(); ++nodeIndex) {
                                    const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
                                    drawGroupControls(
                                        file.mesh.nodes[nodeIndex],
                                        gpuMesh.nodeRanges[nodeIndex],
                                        file,
                                        nodeIndex,
                                        colorIndex + nodeIndex,
                                        translationSpeed);
                                }
                                ImGui::TreePop();
                            }
                            colorIndex += file.groupSettings.size();
                            ImGui::PopID();
                        }
                        if (removeFileIndex.has_value() && removeFileIndex.value() < files.size()) {
                            const std::string removedName = fileDisplayName(files[removeFileIndex.value()].path);
                            removeObjFile(ui, runtimes, removeFileIndex.value());
                            setToastMessage(toast, "Removed " + removedName);
                        }
                    }
                    ImGui::EndChild();
                }
            }
                ImGui::End();
            }
            drawViewerPaneTogglePane(ui, static_cast<float>(width));

            woby::recalculateSceneBounds(ui);
            woby::updateSceneDirty(ui, cleanSceneDocument);
            updateAppWindowTitle(window.get(), currentScenePath, ui.isDirty);

            const uint32_t sceneViewportWidth = std::max(width, 1u);
            bgfx::setViewRect(
                sceneView,
                0,
                0,
                static_cast<uint16_t>(sceneViewportWidth),
                static_cast<uint16_t>(height));
            bgfx::setViewRect(
                helperView,
                0,
                0,
                static_cast<uint16_t>(sceneViewportWidth),
                static_cast<uint16_t>(height));
            bgfx::touch(sceneView);
            bgfx::touch(helperView);

            float view[16];
            float projection[16];
            const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;
            bx::mtxLookAt(
                view,
                woby::cameraEye(camera),
                woby::cameraLookAt(camera),
                bx::Vec3(0.0f, 0.0f, 1.0f));
            bx::mtxProj(
                projection,
                camera.verticalFovDegrees,
                static_cast<float>(sceneViewportWidth) / static_cast<float>(height),
                camera.nearPlane,
                woby::cameraFarPlane(camera, sceneBounds),
                homogeneousDepth);
            bgfx::setViewTransform(sceneView, view, projection);
            bgfx::setViewTransform(helperView, view, projection);

            std::optional<HoveredVertex> hoveredVertex;
            const MousePosition mouse = mousePositionInPixels(window.get());
            if (mouse.x >= activeViewerPaneWidth
                && mouse.x < static_cast<float>(sceneViewportWidth)
                && mouse.y >= 0.0f
                && mouse.y < static_cast<float>(height)) {
                hoveredVertex = findHoveredVertex(
                    files,
                    runtimes,
                    mouse,
                    masterVertexPointSize,
                    view,
                    projection,
                    sceneViewportWidth,
                    height,
                    homogeneousDepth);
            }
            submitSceneFiles(
                files,
                runtimes,
                masterVertexPointSize,
                meshProgram,
                colorProgram,
                pointSpriteProgram,
                colorUniform,
                pointParamsUniform,
                sceneViewportWidth,
                height);
            submitSceneHelpers(ui, helperLayout, colorProgram, colorUniform);

            drawToastMessage(toast, width);
            drawHoveredVertexOverlay(hoveredVertex, width, height);
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
        destroyObjRuntimes(runtimes);
        bgfx::shutdown();
        bgfxInitialized = false;
        window.reset();
        SDL_Quit();
        sdlInitialized = false;
        spdlog::shutdown();

        return 0;
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "%s\n", exception.what());
        if (bgfxInitialized) {
            bgfx::shutdown();
        }
        if (sdlInitialized) {
            SDL_Quit();
        }
        spdlog::shutdown();
        return 1;
    }
}
