#include "bgfx_helpers.h"
#include "background_load.h"
#include "camera.h"
#include "command_line.h"
#include "file_discovery.h"
#include "hover_pick.h"
#include "imgui_bgfx.h"
#include "model_load.h"
#include "native_dialogs.h"
#include "performance_log.h"
#include "scene_file.h"
#include "scene_renderer.h"
#include "scene_screenshot.h"
#include "ui_operations.h"
#include "ui_state.h"

#include <SDL3/SDL.h>
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
#include <atomic>
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
#include <thread>
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
constexpr const char* appFontFilename = "RobotoMonoNerdFont-Regular.ttf";
constexpr ImWchar appFontGlyphRanges[] = {
    0x0020,
    0x00ff,
    0xf04b,
    0xf04b,
    0xf00d,
    0xf00d,
    0xf030,
    0xf030,
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
constexpr const char* addModelFileIcon = "\xee\xa9\xbf";
constexpr const char* openSceneIcon = "\xee\xb6\x95";
constexpr const char* saveSceneIcon = "\xee\xb5\xb5";
constexpr const char* screenshotIcon = "\xef\x80\xb0";
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

std::array<float, 4> scaledRgbColor(const std::array<float, 4>& color, float scale)
{
    return {
        std::clamp(color[0] * scale, 0.0f, 1.0f),
        std::clamp(color[1] * scale, 0.0f, 1.0f),
        std::clamp(color[2] * scale, 0.0f, 1.0f),
        color[3],
    };
}

using GroupRenderSettings = woby::UiGroupState;
using FileRenderSettings = woby::UiFileSettings;
using LoadedModelFile = woby::UiFileState;
using woby::GpuNodeRange;
using woby::HoverPickCache;
using woby::HoveredVertex;
using woby::LoadedModelRuntime;
using woby::MousePosition;
using woby::ModelFileDialogState;
using woby::SceneFileDialogState;
using woby::SceneScreenshotDialogState;
using woby::SceneScreenshotRuntime;
using woby::completeSceneScreenshotReadback;
using woby::createGpuMesh;
using woby::destroyGpuMesh;
using woby::destroyModelRuntimes;
using woby::destroySceneScreenshotFramebuffer;
using woby::failSceneScreenshotCapture;
using woby::findHoveredVertex;
using woby::hoverPickSignature;
using woby::helperLineVertexLayout;
using woby::meshVertexLayout;
using woby::pointSpriteVertexLayout;
using woby::requestSceneScreenshotCapture;
using woby::submitSceneFiles;
using woby::submitSceneHelpers;
using woby::submitSceneScreenshotCapture;
using woby::vertexPointSize;

struct LoadedModelFileWithRuntime {
    LoadedModelFile file;
    LoadedModelRuntime runtime;
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

enum class AsyncLoadKind {
    appendModel,
    openScene,
};

struct AsyncLoadOutcome {
    AsyncLoadKind kind = AsyncLoadKind::appendModel;
    woby::ModelBatchCpuLoadResult modelBatch;
    woby::SceneCpuLoadResult scene;
    std::string error;
    bool failed = false;
};

struct BackgroundLoadRuntime {
    ~BackgroundLoadRuntime()
    {
        cancelRequested.store(true);
        if (worker.joinable()) {
            worker.join();
        }
    }

    std::mutex mutex;
    std::thread worker;
    std::optional<AsyncLoadOutcome> outcome;
    woby::BackgroundLoadProgress progress;
    std::atomic_bool cancelRequested = false;
    AsyncLoadKind kind = AsyncLoadKind::appendModel;
    bool active = false;
};

struct GpuFinalizeRuntime {
    AsyncLoadKind kind = AsyncLoadKind::appendModel;
    std::filesystem::path scenePath;
    woby::SceneDocument sceneDocument;
    std::vector<LoadedModelFile> files;
    std::vector<LoadedModelFile> finalizedFiles;
    std::vector<LoadedModelRuntime> finalizedRuntimes;
    size_t sourceFailedCount = 0;
    size_t sourceSkippedCount = 0;
    size_t gpuFailedCount = 0;
    std::string lastError;
    size_t nextFileIndex = 0;
    bool active = false;
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

bool drawViewerPaneTogglePane(woby::UiState& state, float windowWidth, bool screenshotDisabled)
{
    bool screenshotRequested = false;
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
        if (screenshotDisabled) {
            ImGui::BeginDisabled();
        }
        if (drawRenderModeIconButton(
                "scene_screenshot",
                screenshotIcon,
                "Save scene screenshot",
                RenderModeState::off,
                false)) {
            screenshotRequested = true;
        }
        if (screenshotDisabled) {
            ImGui::EndDisabled();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    return screenshotRequested;
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
    const woby::MeshNode& node,
    const GpuNodeRange& range,
    LoadedModelFile& file,
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

struct SdlDeleter {
    void operator()(SDL_Window* window) const noexcept
    {
        if (window != nullptr) {
            SDL_DestroyWindow(window);
        }
    }
};

double elapsedMilliseconds(woby::PerformanceClock::time_point start);

void appendFolderModelPaths(
    const std::filesystem::path& folder,
    std::vector<std::filesystem::path>& modelPaths)
{
    const auto start = woby::PerformanceClock::now();
    const std::vector<std::filesystem::path> folderPaths = woby::collectModelPathsRecursive(folder);
    spdlog::info(
        "perf folder_scan path=\"{}\" model_count={} duration_ms={}",
        folder.string(),
        folderPaths.size(),
        elapsedMilliseconds(start));
    if (folderPaths.empty()) {
        throw std::runtime_error("--folder did not contain model files recursively: " + folder.string());
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

double elapsedMilliseconds(woby::PerformanceClock::time_point start)
{
    return woby::millisecondsBetween(start, woby::PerformanceClock::now());
}

double timerTicksToMilliseconds(int64_t ticks, int64_t frequency)
{
    if (frequency <= 0) {
        return 0.0;
    }

    return (static_cast<double>(ticks) * 1000.0) / static_cast<double>(frequency);
}

void copyBgfxStats(woby::FrameTimings& timings)
{
    const bgfx::Stats* stats = bgfx::getStats();
    if (stats == nullptr) {
        return;
    }

    timings.bgfxCpuFrameMilliseconds = timerTicksToMilliseconds(stats->cpuTimeFrame, stats->cpuTimerFreq);
    timings.bgfxCpuSubmitMilliseconds =
        timerTicksToMilliseconds(stats->cpuTimeEnd - stats->cpuTimeBegin, stats->cpuTimerFreq);
    if (stats->gpuTimerFreq > 0 && stats->gpuTimeEnd > stats->gpuTimeBegin) {
        timings.bgfxGpuFrameMilliseconds =
            timerTicksToMilliseconds(stats->gpuTimeEnd - stats->gpuTimeBegin, stats->gpuTimerFreq);
        timings.hasBgfxGpuFrameMilliseconds = true;
    }
}

void recordFrameStage(
    woby::FrameTimings& timings,
    woby::FrameStage stage,
    woby::PerformanceClock::time_point& stageStart)
{
    const auto now = woby::PerformanceClock::now();
    timings.stageMilliseconds[static_cast<size_t>(stage)] += woby::millisecondsBetween(stageStart, now);
    stageStart = now;
}

std::vector<std::filesystem::path> resolveModelPaths(const woby::AppArguments& options)
{
    std::vector<std::filesystem::path> modelPaths;

    for (const auto& inputPath : options.inputPaths) {
        if (inputPath.folder) {
            appendFolderModelPaths(inputPath.path, modelPaths);
            continue;
        }

        modelPaths.push_back(inputPath.path);
    }

    return modelPaths;
}

LoadedModelFileWithRuntime loadModelFile(
    const std::filesystem::path& modelPath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    size_t firstColorIndex)
{
    const auto totalStart = woby::PerformanceClock::now();
    LoadedModelFileWithRuntime loaded;
    try {
        const auto parseStart = woby::PerformanceClock::now();
        woby::Mesh mesh = woby::loadModelMesh(modelPath);
        const double parseMilliseconds = elapsedMilliseconds(parseStart);

        loaded.file = woby::createUiFileState(modelPath, std::move(mesh), firstColorIndex);

        const auto gpuStart = woby::PerformanceClock::now();
        loaded.runtime.gpuMesh = createGpuMesh(loaded.file.mesh, meshLayout, pointSpriteLayout);
        const double gpuMilliseconds = elapsedMilliseconds(gpuStart);

        spdlog::info(
            "perf model_load path=\"{}\" vertices={} triangles={} groups={} parse_ms={} gpu_ms={} total_ms={}",
            modelPath.string(),
            loaded.file.mesh.vertices.size(),
            loaded.file.mesh.indices.size() / 3u,
            loaded.file.groupSettings.size(),
            parseMilliseconds,
            gpuMilliseconds,
            elapsedMilliseconds(totalStart));
    } catch (const std::exception& exception) {
        spdlog::info(
            "perf model_load_failed path=\"{}\" duration_ms={} error=\"{}\"",
            modelPath.string(),
            elapsedMilliseconds(totalStart),
            exception.what());
        throw;
    }

    return loaded;
}

std::vector<LoadedModelFile> loadModelFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    std::vector<LoadedModelRuntime>& runtimes,
    size_t firstColorIndex = 0)
{
    const auto start = woby::PerformanceClock::now();
    std::vector<LoadedModelFile> files;
    files.reserve(modelPaths.size());
    runtimes.reserve(modelPaths.size());
    size_t colorIndex = firstColorIndex;

    try {
        for (const auto& modelPath : modelPaths) {
            LoadedModelFileWithRuntime loaded = loadModelFile(modelPath, meshLayout, pointSpriteLayout, colorIndex);
            colorIndex += loaded.file.groupSettings.size();
            files.push_back(std::move(loaded.file));
            runtimes.push_back(std::move(loaded.runtime));
        }
    } catch (...) {
        destroyModelRuntimes(runtimes);
        throw;
    }

    spdlog::info(
        "perf model_load_batch requested_count={} loaded_count={} duration_ms={}",
        modelPaths.size(),
        files.size(),
        elapsedMilliseconds(start));
    return files;
}

void appendInitialModelFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes)
{
    if (modelPaths.empty()) {
        return;
    }

    const auto start = woby::PerformanceClock::now();
    std::vector<LoadedModelRuntime> loadedRuntimes;
    std::vector<LoadedModelFile> loadedFiles = loadModelFiles(
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
    spdlog::info(
        "perf append_initial_model_files requested_count={} duration_ms={}",
        modelPaths.size(),
        elapsedMilliseconds(start));
}

void removeModelFile(
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    size_t fileIndex)
{
    if (fileIndex >= runtimes.size()) {
        return;
    }

    destroyGpuMesh(runtimes[fileIndex].gpuMesh);
    runtimes.erase(runtimes.begin() + static_cast<std::ptrdiff_t>(fileIndex));
    (void)woby::removeFileFromState(state, fileIndex);
}

bool appendModelFiles(
    const std::vector<std::filesystem::path>& modelPaths,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    std::string& status)
{
    const auto start = woby::PerformanceClock::now();
    size_t addedCount = 0;
    size_t skippedCount = 0;
    size_t failedCount = 0;
    std::string lastError;
    size_t colorIndex = woby::totalGroupCount(state);
    state.files.reserve(state.files.size() + modelPaths.size());
    runtimes.reserve(runtimes.size() + modelPaths.size());

    for (const auto& modelPath : modelPaths) {
        if (!woby::isModelPath(modelPath)) {
            ++skippedCount;
            continue;
        }

        try {
            LoadedModelFileWithRuntime loaded = loadModelFile(modelPath, meshLayout, pointSpriteLayout, colorIndex);
            colorIndex += loaded.file.groupSettings.size();
            state.files.push_back(std::move(loaded.file));
            runtimes.push_back(std::move(loaded.runtime));
            ++addedCount;
        } catch (const std::exception& exception) {
            ++failedCount;
            lastError = exception.what();
        }
    }

    status = "Added " + std::to_string(addedCount) + " model file";
    if (addedCount != 1u) {
        status += "s";
    }
    if (skippedCount > 0u) {
        status += ", skipped " + std::to_string(skippedCount) + " non-model";
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

    spdlog::info(
        "perf append_model_files requested_count={} added_count={} skipped_count={} failed_count={} duration_ms={}",
        modelPaths.size(),
        addedCount,
        skippedCount,
        failedCount,
        elapsedMilliseconds(start));
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
    std::vector<std::filesystem::path> modelPaths;
    std::vector<std::filesystem::path> scenePaths;
    size_t unsupportedCount = 0;
    size_t emptyFolderCount = 0;
    size_t failedFolderCount = 0;
    std::string lastFolderError;
};

DroppedPathClassification classifyDroppedPaths(
    const std::vector<std::filesystem::path>& paths)
{
    const auto start = woby::PerformanceClock::now();
    DroppedPathClassification classification;

    for (const auto& path : paths) {
        std::error_code error;
        if (std::filesystem::is_directory(path, error)) {
            try {
                const std::vector<std::filesystem::path> folderModelPaths =
                    woby::collectModelPathsRecursive(path);
                if (folderModelPaths.empty()) {
                    ++classification.emptyFolderCount;
                } else {
                    classification.modelPaths.insert(
                        classification.modelPaths.end(),
                        folderModelPaths.begin(),
                        folderModelPaths.end());
                }
            } catch (const std::exception& exception) {
                ++classification.failedFolderCount;
                classification.lastFolderError = exception.what();
            }
            continue;
        }

        if (woby::isModelPath(path)) {
            classification.modelPaths.push_back(path);
            continue;
        }

        if (woby::isWobyPath(path)) {
            classification.scenePaths.push_back(path);
            continue;
        }

        ++classification.unsupportedCount;
    }

    spdlog::info(
        "perf dropped_path_classification input_count={} model_count={} scene_count={} unsupported_count={} empty_folder_count={} failed_folder_count={} duration_ms={}",
        paths.size(),
        classification.modelPaths.size(),
        classification.scenePaths.size(),
        classification.unsupportedCount,
        classification.emptyFolderCount,
        classification.failedFolderCount,
        elapsedMilliseconds(start));
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

std::vector<LoadedModelFile> loadSceneFiles(
    const std::filesystem::path& scenePath,
    const woby::SceneDocument& document,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    std::vector<LoadedModelRuntime>& runtimes)
{
    std::vector<LoadedModelFile> loadedFiles;
    std::vector<LoadedModelRuntime> loadedRuntimes;
    loadedFiles.reserve(document.files.size());
    loadedRuntimes.reserve(document.files.size());
    size_t colorIndex = 0;

    try {
        for (const auto& record : document.files) {
            const std::filesystem::path modelPath = woby::sceneAbsolutePath(scenePath, record.path);
            LoadedModelFileWithRuntime loaded = loadModelFile(modelPath, meshLayout, pointSpriteLayout, colorIndex);
            woby::applySceneFileRecord(loaded.file, record);
            colorIndex += loaded.file.groupSettings.size();
            loadedRuntimes.push_back(std::move(loaded.runtime));
            loadedFiles.push_back(std::move(loaded.file));
        }
    } catch (...) {
        destroyModelRuntimes(loadedRuntimes);
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
    std::vector<LoadedModelRuntime>& runtimes)
{
    const auto totalStart = woby::PerformanceClock::now();
    const auto readStart = woby::PerformanceClock::now();
    const woby::SceneDocument document = woby::readSceneDocument(scenePath);
    const double readMilliseconds = elapsedMilliseconds(readStart);

    const auto filesStart = woby::PerformanceClock::now();
    std::vector<LoadedModelRuntime> loadedRuntimes;
    std::vector<LoadedModelFile> loadedFiles = loadSceneFiles(
        scenePath,
        document,
        meshLayout,
        pointSpriteLayout,
        loadedRuntimes);
    const double filesMilliseconds = elapsedMilliseconds(filesStart);

    const auto applyStart = woby::PerformanceClock::now();
    destroyModelRuntimes(runtimes);
    runtimes = std::move(loadedRuntimes);
    state.files = std::move(loadedFiles);
    state.upAxis = document.upAxis;
    woby::setShowOrigin(state, document.showOrigin);
    woby::setShowGrid(state, document.showGrid);
    woby::setMasterVertexPointSize(state, document.masterVertexPointSize);
    woby::recalculateSceneBounds(state);
    state.camera = woby::frameCameraBounds(state.sceneBounds, state.upAxis);
    spdlog::info(
        "perf scene_load path=\"{}\" files={} read_ms={} files_ms={} apply_ms={} total_ms={}",
        scenePath.string(),
        state.files.size(),
        readMilliseconds,
        filesMilliseconds,
        elapsedMilliseconds(applyStart),
        elapsedMilliseconds(totalStart));
}

std::filesystem::path saveScene(
    const std::filesystem::path& requestedScenePath,
    const woby::UiState& state)
{
    const auto totalStart = woby::PerformanceClock::now();
    const std::filesystem::path scenePath = woby::sceneSavePathWithExtension(requestedScenePath);
    const auto documentStart = woby::PerformanceClock::now();
    const woby::SceneDocument document = woby::createSceneDocument(state);
    const double documentMilliseconds = elapsedMilliseconds(documentStart);

    const auto writeStart = woby::PerformanceClock::now();
    woby::writeSceneDocument(scenePath, document);
    const double writeMilliseconds = elapsedMilliseconds(writeStart);
    spdlog::info(
        "perf scene_save path=\"{}\" files={} document_ms={} write_ms={} total_ms={}",
        scenePath.string(),
        document.files.size(),
        documentMilliseconds,
        writeMilliseconds,
        elapsedMilliseconds(totalStart));
    return scenePath;
}

void loadSceneFromPath(
    const std::filesystem::path& requestedScenePath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument)
{
    const auto start = woby::PerformanceClock::now();
    const std::filesystem::path scenePath = normalizedPath(requestedScenePath);
    loadScene(scenePath, meshLayout, pointSpriteLayout, state, runtimes);
    currentScenePath = scenePath;
    cleanSceneDocument = woby::createSceneDocument(state);
    woby::clearSceneDirty(state);
    spdlog::info(
        "perf scene_open path=\"{}\" duration_ms={}",
        scenePath.string(),
        elapsedMilliseconds(start));
}

std::filesystem::path saveSceneToPath(
    const std::filesystem::path& requestedScenePath,
    woby::UiState& state,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument)
{
    const auto start = woby::PerformanceClock::now();
    const std::filesystem::path scenePath = normalizedPath(saveScene(requestedScenePath, state));
    currentScenePath = scenePath;
    cleanSceneDocument = woby::createSceneDocument(state);
    woby::clearSceneDirty(state);
    spdlog::info(
        "perf scene_save_total path=\"{}\" duration_ms={}",
        scenePath.string(),
        elapsedMilliseconds(start));
    return scenePath;
}

void setToastMessage(ToastMessage& toast, std::string text)
{
    toast.text = std::move(text);
    toast.startedAt = std::chrono::steady_clock::now();
}

void resetBackgroundLoadProgress(
    BackgroundLoadRuntime& load,
    AsyncLoadKind kind,
    size_t totalCount)
{
    std::lock_guard<std::mutex> lock(load.mutex);
    load.progress = woby::BackgroundLoadProgress{};
    load.progress.totalCount = totalCount;
    load.kind = kind;
    load.outcome.reset();
}

void updateBackgroundLoadProgress(
    BackgroundLoadRuntime& load,
    const woby::BackgroundLoadProgress& progress)
{
    std::lock_guard<std::mutex> lock(load.mutex);
    load.progress = progress;
}

bool startAppendModelBackgroundLoad(
    BackgroundLoadRuntime& load,
    std::vector<std::filesystem::path> modelPaths,
    size_t firstColorIndex)
{
    if (load.active) {
        return false;
    }

    if (load.worker.joinable()) {
        load.worker.join();
    }

    resetBackgroundLoadProgress(load, AsyncLoadKind::appendModel, modelPaths.size());
    load.cancelRequested.store(false);
    load.active = true;
    load.worker = std::thread([&load, modelPaths = std::move(modelPaths), firstColorIndex]() {
        AsyncLoadOutcome outcome;
        outcome.kind = AsyncLoadKind::appendModel;
        try {
            outcome.modelBatch = woby::loadModelBatchCpu(
                modelPaths,
                firstColorIndex,
                [&load](const woby::BackgroundLoadProgress& progress) {
                    updateBackgroundLoadProgress(load, progress);
                },
                [&load]() {
                    return load.cancelRequested.load();
                });
        } catch (const std::exception& exception) {
            outcome.failed = true;
            outcome.error = exception.what();
        }

        std::lock_guard<std::mutex> lock(load.mutex);
        load.outcome = std::move(outcome);
    });
    return true;
}

bool startOpenSceneBackgroundLoad(
    BackgroundLoadRuntime& load,
    const std::filesystem::path& requestedScenePath)
{
    if (load.active) {
        return false;
    }

    if (load.worker.joinable()) {
        load.worker.join();
    }

    const std::filesystem::path scenePath = normalizedPath(requestedScenePath);
    resetBackgroundLoadProgress(load, AsyncLoadKind::openScene, 0u);
    load.cancelRequested.store(false);
    load.active = true;
    load.worker = std::thread([&load, scenePath]() {
        AsyncLoadOutcome outcome;
        outcome.kind = AsyncLoadKind::openScene;
        try {
            outcome.scene = woby::loadSceneCpu(
                scenePath,
                [&load](const woby::BackgroundLoadProgress& progress) {
                    updateBackgroundLoadProgress(load, progress);
                },
                [&load]() {
                    return load.cancelRequested.load();
                });
        } catch (const std::exception& exception) {
            outcome.failed = true;
            outcome.error = exception.what();
        }

        std::lock_guard<std::mutex> lock(load.mutex);
        load.outcome = std::move(outcome);
    });
    return true;
}

std::optional<AsyncLoadOutcome> takeBackgroundLoadOutcome(BackgroundLoadRuntime& load)
{
    std::optional<AsyncLoadOutcome> outcome;
    {
        std::lock_guard<std::mutex> lock(load.mutex);
        outcome = std::move(load.outcome);
        load.outcome.reset();
    }

    if (!outcome.has_value()) {
        return {};
    }

    if (load.worker.joinable()) {
        load.worker.join();
    }
    load.active = false;
    load.cancelRequested.store(false);
    return outcome;
}

void startGpuFinalize(GpuFinalizeRuntime& finalize, AsyncLoadOutcome outcome)
{
    finalize = GpuFinalizeRuntime{};
    finalize.kind = outcome.kind;
    if (outcome.kind == AsyncLoadKind::appendModel) {
        finalize.files = std::move(outcome.modelBatch.files);
        finalize.sourceFailedCount = outcome.modelBatch.failedCount;
        finalize.sourceSkippedCount = outcome.modelBatch.skippedCount;
        finalize.lastError = std::move(outcome.modelBatch.lastError);
    } else {
        finalize.scenePath = std::move(outcome.scene.scenePath);
        finalize.sceneDocument = std::move(outcome.scene.document);
        finalize.files = std::move(outcome.scene.files);
    }
    finalize.finalizedFiles.reserve(finalize.files.size());
    finalize.finalizedRuntimes.reserve(finalize.files.size());
    finalize.active = true;
}

std::string appendFinalizeStatus(const GpuFinalizeRuntime& finalize)
{
    const size_t failedCount = finalize.sourceFailedCount + finalize.gpuFailedCount;
    std::string status = "Added " + std::to_string(finalize.finalizedFiles.size()) + " model file";
    if (finalize.finalizedFiles.size() != 1u) {
        status += "s";
    }
    if (finalize.sourceSkippedCount > 0u) {
        status += ", skipped " + std::to_string(finalize.sourceSkippedCount) + " non-model";
    }
    if (failedCount > 0u) {
        status += ", failed " + std::to_string(failedCount);
        if (!finalize.lastError.empty()) {
            status += ": " + finalize.lastError;
        }
    }
    return status;
}

void abortGpuFinalize(GpuFinalizeRuntime& finalize)
{
    destroyModelRuntimes(finalize.finalizedRuntimes);
    finalize = GpuFinalizeRuntime{};
}

void commitGpuFinalize(
    GpuFinalizeRuntime& finalize,
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument,
    ToastMessage& toast)
{
    if (finalize.kind == AsyncLoadKind::appendModel) {
        const bool addedAnyFiles = !finalize.finalizedFiles.empty();
        state.files.insert(
            state.files.end(),
            std::make_move_iterator(finalize.finalizedFiles.begin()),
            std::make_move_iterator(finalize.finalizedFiles.end()));
        runtimes.insert(
            runtimes.end(),
            std::make_move_iterator(finalize.finalizedRuntimes.begin()),
            std::make_move_iterator(finalize.finalizedRuntimes.end()));
        if (addedAnyFiles) {
            woby::recalculateSceneBounds(state);
            woby::frameCameraToScene(state);
            woby::markSceneDirty(state);
        }
        setToastMessage(toast, appendFinalizeStatus(finalize));
    } else {
        destroyModelRuntimes(runtimes);
        runtimes = std::move(finalize.finalizedRuntimes);
        state.files = std::move(finalize.finalizedFiles);
        state.upAxis = finalize.sceneDocument.upAxis;
        woby::setShowOrigin(state, finalize.sceneDocument.showOrigin);
        woby::setShowGrid(state, finalize.sceneDocument.showGrid);
        woby::setMasterVertexPointSize(state, finalize.sceneDocument.masterVertexPointSize);
        woby::recalculateSceneBounds(state);
        state.camera = woby::frameCameraBounds(state.sceneBounds, state.upAxis);
        currentScenePath = finalize.scenePath;
        cleanSceneDocument = woby::createSceneDocument(state);
        woby::clearSceneDirty(state);
        setToastMessage(toast, "Opened scene " + fileDisplayName(finalize.scenePath));
    }

    finalize = GpuFinalizeRuntime{};
}

void processGpuFinalizeStep(
    GpuFinalizeRuntime& finalize,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument,
    ToastMessage& toast)
{
    if (!finalize.active) {
        return;
    }

    if (finalize.nextFileIndex >= finalize.files.size()) {
        commitGpuFinalize(finalize, state, runtimes, currentScenePath, cleanSceneDocument, toast);
        return;
    }

    LoadedModelFile file = std::move(finalize.files[finalize.nextFileIndex]);
    ++finalize.nextFileIndex;
    try {
        LoadedModelRuntime runtime;
        runtime.gpuMesh = createGpuMesh(file.mesh, meshLayout, pointSpriteLayout);
        finalize.finalizedRuntimes.push_back(std::move(runtime));
        finalize.finalizedFiles.push_back(std::move(file));
    } catch (const std::exception& exception) {
        ++finalize.gpuFailedCount;
        finalize.lastError = exception.what();
        if (finalize.kind == AsyncLoadKind::openScene) {
            setToastMessage(toast, std::string("Open scene failed: ") + exception.what());
            abortGpuFinalize(finalize);
        }
    }
}

void openSceneOrRequestDirtyWarning(
    const std::filesystem::path& scenePath,
    woby::UiState& state,
    BackgroundLoadRuntime& backgroundLoad,
    GpuFinalizeRuntime& gpuFinalize,
    SceneFileDialogState& sceneFileDialogState,
    ToastMessage& toast,
    std::optional<std::filesystem::path>& pendingDirtyOpenScenePath,
    bool& requestDirtyOpenWarning)
{
    if (backgroundLoad.active || gpuFinalize.active) {
        setSceneFileDialogStatus(sceneFileDialogState, "Already processing files");
        return;
    }

    if (state.isDirty) {
        pendingDirtyOpenScenePath = normalizedPath(scenePath);
        requestDirtyOpenWarning = true;
        return;
    }

    if (!startOpenSceneBackgroundLoad(backgroundLoad, scenePath)) {
        setSceneFileDialogStatus(
            sceneFileDialogState,
            "Open scene failed: already processing files");
    } else {
        setToastMessage(toast, "Opening scene " + fileDisplayName(scenePath));
    }
}

void processDroppedPaths(
    const std::vector<std::filesystem::path>& paths,
    woby::UiState& state,
    BackgroundLoadRuntime& backgroundLoad,
    GpuFinalizeRuntime& gpuFinalize,
    SceneFileDialogState& sceneFileDialogState,
    ToastMessage& toast,
    std::optional<std::filesystem::path>& pendingDirtyOpenScenePath,
    bool& requestDirtyOpenWarning)
{
    const auto start = woby::PerformanceClock::now();
    const DroppedPathClassification classification = classifyDroppedPaths(paths);
    const size_t extraPathCount = classification.modelPaths.size()
        + classification.unsupportedCount
        + classification.emptyFolderCount
        + classification.failedFolderCount;

    if (!classification.scenePaths.empty()) {
        if (classification.scenePaths.size() != 1u || extraPathCount > 0u) {
            setToastMessage(toast, "Drop one .woby scene file by itself");
            spdlog::info(
                "perf dropped_paths_processed input_count={} duration_ms={}",
                paths.size(),
                elapsedMilliseconds(start));
            return;
        }

        openSceneOrRequestDirtyWarning(
            classification.scenePaths[0],
            state,
            backgroundLoad,
            gpuFinalize,
            sceneFileDialogState,
            toast,
            pendingDirtyOpenScenePath,
            requestDirtyOpenWarning);
        spdlog::info(
            "perf dropped_paths_processed input_count={} duration_ms={}",
            paths.size(),
            elapsedMilliseconds(start));
        return;
    }

    std::string status;
    if (classification.modelPaths.empty()) {
        status = "No model files added";
    } else {
        if (!startAppendModelBackgroundLoad(
                backgroundLoad,
                classification.modelPaths,
                woby::totalGroupCount(state))) {
            status = "Already processing files";
        }
    }
    appendDropClassificationStatus(status, classification);
    if (!status.empty()) {
        setToastMessage(toast, std::move(status));
    }
    spdlog::info(
        "perf dropped_paths_processed input_count={} duration_ms={}",
        paths.size(),
        elapsedMilliseconds(start));
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

bool drawProcessingDialog(BackgroundLoadRuntime& backgroundLoad, GpuFinalizeRuntime& gpuFinalize)
{
    bool backgroundActive = false;
    bool finalizingActive = gpuFinalize.active;
    AsyncLoadKind kind = AsyncLoadKind::appendModel;
    woby::BackgroundLoadProgress progress;
    {
        std::lock_guard<std::mutex> lock(backgroundLoad.mutex);
        backgroundActive = backgroundLoad.active;
        kind = backgroundLoad.kind;
        progress = backgroundLoad.progress;
    }

    if (!backgroundActive && !finalizingActive) {
        return false;
    }

    ImGui::OpenPopup("Processing files");
    bool modalOpen = false;
    if (ImGui::BeginPopupModal(
            "Processing files",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize)) {
        modalOpen = true;
        if (backgroundActive) {
            ImGui::TextUnformatted(kind == AsyncLoadKind::openScene ? "Opening scene..." : "Loading model files...");
            if (!progress.currentPath.empty()) {
                ImGui::Text(
                    "%s",
                    fileDisplayName(progress.currentPath).c_str());
            }
            if (progress.totalCount > 0u) {
                const float fraction = static_cast<float>(std::min(progress.completedCount, progress.totalCount))
                    / static_cast<float>(progress.totalCount);
                ImGui::ProgressBar(
                    fraction,
                    ImVec2(260.0f, 0.0f),
                    (std::to_string(progress.completedCount) + " / " + std::to_string(progress.totalCount)).c_str());
            } else {
                ImGui::TextUnformatted("Preparing...");
            }
            if (ImGui::Button("Cancel")) {
                backgroundLoad.cancelRequested.store(true);
            }
        } else if (finalizingActive) {
            ImGui::TextUnformatted("Finalizing GPU resources...");
            if (gpuFinalize.nextFileIndex < gpuFinalize.files.size()) {
                ImGui::Text(
                    "%s",
                    fileDisplayName(gpuFinalize.files[gpuFinalize.nextFileIndex].path).c_str());
            }
            if (!gpuFinalize.files.empty()) {
                const float fraction = static_cast<float>(std::min(gpuFinalize.nextFileIndex, gpuFinalize.files.size()))
                    / static_cast<float>(gpuFinalize.files.size());
                ImGui::ProgressBar(
                    fraction,
                    ImVec2(260.0f, 0.0f),
                    (std::to_string(gpuFinalize.nextFileIndex) + " / " + std::to_string(gpuFinalize.files.size())).c_str());
            } else {
                ImGui::TextUnformatted("Committing scene...");
            }
        }
        ImGui::EndPopup();
    }

    return modalOpen;
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
        const auto startupStart = woby::PerformanceClock::now();
        if (commandLine.showVersion) {
            std::printf("%s\n", WOBY_VERSION);
            spdlog::shutdown();
            return 0;
        }

        const auto sdlStart = woby::PerformanceClock::now();
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
        woby::logDuration("startup_sdl_window", elapsedMilliseconds(sdlStart));

        const auto bgfxStart = woby::PerformanceClock::now();
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
        woby::logDuration("startup_bgfx", elapsedMilliseconds(bgfxStart));

        const auto assets = assetRoot();
        const auto modelPathsStart = woby::PerformanceClock::now();
        const auto modelPaths = resolveModelPaths(commandLine);
        woby::logDuration("startup_resolve_model_paths", elapsedMilliseconds(modelPathsStart));
        const auto layout = meshVertexLayout();
        const auto pointLayout = pointSpriteVertexLayout();
        const auto helperLayout = helperLineVertexLayout();
        woby::UiState ui;
        std::vector<LoadedModelRuntime> runtimes;
        std::optional<std::filesystem::path> currentScenePath;
        woby::SceneDocument cleanSceneDocument;

        const auto initialLoadStart = woby::PerformanceClock::now();
        if (commandLine.scenePath.has_value()) {
            loadSceneFromPath(
                commandLine.scenePath.value(),
                layout,
                pointLayout,
                ui,
                runtimes,
                currentScenePath,
                cleanSceneDocument);
            appendInitialModelFiles(modelPaths, layout, pointLayout, ui, runtimes);
        } else {
            ui.files = loadModelFiles(modelPaths, layout, pointLayout, runtimes);
            woby::recalculateSceneBounds(ui);
            woby::updateSceneDirty(ui, cleanSceneDocument);
        }
        woby::logDuration("startup_initial_scene", elapsedMilliseconds(initialLoadStart));

        const auto shaderStart = woby::PerformanceClock::now();
        bgfx::ProgramHandle meshProgram = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
        bgfx::ProgramHandle colorProgram = woby::loadProgram(assets, "vs_color.bin", "fs_color.bin");
        bgfx::ProgramHandle pointSpriteProgram = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_point_sprite.bin");
        bgfx::UniformHandle colorUniform = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
        bgfx::UniformHandle pointParamsUniform = bgfx::createUniform("u_pointParams", bgfx::UniformType::Vec4);
        woby::logDuration("startup_shaders", elapsedMilliseconds(shaderStart));

        const auto imguiStart = woby::PerformanceClock::now();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        loadAppFont(assets);
        configureAppStyle();

        if (!ImGui_ImplSDL3_InitForOther(window.get())) {
            throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed.");
        }
        woby::imgui_bgfx::init(assets, imguiView);
        woby::logDuration("startup_imgui", elapsedMilliseconds(imguiStart));

        ui.camera = woby::frameCameraBounds(ui.sceneBounds, ui.upAxis);
        ui.viewerPaneWidth = minimumViewerPaneWidth();
        woby::logDuration("startup_total", elapsedMilliseconds(startupStart));
        auto& running = ui.running;
        auto& files = ui.files;
        auto& sceneBounds = ui.sceneBounds;
        auto& camera = ui.camera;
        auto& cameraInput = ui.cameraInput;
        auto& masterVertexPointSize = ui.masterVertexPointSize;
        auto& viewerPaneWidth = ui.viewerPaneWidth;
        static ModelFileDialogState modelFileDialogState;
        static SceneFileDialogState sceneFileDialogState;
        static SceneScreenshotDialogState sceneScreenshotDialogState;
        BackgroundLoadRuntime backgroundLoad;
        GpuFinalizeRuntime gpuFinalize;
        SceneScreenshotRuntime sceneScreenshot;
        DragDropState dragDropState;
        std::optional<std::filesystem::path> pendingDirtyOpenScenePath;
        bool requestDirtyOpenWarning = false;
        bool requestDirtyQuitWarning = false;
        ToastMessage toast;
        uint64_t observedModelFileDialogStatusVersion = 0;
        uint64_t observedSceneFileDialogStatusVersion = 0;
        uint64_t observedSceneScreenshotDialogStatusVersion = 0;
        auto previousFrame = std::chrono::steady_clock::now();
        auto fpsWindowStart = previousFrame;
        int fpsFrameCount = 0;
        float fps = 0.0f;
        woby::FrameTimingAccumulator frameTimingAccumulator;
        uint64_t frameIndex = 0;
        HoverPickCache hoverPickCache;
        while (running) {
            woby::FrameTimings frameTimings;
            frameTimings.frameIndex = ++frameIndex;
            const auto frameStart = woby::PerformanceClock::now();
            auto stageStart = frameStart;

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
                        const bool altPressed = (SDL_GetModState() & SDL_KMOD_ALT) != 0u;
                        if (altPressed) {
                            woby::setCameraRolling(ui, true);
                        } else {
                            woby::setCameraOrbiting(ui, true);
                        }
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        woby::setCameraPanning(ui, true);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        woby::setCameraOrbiting(ui, false);
                        woby::setCameraRolling(ui, false);
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        woby::setCameraPanning(ui, false);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    if (cameraInput.orbiting) {
                        woby::orbitUiCamera(ui, event.motion.xrel, event.motion.yrel);
                    }
                    if (cameraInput.rolling) {
                        woby::rollUiCamera(ui, event.motion.xrel);
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
            recordFrameStage(frameTimings, woby::FrameStage::events, stageStart);

            getDrawableSize(window.get(), width, height);

            if (auto outcome = takeBackgroundLoadOutcome(backgroundLoad); outcome.has_value()) {
                if (outcome->failed) {
                    const std::string prefix = outcome->kind == AsyncLoadKind::openScene
                        ? "Open scene failed: "
                        : "Open model files failed: ";
                    setToastMessage(toast, prefix + outcome->error);
                } else if (outcome->kind == AsyncLoadKind::appendModel) {
                    if (outcome->modelBatch.canceled || outcome->modelBatch.files.empty()) {
                        setToastMessage(toast, outcome->modelBatch.status);
                    } else {
                        startGpuFinalize(gpuFinalize, std::move(outcome.value()));
                    }
                } else if (outcome->scene.canceled) {
                    setToastMessage(toast, "Open scene canceled");
                } else {
                    startGpuFinalize(gpuFinalize, std::move(outcome.value()));
                }
            }

            processGpuFinalizeStep(
                gpuFinalize,
                layout,
                pointLayout,
                ui,
                runtimes,
                currentScenePath,
                cleanSceneDocument,
                toast);

            const bool processingFiles = backgroundLoad.active || gpuFinalize.active;
            const auto pendingModelPaths = takePendingModelPaths(modelFileDialogState);
            if (!pendingModelPaths.empty()) {
                if (processingFiles) {
                    setModelFileDialogStatus(modelFileDialogState, "Already processing files");
                } else if (!startAppendModelBackgroundLoad(
                               backgroundLoad,
                               pendingModelPaths,
                               woby::totalGroupCount(ui))) {
                    setModelFileDialogStatus(modelFileDialogState, "Open model files failed: already processing files");
                }
            }
            const std::string modelDialogStatus = modelFileDialogStatus(
                modelFileDialogState,
                observedModelFileDialogStatusVersion);
            if (!modelDialogStatus.empty()) {
                setToastMessage(toast, modelDialogStatus);
            }

            const auto pendingOpenScenePath = takePendingOpenScenePath(sceneFileDialogState);
            if (pendingOpenScenePath.has_value()) {
                openSceneOrRequestDirtyWarning(
                    pendingOpenScenePath.value(),
                    ui,
                    backgroundLoad,
                    gpuFinalize,
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

            const auto pendingSceneScreenshotPath = takePendingSaveSceneScreenshotPath(sceneScreenshotDialogState);
            if (pendingSceneScreenshotPath.has_value()) {
                try {
                    requestSceneScreenshotCapture(sceneScreenshot, pendingSceneScreenshotPath.value());
                    setToastMessage(toast, "Saving screenshot...");
                } catch (const std::exception& exception) {
                    setSceneScreenshotDialogStatus(
                        sceneScreenshotDialogState,
                        std::string("Save screenshot failed: ") + exception.what());
                }
            }
            const std::string screenshotDialogStatus = sceneScreenshotDialogStatus(
                sceneScreenshotDialogState,
                observedSceneScreenshotDialogStatusVersion);
            if (!screenshotDialogStatus.empty()) {
                setToastMessage(toast, screenshotDialogStatus);
            }

            const auto droppedPaths = takePendingDropPaths(dragDropState);
            if (!droppedPaths.empty()) {
                if (processingFiles) {
                    setToastMessage(toast, "Already processing files");
                } else {
                    processDroppedPaths(
                        droppedPaths,
                        ui,
                        backgroundLoad,
                        gpuFinalize,
                        sceneFileDialogState,
                        toast,
                        pendingDirtyOpenScenePath,
                        requestDirtyOpenWarning);
                }
            }
            recordFrameStage(frameTimings, woby::FrameStage::pendingIo, stageStart);

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
            woby::updateCameraFromKeyboard(camera, bounds, deltaSeconds, ui.upAxis);

            const float minViewerPaneWidth = minimumViewerPaneWidth();
            const float maxViewerPaneWidth = std::max(
                minViewerPaneWidth,
                static_cast<float>(width) - minSceneViewportWidth);
            woby::setViewerPaneWidth(ui, viewerPaneWidth, minViewerPaneWidth, maxViewerPaneWidth);

            bgfx::dbgTextClear();
            recordFrameStage(frameTimings, woby::FrameStage::stateUpdate, stageStart);

            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            bool modalDialogOpen = false;
            if (requestDirtyOpenWarning) {
                ImGui::OpenPopup("Unsaved scene changes");
                requestDirtyOpenWarning = false;
            }
            if (ImGui::BeginPopupModal(
                    "Unsaved scene changes",
                    nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize)) {
                modalDialogOpen = true;
                ImGui::TextUnformatted("The current scene has unsaved changes.");
                if (pendingDirtyOpenScenePath.has_value()) {
                    ImGui::Text(
                        "Open %s and discard them?",
                        fileDisplayName(pendingDirtyOpenScenePath.value()).c_str());
                }
                if (ImGui::Button("Open")) {
                    if (pendingDirtyOpenScenePath.has_value()) {
                        const auto scenePath = pendingDirtyOpenScenePath.value();
                        if (!startOpenSceneBackgroundLoad(backgroundLoad, scenePath)) {
                            setSceneFileDialogStatus(
                                sceneFileDialogState,
                                "Open scene failed: already processing files");
                        } else {
                            setToastMessage(toast, "Opening scene " + fileDisplayName(scenePath));
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
                modalDialogOpen = true;
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
            if (drawProcessingDialog(backgroundLoad, gpuFinalize)) {
                modalDialogOpen = true;
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
                        const bool fileDialogOpen = modelFileDialogIsOpen(modelFileDialogState);
                        const bool sceneDialogOpen = sceneFileDialogIsOpen(sceneFileDialogState);
                        const bool screenshotDialogOpen = sceneScreenshotDialogIsOpen(sceneScreenshotDialogState);
                        const bool anyFileDialogOpen = fileDialogOpen
                            || sceneDialogOpen
                            || screenshotDialogOpen
                            || backgroundLoad.active
                            || gpuFinalize.active;
                        if (anyFileDialogOpen) {
                            ImGui::BeginDisabled();
                        }
                        if (ImGui::Button(
                                std::string(addModelFileIcon).append("##add_model_file").c_str(),
                                ImVec2(renderModeButtonSize, renderModeButtonSize))) {
                            showModelFileDialog(window.get(), modelFileDialogState);
                        }
                        if (anyFileDialogOpen) {
                            ImGui::EndDisabled();
                        }
                        setLastItemTooltip("Add model files");
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
                                ui.showGrid ? "Hide ground grid" : "Show ground grid",
                                ui.showGrid ? RenderModeState::on : RenderModeState::off,
                                false)) {
                            woby::toggleShowGrid(ui);
                        }
                        ImGui::SameLine();
                        const bool yUp = ui.upAxis == woby::SceneUpAxis::y;
                        if (drawRenderModeIconButton(
                                "up_axis",
                                yUp ? "Y" : "Z",
                                yUp ? "Use Z as scene up axis" : "Use Y as scene up axis",
                                RenderModeState::on,
                                false)) {
                            woby::toggleSceneUpAxis(ui);
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
                            removeModelFile(ui, runtimes, removeFileIndex.value());
                            setToastMessage(toast, "Removed " + removedName);
                        }
                    }
                    ImGui::EndChild();
                }
            }
                ImGui::End();
            }
            const bool screenshotActionDisabled = modelFileDialogIsOpen(modelFileDialogState)
                || sceneFileDialogIsOpen(sceneFileDialogState)
                || sceneScreenshotDialogIsOpen(sceneScreenshotDialogState)
                || backgroundLoad.active
                || gpuFinalize.active
                || sceneScreenshot.captureRequested
                || sceneScreenshot.readbackPending;
            if (drawViewerPaneTogglePane(ui, static_cast<float>(width), screenshotActionDisabled)) {
                showSaveSceneScreenshotDialog(window.get(), sceneScreenshotDialogState);
            }
            recordFrameStage(frameTimings, woby::FrameStage::imguiBuild, stageStart);

            woby::recalculateSceneBounds(ui);
            woby::updateSceneDirty(ui, cleanSceneDocument);
            updateAppWindowTitle(window.get(), currentScenePath, ui.isDirty);
            recordFrameStage(frameTimings, woby::FrameStage::sceneState, stageStart);

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
                woby::cameraEye(camera, ui.upAxis),
                woby::cameraLookAt(camera),
                woby::cameraUp(camera, ui.upAxis));
            bx::mtxProj(
                projection,
                camera.verticalFovDegrees,
                static_cast<float>(sceneViewportWidth) / static_cast<float>(height),
                camera.nearPlane,
                woby::cameraFarPlane(camera, sceneBounds),
                homogeneousDepth);
            bgfx::setViewTransform(sceneView, view, projection);
            bgfx::setViewTransform(helperView, view, projection);
            recordFrameStage(frameTimings, woby::FrameStage::viewSetup, stageStart);

            std::optional<HoveredVertex> hoveredVertex;
            const MousePosition mouse = mousePositionInPixels(window.get());
            const bool mouseInsideViewport = mouse.x >= activeViewerPaneWidth
                && mouse.x < static_cast<float>(sceneViewportWidth)
                && mouse.y >= 0.0f
                && mouse.y < static_cast<float>(height);
            const bool cameraInteractionActive = cameraInput.orbiting
                || cameraInput.rolling
                || cameraInput.panning;
            const bool nativeFileDialogOpen = modelFileDialogIsOpen(modelFileDialogState)
                || sceneFileDialogIsOpen(sceneFileDialogState)
                || sceneScreenshotDialogIsOpen(sceneScreenshotDialogState);
            const bool dialogOpen = modalDialogOpen
                || nativeFileDialogOpen
                || backgroundLoad.active
                || gpuFinalize.active;
            const bool hoverPickingEnabled = mouseInsideViewport
                && !cameraInteractionActive
                && !dialogOpen;
            if (!hoverPickingEnabled) {
                hoverPickCache.hoveredVertex.reset();
                hoverPickCache.valid = false;
            } else {
                const uint64_t hoverSignature = hoverPickSignature(
                    files,
                    runtimes,
                    mouse,
                    mouseInsideViewport,
                    masterVertexPointSize,
                    camera,
                    ui.upAxis,
                    sceneBounds,
                    sceneViewportWidth,
                    height,
                    homogeneousDepth);
                if (!hoverPickCache.valid || hoverPickCache.signature != hoverSignature) {
                    hoverPickCache.hoveredVertex.reset();
                    hoverPickCache.hoveredVertex = findHoveredVertex(
                        files,
                        runtimes,
                        mouse,
                        masterVertexPointSize,
                        view,
                        projection,
                        sceneViewportWidth,
                        height,
                        homogeneousDepth);
                    hoverPickCache.signature = hoverSignature;
                    hoverPickCache.valid = true;
                }
            }
            hoveredVertex = hoverPickCache.hoveredVertex;
            recordFrameStage(frameTimings, woby::FrameStage::hoverPick, stageStart);

            submitSceneFiles(
                sceneView,
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
            recordFrameStage(frameTimings, woby::FrameStage::submitScene, stageStart);

            submitSceneHelpers(helperView, ui, helperLayout, colorProgram, colorUniform);
            recordFrameStage(frameTimings, woby::FrameStage::submitHelpers, stageStart);

            try {
                submitSceneScreenshotCapture(
                    sceneScreenshot,
                    files,
                    runtimes,
                    masterVertexPointSize,
                    meshProgram,
                    colorProgram,
                    pointSpriteProgram,
                    colorUniform,
                    pointParamsUniform,
                    ui,
                    helperLayout,
                    sceneBounds,
                    camera,
                    homogeneousDepth);
            } catch (const std::exception& exception) {
                failSceneScreenshotCapture(sceneScreenshot);
                setToastMessage(toast, std::string("Save screenshot failed: ") + exception.what());
            }

            drawToastMessage(toast, width);
            drawHoveredVertexOverlay(hoveredVertex, width, height);
            ImGui::Render();
            woby::imgui_bgfx::render(ImGui::GetDrawData());
            recordFrameStage(frameTimings, woby::FrameStage::imguiRender, stageStart);

            const uint32_t frameNumber = bgfx::frame();
            try {
                const std::optional<std::string> screenshotStatus = completeSceneScreenshotReadback(
                    sceneScreenshot,
                    frameNumber);
                if (screenshotStatus.has_value()) {
                    setToastMessage(toast, screenshotStatus.value());
                }
            } catch (const std::exception& exception) {
                sceneScreenshot.readbackPending = false;
                setToastMessage(toast, std::string("Save screenshot failed: ") + exception.what());
            }
            recordFrameStage(frameTimings, woby::FrameStage::bgfxFrame, stageStart);
            frameTimings.totalMilliseconds = woby::millisecondsBetween(frameStart, woby::PerformanceClock::now());
            copyBgfxStats(frameTimings);
            if (commandLine.logPerformance) {
                woby::accumulateFrameTiming(frameTimingAccumulator, frameTimings);
                if (commandLine.logSlowFrameMilliseconds.has_value()) {
                    woby::logSlowFrame(frameTimings, commandLine.logSlowFrameMilliseconds.value());
                }
                if (frameTimingAccumulator.frameCount >= commandLine.logFrameInterval) {
                    woby::logFrameSummary(frameTimingAccumulator);
                    woby::resetFrameTimingAccumulator(frameTimingAccumulator);
                }
            }
        }

        backgroundLoad.cancelRequested.store(true);
        if (backgroundLoad.worker.joinable()) {
            backgroundLoad.worker.join();
        }
        abortGpuFinalize(gpuFinalize);

        woby::imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        bgfx::destroy(pointParamsUniform);
        bgfx::destroy(colorUniform);
        bgfx::destroy(pointSpriteProgram);
        bgfx::destroy(colorProgram);
        bgfx::destroy(meshProgram);
        destroySceneScreenshotFramebuffer(sceneScreenshot);
        destroyModelRuntimes(runtimes);
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
