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
#include <cctype>
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
constexpr float defaultViewerPaneWidth = 360.0f;
constexpr float minViewerPaneWidth = 280.0f;
constexpr float defaultScenePaneHeight = 150.0f;
constexpr float minSceneViewportWidth = 160.0f;
constexpr float appFontSize = 15.0f;
constexpr const char* appFontFilename = "RobotoMonoNerdFont-Regular.ttf";
constexpr ImWchar appFontGlyphRanges[] = {
    0x0020,
    0x00ff,
    0xf04b,
    0xf04b,
    0xf192,
    0xf192,
    0xf068,
    0xf068,
    0xf0b2,
    0xf0b2,
    0xf1b2,
    0xf1b2,
    0,
};
constexpr const char* solidMeshIcon = "\xef\x86\xb2";
constexpr const char* trianglesIcon = "\xef\x81\x8b";
constexpr const char* verticesIcon = "\xef\x86\x92";
constexpr const char* transformIcon = "\xef\x82\xb2";
constexpr const char* mixedStateIcon = "\xef\x81\xa8";
constexpr float renderModeButtonSize = 26.0f;

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

struct FileRenderSettings {
    bool visible = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    std::array<float, 3> center{};
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
};

struct LoadedObjFile {
    std::filesystem::path path;
    woby::ObjMesh mesh;
    GpuMesh gpuMesh;
    std::vector<GroupRenderSettings> groupSettings;
    FileRenderSettings fileSettings;
    float vertexSizeScale = 1.0f;
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

size_t totalGroupCount(const std::vector<LoadedObjFile>& files)
{
    size_t groupCount = 0;
    for (const auto& file : files) {
        groupCount += file.groupSettings.size();
    }

    return groupCount;
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

size_t countEnabledFileSettings(
    const std::vector<LoadedObjFile>& files,
    bool GroupRenderSettings::*field)
{
    size_t enabledCount = 0;
    for (const auto& file : files) {
        enabledCount += countEnabledGroupSettings(file.groupSettings, field);
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

void setAllFileGroupSettings(
    std::vector<LoadedObjFile>& files,
    bool GroupRenderSettings::*field,
    bool enabled)
{
    for (auto& file : files) {
        setAllGroupSettings(file.groupSettings, field, enabled);
    }
}

void resetGroupTransform(GroupRenderSettings& settings)
{
    settings.scale = 1.0f;
    settings.opacity = 1.0f;
    settings.translation = {};
    settings.rotationDegrees = {};
}

void resetFileTransform(FileRenderSettings& settings)
{
    settings.scale = 1.0f;
    settings.opacity = 1.0f;
    settings.translation = {};
    settings.rotationDegrees = {};
}

std::array<float, 4> groupColor(
    const GroupRenderSettings& settings,
    float rgbScale,
    float opacityScale = 1.0f)
{
    auto color = scaledRgbColor(settings.color, rgbScale);
    color[3] = std::clamp(settings.opacity * opacityScale, minGroupOpacity, maxGroupOpacity);
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

void fileTransform(const FileRenderSettings& settings, float* model)
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

std::string fileDisplayName(const std::filesystem::path& path)
{
    const auto filename = path.filename().string();
    if (!filename.empty()) {
        return filename;
    }

    return path.string();
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

float renderModeButtonRowWidth()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return renderModeButtonSize * 3.0f + style.ItemSpacing.x * 2.0f;
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

bool groupTransformIsDefault(const GroupRenderSettings& settings)
{
    return settings.scale == 1.0f
        && settings.opacity == 1.0f
        && settings.translation == std::array<float, 3>{}
        && settings.rotationDegrees == std::array<float, 3>{};
}

bool fileTransformIsDefault(const FileRenderSettings& settings)
{
    return settings.scale == 1.0f
        && settings.opacity == 1.0f
        && settings.translation == std::array<float, 3>{}
        && settings.rotationDegrees == std::array<float, 3>{};
}

void drawMeshCountLine(size_t vertexCount, size_t triangleCount)
{
    ImGui::Text("Vertices: %zu  Triangles: %zu", vertexCount, triangleCount);
}

void drawGroupMasterControls(std::vector<GroupRenderSettings>& settings)
{
    const size_t groupCount = settings.size();
    const size_t solidMeshCount = countEnabledGroupSettings(
        settings,
        &GroupRenderSettings::showSolidMesh);
    if (drawTriStateMasterIconButton(
            "solid_mesh",
            solidMeshIcon,
            "Solid mesh",
            solidMeshCount,
            groupCount)) {
        setAllGroupSettings(
            settings,
            &GroupRenderSettings::showSolidMesh,
            solidMeshCount != groupCount);
    }
    ImGui::SameLine();
    const size_t triangleCount = countEnabledGroupSettings(
        settings,
        &GroupRenderSettings::showTriangles);
    if (drawTriStateMasterIconButton(
            "triangles",
            trianglesIcon,
            "Triangles",
            triangleCount,
            groupCount)) {
        setAllGroupSettings(
            settings,
            &GroupRenderSettings::showTriangles,
            triangleCount != groupCount);
    }
    ImGui::SameLine();
    const size_t vertexCount = countEnabledGroupSettings(
        settings,
        &GroupRenderSettings::showVertices);
    if (drawTriStateMasterIconButton(
            "vertices",
            verticesIcon,
            "Vertices",
            vertexCount,
            groupCount)) {
        setAllGroupSettings(
            settings,
            &GroupRenderSettings::showVertices,
            vertexCount != groupCount);
    }
}

void drawGroupControls(
    const woby::ObjNode& node,
    GroupRenderSettings& settings,
    size_t nodeIndex,
    size_t colorIndex,
    float translationSpeed)
{
    ImGui::PushID(static_cast<int>(nodeIndex));
    ImGui::Checkbox("##visible", &settings.visible);
    setLastItemTooltip("Show group");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(node.name.c_str());
    const std::string triangleTooltip = std::to_string(node.indexCount / 3u) + " triangles";
    setLastItemTooltip(triangleTooltip.c_str());
    ImGui::SameLine();
    if (drawRenderModeIconButton(
            "solid_mesh",
            solidMeshIcon,
            "Solid mesh for this group",
            settings.showSolidMesh ? RenderModeState::on : RenderModeState::off,
            false)) {
        settings.showSolidMesh = !settings.showSolidMesh;
    }
    ImGui::SameLine();
    if (drawRenderModeIconButton(
            "triangles",
            trianglesIcon,
            "Triangles for this group",
            settings.showTriangles ? RenderModeState::on : RenderModeState::off,
            false)) {
        settings.showTriangles = !settings.showTriangles;
    }
    ImGui::SameLine();
    if (drawRenderModeIconButton(
            "vertices",
            verticesIcon,
            "Vertices for this group",
            settings.showVertices ? RenderModeState::on : RenderModeState::off,
            false)) {
        settings.showVertices = !settings.showVertices;
    }
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SetNextItemWidth(70.0f);
    pushRenderModeControlHeight();
    ImGui::DragFloat(
        "##vertex_size",
        &settings.vertexSizeScale,
        0.02f,
        minVertexSizeScale,
        maxVertexSizeScale,
        "%.2fx");
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
    const RenderModeState transformState = groupTransformIsDefault(settings)
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
        ImGui::ColorPicker3("##color_picker", settings.color.data());
        if (ImGui::Button("Reset")) {
            settings.color = objGroupColor(colorIndex);
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("Transform")) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Transform geometry");
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            resetGroupTransform(settings);
        }
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat3(
            "Move",
            settings.translation.data(),
            translationSpeed);
        setLastItemTooltip("Position offset for this group");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat3(
            "Rotate",
            settings.rotationDegrees.data(),
            1.0f,
            -180.0f,
            180.0f,
            "%.0f deg");
        setLastItemTooltip("Rotation in degrees for this group");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat(
            "Scale",
            &settings.scale,
            0.01f,
            minGroupScale,
            maxGroupScale,
            "%.2fx");
        setLastItemTooltip("Uniform scale for this group");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderFloat(
            "Opacity",
            &settings.opacity,
            minGroupOpacity,
            maxGroupOpacity,
            "%.2f");
        setLastItemTooltip("Opacity for this group");
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void drawFileTransformControls(FileRenderSettings& settings, float translationSpeed)
{
    ImGui::SameLine();
    const RenderModeState transformState = fileTransformIsDefault(settings)
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
            resetFileTransform(settings);
        }
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat3(
            "Move",
            settings.translation.data(),
            translationSpeed);
        setLastItemTooltip("Position offset for this file");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat3(
            "Rotate",
            settings.rotationDegrees.data(),
            1.0f,
            -180.0f,
            180.0f,
            "%.0f deg");
        setLastItemTooltip("Rotation in degrees for this file");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::DragFloat(
            "Scale",
            &settings.scale,
            0.01f,
            minGroupScale,
            maxGroupScale,
            "%.2fx");
        setLastItemTooltip("Uniform scale for this file");
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderFloat(
            "Opacity",
            &settings.opacity,
            minGroupOpacity,
            maxGroupOpacity,
            "%.2f");
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

struct ModelPathOption {
    bool folder = false;
    std::filesystem::path path;
};

struct CommandLineOptions {
    bool showVersion = false;
    std::vector<ModelPathOption> inputPaths;
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

            ModelPathOption inputPath;
            inputPath.path = argv[++index];
            options.inputPaths.push_back(std::move(inputPath));
            continue;
        }

        if (argument == "--folder") {
            if (index + 1 >= argc) {
                throw std::runtime_error("--folder requires a folder path.");
            }

            ModelPathOption inputPath;
            inputPath.folder = true;
            inputPath.path = argv[++index];
            options.inputPaths.push_back(std::move(inputPath));
            continue;
        }

        if (argument.rfind("--", 0) == 0) {
            throw std::runtime_error("Unknown option: " + argument);
        }

        throw std::runtime_error("Unexpected argument: " + argument);
    }

    return options;
}

std::string lowercase(std::string value)
{
    for (char& character : value) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }

    return value;
}

bool isObjPath(const std::filesystem::path& path)
{
    return lowercase(path.extension().string()) == ".obj";
}

void appendFolderObjPaths(
    const std::filesystem::path& folder,
    std::vector<std::filesystem::path>& modelPaths)
{
    if (!std::filesystem::is_directory(folder)) {
        throw std::runtime_error("--folder path is not a folder: " + folder.string());
    }

    std::vector<std::filesystem::path> folderPaths;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        if (entry.is_regular_file() && isObjPath(entry.path())) {
            folderPaths.push_back(entry.path());
        }
    }

    std::sort(folderPaths.begin(), folderPaths.end());
    if (folderPaths.empty()) {
        throw std::runtime_error("--folder did not contain OBJ files: " + folder.string());
    }

    modelPaths.insert(modelPaths.end(), folderPaths.begin(), folderPaths.end());
}

std::vector<std::filesystem::path> resolveModelPaths(
    const CommandLineOptions& options,
    const std::filesystem::path& assets)
{
    std::vector<std::filesystem::path> modelPaths;
    if (options.inputPaths.empty()) {
        modelPaths.push_back(assets / "models" / "cube.obj");
        return modelPaths;
    }

    for (const auto& inputPath : options.inputPaths) {
        if (inputPath.folder) {
            appendFolderObjPaths(inputPath.path, modelPaths);
            continue;
        }

        modelPaths.push_back(inputPath.path);
    }

    return modelPaths;
}

woby::Bounds combineBounds(const std::vector<LoadedObjFile>& files)
{
    if (files.empty()) {
        throw std::runtime_error("No OBJ files were loaded.");
    }

    woby::Bounds bounds = files.front().mesh.bounds;
    for (const auto& file : files) {
        for (size_t axis = 0; axis < 3u; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], file.mesh.bounds.min[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], file.mesh.bounds.max[axis]);
        }
    }

    for (size_t axis = 0; axis < 3u; ++axis) {
        bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }

    float radiusSquared = 0.0f;
    for (const auto& file : files) {
        for (size_t corner = 0; corner < 8u; ++corner) {
            std::array<float, 3> position{};
            for (size_t axis = 0; axis < 3u; ++axis) {
                position[axis] = (corner & (size_t{1u} << axis)) != 0u
                    ? file.mesh.bounds.max[axis]
                    : file.mesh.bounds.min[axis];
            }

            const float x = position[0] - bounds.center[0];
            const float y = position[1] - bounds.center[1];
            const float z = position[2] - bounds.center[2];
            radiusSquared = std::max(radiusSquared, x * x + y * y + z * z);
        }
    }

    bounds.radius = std::max(std::sqrt(radiusSquared), 0.001f);
    return bounds;
}

std::vector<LoadedObjFile> loadObjFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout)
{
    std::vector<LoadedObjFile> files;
    files.reserve(modelPaths.size());
    size_t colorIndex = 0;

    for (const auto& modelPath : modelPaths) {
        LoadedObjFile file;
        file.path = modelPath;
        file.mesh = woby::loadObjMesh(modelPath);
        file.gpuMesh = createGpuMesh(file.mesh, meshLayout, pointSpriteLayout);
        file.groupSettings = createGroupRenderSettings(file.mesh);
        file.fileSettings.center = file.mesh.bounds.center;
        for (auto& group : file.groupSettings) {
            group.color = objGroupColor(colorIndex);
            ++colorIndex;
        }
        files.push_back(std::move(file));
    }

    return files;
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
        const auto modelPaths = resolveModelPaths(commandLine, assets);
        const auto layout = meshVertexLayout();
        const auto pointLayout = pointSpriteVertexLayout();
        std::vector<LoadedObjFile> files = loadObjFiles(modelPaths, layout, pointLayout);
        const woby::Bounds sceneBounds = combineBounds(files);
        bgfx::ProgramHandle meshProgram = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
        bgfx::ProgramHandle colorProgram = woby::loadProgram(assets, "vs_color.bin", "fs_color.bin");
        bgfx::ProgramHandle pointSpriteProgram = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_point_sprite.bin");
        bgfx::UniformHandle colorUniform = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
        bgfx::UniformHandle pointParamsUniform = bgfx::createUniform("u_pointParams", bgfx::UniformType::Vec4);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        loadAppFont(assets);
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL3_InitForOther(window.get())) {
            throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed.");
        }
        woby::imgui_bgfx::init(assets, imguiView);

        bool running = true;
        float masterVertexPointSize = 4.0f;
        woby::SceneCamera camera = woby::frameCameraBounds(sceneBounds);
        woby::CameraInput cameraInput;
        auto previousFrame = std::chrono::steady_clock::now();
        auto fpsWindowStart = previousFrame;
        int fpsFrameCount = 0;
        float fps = 0.0f;
        float viewerPaneWidth = defaultViewerPaneWidth;

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
                    camera = woby::frameCameraBounds(sceneBounds);
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

            const auto& bounds = sceneBounds;
            woby::updateCameraFromKeyboard(camera, bounds, deltaSeconds);

            const float maxViewerPaneWidth = std::max(
                minViewerPaneWidth,
                static_cast<float>(width) - minSceneViewportWidth);
            viewerPaneWidth = std::clamp(viewerPaneWidth, minViewerPaneWidth, maxViewerPaneWidth);

            uint32_t sceneViewportX = 0;
            if (width > 1u) {
                const auto panePixelWidth = static_cast<uint32_t>(std::lround(viewerPaneWidth));
                sceneViewportX = std::min(panePixelWidth, width - 1u);
            }
            const uint32_t sceneViewportWidth = std::max(width - sceneViewportX, 1u);

            bgfx::setViewRect(
                sceneView,
                static_cast<uint16_t>(sceneViewportX),
                0,
                static_cast<uint16_t>(sceneViewportWidth),
                static_cast<uint16_t>(height));
            bgfx::touch(sceneView);

            float view[16];
            float projection[16];
            bx::mtxLookAt(view, woby::cameraEye(camera), woby::cameraLookAt(camera));
            bx::mtxProj(
                projection,
                camera.verticalFovDegrees,
                static_cast<float>(sceneViewportWidth) / static_cast<float>(height),
                camera.nearPlane,
                woby::cameraFarPlane(camera, bounds),
                bgfx::getCaps()->homogeneousDepth);
            bgfx::setViewTransform(sceneView, view, projection);

            for (const auto& file : files) {
                if (!file.fileSettings.visible) {
                    continue;
                }

                float fileModel[16];
                fileTransform(file.fileSettings, fileModel);
                for (size_t nodeIndex = 0; nodeIndex < file.gpuMesh.nodeRanges.size(); ++nodeIndex) {
                    const auto& settings = file.groupSettings[nodeIndex];
                    if (!settings.visible) {
                        continue;
                    }

                    float groupModel[16];
                    float model[16];
                    groupTransform(settings, groupModel);
                    bx::mtxMul(model, fileModel, groupModel);
                    const auto& range = file.gpuMesh.nodeRanges[nodeIndex];
                    if (settings.showSolidMesh) {
                        submitTriangleRange(
                            file.gpuMesh,
                            meshProgram,
                            colorUniform,
                            model,
                            groupColor(settings, 1.0f, file.fileSettings.opacity),
                            range.triangleIndexOffset,
                            range.triangleIndexCount);
                    }
                    if (settings.showTriangles) {
                        submitColorRange(
                            file.gpuMesh,
                            file.gpuMesh.lineIndexBuffer,
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
                            file.gpuMesh,
                            pointSpriteProgram,
                            colorUniform,
                            pointParamsUniform,
                            model,
                            groupColor(settings, 1.5f, file.fileSettings.opacity),
                            static_cast<float>(pointSize),
                            sceneViewportWidth,
                            height,
                            range.pointSpriteIndexOffset,
                            range.pointSpriteIndexCount);
                    }
                }
            }

            bgfx::dbgTextClear();

            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
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
                viewerPaneWidth = std::clamp(
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
                        ImGui::Text("Renderer: %s", bgfx::getRendererName(bgfx::getRendererType()));
                        ImGui::Text("FPS: %.1f", fps);
                        size_t vertexCountTotal = 0;
                        size_t triangleCountTotal = 0;
                        for (const auto& file : files) {
                            vertexCountTotal += file.mesh.vertices.size();
                            triangleCountTotal += file.mesh.indices.size() / 3u;
                        }
                        drawMeshCountLine(vertexCountTotal, triangleCountTotal);
                        const size_t groupCount = totalGroupCount(files);
                        const size_t solidMeshCount = countEnabledFileSettings(
                            files,
                            &GroupRenderSettings::showSolidMesh);
                        if (drawTriStateMasterIconButton(
                                "solid_mesh",
                                solidMeshIcon,
                                "Solid mesh",
                                solidMeshCount,
                                groupCount)) {
                            setAllFileGroupSettings(
                                files,
                                &GroupRenderSettings::showSolidMesh,
                                solidMeshCount != groupCount);
                        }
                        ImGui::SameLine();
                        const size_t triangleCount = countEnabledFileSettings(
                            files,
                            &GroupRenderSettings::showTriangles);
                        if (drawTriStateMasterIconButton(
                                "triangles",
                                trianglesIcon,
                                "Triangles",
                                triangleCount,
                                groupCount)) {
                            setAllFileGroupSettings(
                                files,
                                &GroupRenderSettings::showTriangles,
                                triangleCount != groupCount);
                        }
                        ImGui::SameLine();
                        const size_t vertexCount = countEnabledFileSettings(
                            files,
                            &GroupRenderSettings::showVertices);
                        if (drawTriStateMasterIconButton(
                                "vertices",
                                verticesIcon,
                                "Vertices",
                                vertexCount,
                                groupCount)) {
                            setAllFileGroupSettings(
                                files,
                                &GroupRenderSettings::showVertices,
                                vertexCount != groupCount);
                        }
                        ImGui::SameLine(0.0f, 0.0f);
                        ImGui::SetNextItemWidth(renderModeButtonRowWidth());
                        pushRenderModeControlHeight();
                        ImGui::DragFloat(
                            "##vertex_size",
                            &masterVertexPointSize,
                            0.2f,
                            minVertexPointSize,
                            maxVertexPointSize,
                            "%.0f px");
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
                        for (size_t fileIndex = 0; fileIndex < files.size(); ++fileIndex) {
                            auto& file = files[fileIndex];
                            ImGui::PushID(static_cast<int>(fileIndex));
                            const std::string label = fileDisplayName(file.path)
                                + "##file_" + std::to_string(fileIndex);
                            ImGui::Checkbox("##visible", &file.fileSettings.visible);
                            setLastItemTooltip("Show file");
                            ImGui::SameLine();
                            const std::string pathText = file.path.string();
                            const bool fileTreeOpen = ImGui::TreeNode(label.c_str());
                            setLastItemTooltip(pathText.c_str());
                            if (fileTreeOpen) {
                                drawMeshCountLine(
                                    file.mesh.vertices.size(),
                                    file.mesh.indices.size() / 3u);
                                drawGroupMasterControls(file.groupSettings);
                                ImGui::SameLine(0.0f, 0.0f);
                                const float translationSpeed = std::max(file.mesh.bounds.radius * 0.005f, 0.01f);
                                ImGui::SetNextItemWidth(renderModeButtonRowWidth());
                                pushRenderModeControlHeight();
                                ImGui::DragFloat(
                                    "##vertex_size",
                                    &file.vertexSizeScale,
                                    0.02f,
                                    minVertexSizeScale,
                                    maxVertexSizeScale,
                                    "%.2fx");
                                ImGui::PopStyleVar();
                                setLastItemTooltip("Vertex size multiplier for this file");
                                drawFileTransformControls(file.fileSettings, translationSpeed);
                                for (size_t nodeIndex = 0; nodeIndex < file.mesh.nodes.size(); ++nodeIndex) {
                                    drawGroupControls(
                                        file.mesh.nodes[nodeIndex],
                                        file.groupSettings[nodeIndex],
                                        nodeIndex,
                                        colorIndex + nodeIndex,
                                        translationSpeed);
                                }
                                ImGui::TreePop();
                            }
                            colorIndex += file.groupSettings.size();
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();
                }
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
        for (auto& file : files) {
            destroyGpuMesh(file.gpuMesh);
        }
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
