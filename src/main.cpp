#include "bgfx_helpers.h"
#include "background_load.h"
#include "camera.h"
#include "scene_viewport.h"
#include "command_line.h"
#include "console.h"
#include "file_discovery.h"
#include "hover_pick.h"
#include "comparison_view.h"
#include "comparison_scene.h"
#include "imgui_bgfx.h"
#include "model_load.h"
#include "native_dialogs.h"
#include "performance_log.h"
#include "scene_file.h"
#include "scene_inspector.h"
#include "scene_scale_overlay.h"
#include "scene_lifecycle.h"
#include "scene_history.h"
#include "scene_history_load.h"
#include "ui_history_controls.h"
#include "scene_renderer.h"
#include "scene_screenshot.h"
#include "ui_operations.h"
#include "ui_state.h"
#include "ui_views.h"
#include "ui_layout.h"
#include "ui_icon_controls.h"
#include "ui_popup_controls.h"
#include "settings_dialog.h"
#include "utf8_path.h"
#include "automation.h"
#include "control_scene.h"
#include "control_importers.h"

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
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using woby::RenderModeState;
using woby::uiSize;
using woby::renderModeButtonSize;
using woby::drawRenderModeIconButton;
using woby::drawTriStateMasterIconButton;
using woby::drawTriStateVisibilityButton;
using woby::drawVisibilityButton;
using woby::drawRemoveButton;

constexpr uint32_t resetFlags = BGFX_RESET_VSYNC | BGFX_RESET_MSAA_X4;
constexpr bgfx::ViewId clearView = 0;
constexpr bgfx::ViewId sceneView = 1;
constexpr bgfx::ViewId helperView = 2;
constexpr bgfx::ViewId imguiView = 255;
constexpr float minSceneViewportWidth = 160.0f;
constexpr float viewerPaneBackgroundRed = 0.20f;
constexpr float viewerPaneBackgroundGreen = 0.21f;
constexpr float viewerPaneBackgroundBlue = 0.22f;
constexpr float viewerPaneBackgroundAlpha = 1.0f;
constexpr float popupBackgroundRed = 0.20f;
constexpr float popupBackgroundGreen = 0.21f;
constexpr float popupBackgroundBlue = 0.22f;
constexpr float popupBackgroundAlpha = 1.0f;
constexpr float appFontSize = 17.0f;
constexpr const char* appFontFilename = "RobotoMonoNerdFont-Regular.ttf";
constexpr ImWchar appFontGlyphRanges[] = {
    0xf013,
    0xf013,
    0xf01e,
    0xf01e,
    0xf05a,
    0xf05a,
    0xf04b,
    0xf04b,
    0xf00d,
    0xf00d,
    0xf030,
    0xf030,
    0xf065,
    0xf065,
    0xf192,
    0xf192,
    0xf068,
    0xf068,
    0xf06e,
    0xf070,
    0xf0b2,
    0xf0b2,
    0xf0e2,
    0xf0e2,
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
    0xea80,
    0xea80,
    0,
};
using woby::solidMeshIcon;
using woby::trianglesIcon;
using woby::verticesIcon;
constexpr const char* frameSceneIcon = "\xef\x81\xa5";
constexpr const char* screenshotIcon = "\xef\x80\xb0";
constexpr const char* originIcon = "\xf3\xb0\xad\x83";
constexpr const char* gridIcon = "\xf3\xb0\x8b\x81";
constexpr const char* viewerPanePinnedIcon = "\xee\xae\xa0";
constexpr const char* viewerPaneUnpinnedIcon = "\xf3\xb0\xa4\xb0";
constexpr float viewerPaneTogglePaneMargin = 6.0f;
constexpr float toastDurationSeconds = 8.0f;
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
    const auto textPath = assets / "fonts" / "Lato-Regular.ttf";
    ImFont* font = io.Fonts->AddFontFromFileTTF(textPath.string().c_str(), appFontSize);
    if (!font) { throw std::runtime_error("Failed to load UI font: " + textPath.string()); }
    ImFontConfig icons;
    icons.MergeMode = true;
    if (!io.Fonts->AddFontFromFileTTF(fontPath.string().c_str(), appFontSize, &icons, appFontGlyphRanges)) {
        throw std::runtime_error("Failed to load icon font: " + fontPath.string());
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
    appendFolderTree,
    openScene,
};

struct AsyncLoadOutcome {
    AsyncLoadKind kind = AsyncLoadKind::appendModel;
    std::filesystem::path folderTreeRoot;
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
    std::filesystem::path folderTreeRoot;
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

struct AutomationAppendRuntime {
    woby::AutomationCommandId id = 0;
    size_t firstFileIndex = 0;
    std::vector<woby::ModelInputOutcome> outcomes;
    bool canceled = false;
};

struct AutomationComparisonRuntime {
    woby::AutomationCommandId id = 0;
    std::string target;
    double tolerance = 0;
    woby::SceneObjectId objectId = woby::invalidSceneObjectId;
    uint64_t signature = 0, sceneGeneration = 0;
    uint32_t stages = 0;
};

struct ResolvedModelInputGroup {
    bool folderTree = false;
    std::filesystem::path root;
    size_t firstPathIndex = 0;
    size_t pathCount = 0;
};

struct ResolvedModelInputs {
    std::vector<std::filesystem::path> paths;
    std::vector<ResolvedModelInputGroup> groups;
};

bool isAppendModelLoadKind(AsyncLoadKind kind)
{
    return kind == AsyncLoadKind::appendModel || kind == AsyncLoadKind::appendFolderTree;
}

const char* backgroundLoadDescription(AsyncLoadKind kind)
{
    switch (kind) {
    case AsyncLoadKind::appendModel:
        return "Loading model files...";
    case AsyncLoadKind::appendFolderTree:
        return "Loading folder tree...";
    case AsyncLoadKind::openScene:
        return "Opening scene...";
    }

    return "Processing files...";
}

const char* backgroundLoadFailurePrefix(AsyncLoadKind kind)
{
    switch (kind) {
    case AsyncLoadKind::appendModel:
        return "Open model files failed: ";
    case AsyncLoadKind::appendFolderTree:
        return "Open folder tree failed: ";
    case AsyncLoadKind::openScene:
        return "Open scene failed: ";
    }

    return "Processing files failed: ";
}

struct CanvasLayout {
    float width = 1.0f;
    float height = 1.0f;
    float leftWidth = 0.0f;
    float maxLeftWidth = 0.0f;
    float rightWidth = 0.0f;
    float maxRightWidth = 0.0f;
    float rightEdge = 1.0f;
    woby::SceneViewport viewport;
};

CanvasLayout canvasLayout(SDL_Window* window, const woby::UiState& state)
{
    int windowWidth = 1;
    int windowHeight = 1;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    CanvasLayout layout;
    layout.width = static_cast<float>(std::max(windowWidth, 1));
    layout.height = static_cast<float>(std::max(windowHeight, 1));
    layout.rightEdge = layout.width;
    const float reservedScene = std::min(minSceneViewportWidth, layout.width * 0.2f);
    const float reservedLeft = state.viewerPaneVisible
        ? std::min(state.viewerPaneWidth, std::max(layout.width - reservedScene - uiSize(300.0f), 0.0f))
        : 0.0f;
    layout.maxRightWidth = std::max(layout.width - reservedLeft - reservedScene, 0.0f);
    layout.rightWidth = std::clamp(state.propertiesPaneWidth, 0.0f, layout.maxRightWidth);
    const float reservedRight = state.propertiesPaneVisible ? layout.width - layout.rightEdge + layout.rightWidth : 0.0f;
    layout.maxLeftWidth = std::max(layout.width - reservedRight
        - std::min(minSceneViewportWidth, layout.width * 0.2f), 0.0f);
    layout.leftWidth = state.viewerPaneVisible
        ? std::clamp(state.viewerPaneWidth, 0.0f, layout.maxLeftWidth) : 0.0f;
    uint32_t pixelWidth = 1;
    uint32_t pixelHeight = 1;
    getDrawableSize(window, pixelWidth, pixelHeight);
    layout.viewport = woby::sceneViewport(pixelWidth, pixelHeight, layout.width, layout.leftWidth, reservedRight);
    return layout;
}

MousePosition mousePositionInPixels(SDL_Window* window, float mouseWindowX, float mouseWindowY)
{
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

MousePosition mousePositionInPixels(SDL_Window* window)
{
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    return mousePositionInPixels(window, x, y);
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
    bool isDirty,
    const std::string& instanceId)
{
    std::string title = "woby " WOBY_VERSION " [" + instanceId + "] - ";
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
    bool isDirty,
    const std::string& instanceId)
{
    SDL_SetWindowTitle(window, appWindowTitle(currentScenePath, isDirty, instanceId).c_str());
}

void setLastItemTooltip(const char* text)
{
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", text);
    }
}

void drawClippedTextItem(const char* id, const char* text, float width, bool selected)
{
    const float itemWidth = std::max(width, 1.0f);
    const float itemHeight = ImGui::GetFrameHeight();
    ImGui::InvisibleButton(id, ImVec2(itemWidth, itemHeight));

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    if (selected || ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(itemMin, itemMax,
            ImGui::GetColorU32(selected ? ImGuiCol_Header : ImGuiCol_HeaderHovered));
    }
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
    auto& style = ImGui::GetStyle();
    style.FrameRounding = 3.0f;
    style.FrameBorderSize = 1.0f;
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.Colors[ImGuiCol_Text] = ImVec4(.94f, .95f, .96f, 1);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(.65f, .67f, .70f, 1);
    style.Colors[ImGuiCol_Button] = ImVec4(.28f, .29f, .31f, 1);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(.36f, .38f, .40f, 1);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(.42f, .44f, .47f, 1);
    style.Colors[ImGuiCol_Header] = ImVec4(.29f, .31f, .33f, 1);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(.36f, .38f, .40f, 1);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(.42f, .44f, .47f, 1);
    style.Colors[ImGuiCol_FrameBg] = ImVec4(.14f, .15f, .17f, 1);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.23f, .25f, .28f, 1);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(.29f, .31f, .34f, 1);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(.40f, .78f, .92f, 1);
    style.Colors[ImGuiCol_SliderGrab] = style.Colors[ImGuiCol_CheckMark];
}

std::filesystem::path uiPreferencePath()
{
    char* directory = SDL_GetPrefPath("woby", "woby");
    if (!directory) { return {}; }
    auto path = woby::pathFromUtf8(directory) / "ui-scale.txt";
    SDL_free(directory);
    return path;
}

void updateUiScale(SDL_Window* window, woby::UiState& state, const ImGuiStyle& baseStyle)
{
    const float scale = woby::logicalUiScale(state.uiScale,
        SDL_GetWindowDisplayScale(window), SDL_GetWindowPixelDensity(window));
    auto& style = ImGui::GetStyle();
    if (style.FontScaleMain == scale) { return; }
    const float ratio = scale / style.FontScaleMain;
    woby::setViewerPaneWidth(state, state.viewerPaneWidth * ratio, 380.0f * scale, 10000.0f);
    woby::setPropertiesPaneWidth(state, state.propertiesPaneWidth * ratio, 300.0f * scale, 10000.0f);
    style = woby::scaledUiStyle(baseStyle, scale);
}

float renderModeButtonRowWidth()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    return renderModeButtonSize() * 3.0f + style.ItemSpacing.x * 2.0f;
}

float minimumViewerPaneWidth()
{
    return uiSize(380.0f);
}

void drawViewerPaneToggleButton(woby::UiState& state)
{
    if (drawRenderModeIconButton(
            "toggle_viewer_pane",
            state.viewerPaneVisible ? viewerPanePinnedIcon : viewerPaneUnpinnedIcon,
            state.viewerPaneVisible ? "Hide left pane (Ctrl+B)" : "Show left pane (Ctrl+B)",
            state.viewerPaneVisible ? RenderModeState::on : RenderModeState::off,
            false)) {
        woby::toggleViewerPaneVisible(state);
    }
}

void drawPropertiesPaneToggleButton(woby::UiState& state)
{
    if (drawRenderModeIconButton(
            "toggle_properties_pane",
            state.propertiesPaneVisible ? viewerPanePinnedIcon : viewerPaneUnpinnedIcon,
            state.propertiesPaneVisible ? "Hide properties pane" : "Show properties pane",
            state.propertiesPaneVisible ? RenderModeState::on : RenderModeState::off,
            false)) {
        woby::setPropertiesPaneVisible(state, !state.propertiesPaneVisible);
    }
}

void drawCameraToolbar(woby::UiState& state)
{
    const std::array<std::pair<const char*, woby::CameraView>, 6> views = {{
        {"Top", woby::CameraView::top}, {"Bottom", woby::CameraView::bottom},
        {"Front", woby::CameraView::front}, {"Back", woby::CameraView::back},
        {"Left", woby::CameraView::left}, {"Right", woby::CameraView::right},
    }};
    for (const auto& [label, view] : views) {
        if (woby::drawCameraViewButton(label, view, label)) { woby::setCameraView(state, view); }
        ImGui::SameLine();
    }
    if (drawRenderModeIconButton("fit_all", frameSceneIcon,
            "Fit all - frame the scene (R)", RenderModeState::off,
            state.files.empty() && state.comparisons.empty())) {
        woby::fitCameraToScene(state);
    }
    ImGui::SameLine();
    const bool selectionAvailable = woby::selectedSceneBounds(state).has_value();
    if (drawRenderModeIconButton("fit_selection", frameSceneIcon,
            "Fit selection", RenderModeState::off, !selectionAvailable)) {
        woby::fitCameraToSelection(state);
    }
    const auto low = ImGui::GetItemRectMin();
    const float centerX = low.x + renderModeButtonSize() * 0.5f;
    const float centerY = low.y + renderModeButtonSize() * 0.5f;
    const float radius = uiSize(3.0f);
    ImGui::GetWindowDrawList()->AddRect(ImVec2(centerX - radius, centerY - radius),
        ImVec2(centerX + radius, centerY + radius),
        ImGui::GetColorU32(ImGuiCol_TextDisabled, selectionAvailable ? 1.0f : ImGui::GetStyle().DisabledAlpha));
}

void drawPropertiesPane(woby::UiState& state, woby::ComparisonRuntimes& runtimes,
    const CanvasLayout& layout, woby::SceneDimensionsCache& dimensionsCache)
{
    if (!state.propertiesPaneVisible) { return; }
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::SetNextWindowPos(ImVec2(layout.rightEdge, 0.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(layout.rightWidth, layout.height), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(std::min(uiSize(300.0f), layout.rightWidth), layout.height),
        ImVec2(layout.maxRightWidth, layout.height));
    if (ImGui::Begin("##PropertiesPane", nullptr, ImGuiWindowFlags_NoTitleBar
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
        woby::setPropertiesPaneWidth(state, ImGui::GetWindowSize().x, uiSize(300.0f), layout.maxRightWidth);
        drawPropertiesPaneToggleButton(state);
        ImGui::SameLine();
        ImGui::TextUnformatted("Properties");
        ImGui::Separator();
        if (woby::selectedComparison(state)) {
            woby::drawComparisonPanelContents(state, runtimes);
        } else {
            woby::drawSceneInspector(state, dimensionsCache);
        }
    }
    ImGui::End();
}

void drawPaneToggles(woby::UiState& state, float windowWidth, bool settingsDisabled, bool& requestSettings)
{
    const auto flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_AlwaysAutoResize
        | ImGuiWindowFlags_NoMove
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    if (!state.viewerPaneVisible) {
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::SetNextWindowPos(ImVec2(viewerPaneTogglePaneMargin, viewerPaneTogglePaneMargin), ImGuiCond_Always);
        if (ImGui::Begin("##ShowViewerPane", nullptr, flags)) {
            drawViewerPaneToggleButton(state);
            ImGui::SameLine();
            ImGui::TextUnformatted("Scene controls");
            ImGui::SameLine();
            if (woby::drawSettingsButton(settingsDisabled)) { requestSettings = true; }
        }
        ImGui::End();
    }
    if (!state.propertiesPaneVisible) {
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::SetNextWindowPos(
            ImVec2(windowWidth - viewerPaneTogglePaneMargin, viewerPaneTogglePaneMargin),
            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        if (ImGui::Begin("##ShowPropertiesPane", nullptr, flags)) {
            drawPropertiesPaneToggleButton(state);
            ImGui::SameLine();
            ImGui::TextUnformatted("Properties");
        }
        ImGui::End();
    }
    ImGui::PopStyleVar();
}

void pushRenderModeControlHeight()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float paddingY = std::max(
        (renderModeButtonSize() - ImGui::GetFontSize()) * 0.5f,
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

void drawSceneItemInteraction(woby::UiState& state, woby::SceneObjectId id,
    bool treeNode)
{
    if (woby::sceneObjectSelected(state, id)) {
        woby::drawSceneItemOutline();
    }
    // Ordinary clicks select on release so source drags keep the comparison inspector visible.
    // TreeNodeEx does not activate labels with Ctrl held, so Ctrl-clicks must be
    // handled on mouse-down instead of waiting for IsItemDeactivated().
    const auto selectionKey = ImGui::GetID(("source_click_" + std::to_string(id)).c_str());
    auto* storage = ImGui::GetStateStorage();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        const bool select = !treeNode || !ImGui::IsItemToggledOpen();
        const bool toggle = ImGui::GetIO().KeyCtrl;
        storage->SetBool(selectionKey, select && !toggle);
        if (select && toggle) {
            woby::selectSceneObject(state, id, true);
        }
    }
    const auto dragDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Left)
        && storage->GetBool(selectionKey) && dragDelta.x == 0.0f && dragDelta.y == 0.0f) {
        woby::selectSceneObject(state, id);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        woby::selectSceneObject(state, id, false, true);
    }
    if (ImGui::BeginDragDropSource()) {
        const auto sources = woby::sceneObjectSelected(state, id)
            ? state.selectedSceneObjects : std::vector<woby::SceneObjectId>{id};
        const auto parts = woby::comparisonObjectParts(state, sources);
        if (!parts.empty()) {
            ImGui::SetDragDropPayload(woby::comparisonSourcePayload, parts.data(),
                parts.size() * sizeof(woby::SceneObjectId));
            ImGui::Text("Add %zu parts to analysis input A or B", parts.size());
        } else {
            ImGui::TextUnformatted("This source has no triangular mesh parts.");
        }
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("scene_item_context")) {
        if (ImGui::MenuItem("Create analysis", nullptr, false, woby::canCompareSceneSelection(state))) {
            woby::compareSceneSelection(state);
        }
        if (ImGui::BeginMenu("Analysis membership", !state.comparisons.empty())) {
            for (const auto& comparisonObject : state.comparisons) {
                ImGui::PushID(std::to_string(comparisonObject.objectId).c_str());
                if (ImGui::BeginMenu(comparisonObject.name.c_str())) {
                    for (const auto side : {woby::ComparisonSide::a, woby::ComparisonSide::b}) {
                        const auto action = woby::comparisonMembershipAction(state, state.selectedSceneObjects, side, comparisonObject.objectId);
                        const bool remove = action == woby::ComparisonMembershipAction::remove;
                        const char* label = side == woby::ComparisonSide::a
                            ? (remove ? "Remove from A" : "Add to A") : (remove ? "Remove from B" : "Add to B");
                        if (ImGui::MenuItem(label, nullptr, false, action != woby::ComparisonMembershipAction::unavailable)) {
                            woby::setComparisonObjects(state, state.selectedSceneObjects, side, !remove, comparisonObject.objectId);
                            woby::selectSceneObject(state, comparisonObject.objectId);
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::PopID();
            }
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
}

void drawGroupControls(
    woby::UiState& state,
    const woby::MeshNode& node,
    const GpuNodeRange& range,
    LoadedModelFile& file,
    size_t nodeIndex)
{
    ImGui::PushID(static_cast<int>(nodeIndex));
    auto& settings = file.groupSettings[nodeIndex];
    if (drawVisibilityButton("visible", settings.visible, "part")) {
        woby::toggleGroupVisible(state, file, settings);
    }
    ImGui::SameLine();
    const bool memberA = woby::comparisonContains(state, settings.objectId, woby::ComparisonSide::a);
    const bool memberB = woby::comparisonContains(state, settings.objectId, woby::ComparisonSide::b);
    const std::string badge = memberA ? (memberB ? "[A B] " : "[A] ") : (memberB ? "[B] " : "");
    const std::string displayName = badge + node.name;
    drawClippedTextItem("##name", displayName.c_str(), ImGui::GetContentRegionAvail().x,
        woby::sceneObjectSelected(state, settings.objectId));
    const std::string tooltip = node.name + "\n" + meshCountLine(range.pointIndexCount, node.indexCount / 3u);
    setLastItemTooltip(tooltip.c_str());
    drawSceneItemInteraction(state, settings.objectId, false);
    ImGui::PopID();
}

void drawSceneTreeNode(
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    woby::UiSceneNode& node,
    std::optional<size_t>& removeFileIndex,
    std::span<const woby::SceneObjectId> revealPath)
{
    if (node.kind == woby::UiSceneNodeKind::folder) {
        const size_t groupCount = woby::countSceneNodeGroups(state, node);
        const size_t visibleCount = woby::countVisibleSceneNodeGroups(state, node);
        if (drawTriStateVisibilityButton(
                "visible",
                "Folder",
                visibleCount,
                groupCount)) {
            woby::setSceneNodeSubtreeVisible(state, node, visibleCount != groupCount);
        }
        ImGui::SameLine();
        const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
            | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
            | (woby::sceneObjectSelected(state, node.objectId) ? ImGuiTreeNodeFlags_Selected : 0);
        if (std::find(revealPath.begin(), revealPath.end(), node.objectId) != revealPath.end()) { ImGui::SetNextItemOpen(true); }
        const bool folderOpen = ImGui::TreeNodeEx(node.name.c_str(), flags);
        drawSceneItemInteraction(state, node.objectId, true);
        if (folderOpen) {
            for (size_t childIndex = 0; childIndex < node.children.size(); ++childIndex) {
                ImGui::PushID(static_cast<int>(childIndex));
                drawSceneTreeNode(state, runtimes, node.children[childIndex], removeFileIndex, revealPath);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        return;
    }

    if (node.kind == woby::UiSceneNodeKind::file) {
        if (node.fileIndex >= state.files.size()) {
            return;
        }

        auto& file = state.files[node.fileIndex];
        const ImGuiStyle& style = ImGui::GetStyle();
        const float rowStartX = ImGui::GetCursorPosX();
        const float removeControlStartX = rowStartX + ImGui::GetContentRegionAvail().x - renderModeButtonSize();
        const std::string label = node.name + "##file_" + std::to_string(node.fileIndex);
        const size_t fileGroupCount = woby::countSceneNodeGroups(state, node);
        const size_t fileVisibleCount = woby::countVisibleSceneNodeGroups(state, node);
        if (drawTriStateVisibilityButton(
                "visible",
                "File",
                fileVisibleCount,
                fileGroupCount)) {
            woby::setSceneNodeSubtreeVisible(state, node, fileVisibleCount != fileGroupCount);
        }
        ImGui::SameLine();
        const std::string tooltipText = file.path.string()
            + "\n"
            + meshCountLine(
                file.mesh.vertices.size(),
                file.mesh.indices.size() / 3u);
        // Reserve the remove button's column for both drawing and hit testing.
        const ImVec2 labelClipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
        ImVec2 labelClipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
        labelClipMax.x = ImGui::GetCursorScreenPos().x + removeControlStartX
            - ImGui::GetCursorPosX() - style.ItemSpacing.x;
        ImGui::PushClipRect(labelClipMin, labelClipMax, true);
        const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
            | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
            | (woby::sceneObjectSelected(state, node.objectId) ? ImGuiTreeNodeFlags_Selected : 0);
        if (std::find(revealPath.begin(), revealPath.end(), node.objectId) != revealPath.end()) { ImGui::SetNextItemOpen(true); }
        const bool fileTreeOpen = ImGui::TreeNodeEx(label.c_str(), flags);
        setLastItemTooltip(tooltipText.c_str());
        drawSceneItemInteraction(state, node.objectId, true);
        ImGui::PopClipRect();
        ImGui::SameLine(removeControlStartX, 0.0f);
        if (drawRemoveButton("remove", "Remove file from scene")) {
            removeFileIndex = node.fileIndex;
        }
        if (fileTreeOpen) {
            for (size_t childIndex = 0; childIndex < node.children.size(); ++childIndex) {
                ImGui::PushID(static_cast<int>(childIndex));
                drawSceneTreeNode(state, runtimes, node.children[childIndex], removeFileIndex, revealPath);
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        return;
    }

    if (node.fileIndex >= state.files.size() || node.fileIndex >= runtimes.size()) {
        return;
    }

    auto& file = state.files[node.fileIndex];
    const auto& gpuMesh = runtimes[node.fileIndex].gpuMesh;
    if (node.groupIndex >= file.mesh.nodes.size() || node.groupIndex >= gpuMesh.nodeRanges.size()) {
        return;
    }

    drawGroupControls(
        state,
        file.mesh.nodes[node.groupIndex],
        gpuMesh.nodeRanges[node.groupIndex],
        file,
        node.groupIndex);
    if (!revealPath.empty() && revealPath.back() == node.objectId) { ImGui::SetScrollHereY(.5f); }
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
        throw std::runtime_error("Folder did not contain model files recursively: " + folder.string());
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

ResolvedModelInputs resolveModelInputs(const woby::AppArguments& options)
{
    ResolvedModelInputs inputs;

    for (const auto& inputPath : options.inputPaths) {
        ResolvedModelInputGroup group;
        group.folderTree = inputPath.folderTree;
        group.root = inputPath.path;
        group.firstPathIndex = inputs.paths.size();

        if (inputPath.folder) {
            appendFolderModelPaths(inputPath.path, inputs.paths);
            group.pathCount = inputs.paths.size() - group.firstPathIndex;
            inputs.groups.push_back(std::move(group));
            continue;
        }

        inputs.paths.push_back(inputPath.path);
        group.pathCount = 1u;
        inputs.groups.push_back(std::move(group));
    }

    return inputs;
}

void appendSceneNodesForResolvedInputs(
    woby::UiState& state,
    const ResolvedModelInputs& inputs,
    size_t firstFileIndex)
{
    for (const auto& group : inputs.groups) {
        if (group.pathCount == 0u) {
            continue;
        }

        if (group.folderTree) {
            woby::appendFolderTreeSceneNode(
                state,
                group.root,
                firstFileIndex + group.firstPathIndex,
                group.pathCount);
            continue;
        }

        for (size_t offset = 0; offset < group.pathCount; ++offset) {
            const size_t fileIndex = firstFileIndex + group.firstPathIndex + offset;
            if (fileIndex < state.files.size()) {
                state.sceneNodes.push_back(woby::createFileSceneNode(state.files[fileIndex], fileIndex));
            }
        }
    }

    woby::assignSceneObjectIds(state);
    woby::refreshSceneTreeFolderCenters(state);
}

LoadedModelFileWithRuntime loadModelFile(
    const std::filesystem::path& modelPath,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    size_t firstColorIndex,
    const woby::SceneFileRecord* sceneRecord = nullptr)
{
    const auto totalStart = woby::PerformanceClock::now();
    LoadedModelFileWithRuntime loaded;
    try {
        const auto parseStart = woby::PerformanceClock::now();
        auto imported = woby::loadModel(modelPath, sceneRecord ? sceneRecord->importerId : std::string{});
        const double parseMilliseconds = elapsedMilliseconds(parseStart);

        loaded.file = woby::createUiFileState(modelPath, std::move(imported.mesh), firstColorIndex, std::move(imported.importerId));
        if (sceneRecord != nullptr) {
            woby::applySceneFileRecord(loaded.file, *sceneRecord);
        }

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
    const ResolvedModelInputs& modelInputs,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout,
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes)
{
    if (modelInputs.paths.empty()) {
        return;
    }

    const auto start = woby::PerformanceClock::now();
    std::vector<LoadedModelRuntime> loadedRuntimes;
    std::vector<LoadedModelFile> loadedFiles = loadModelFiles(
        modelInputs.paths,
        meshLayout,
        pointSpriteLayout,
        loadedRuntimes,
        woby::totalGroupCount(state));

    const size_t firstFileIndex = state.files.size();
    state.files.insert(
        state.files.end(),
        std::make_move_iterator(loadedFiles.begin()),
        std::make_move_iterator(loadedFiles.end()));
    runtimes.insert(
        runtimes.end(),
        std::make_move_iterator(loadedRuntimes.begin()),
        std::make_move_iterator(loadedRuntimes.end()));
    appendSceneNodesForResolvedInputs(state, modelInputs, firstFileIndex);
    woby::recalculateSceneBounds(state);
    woby::frameCameraToScene(state);
    woby::markSceneDirty(state);
    spdlog::info(
        "perf append_initial_model_files requested_count={} duration_ms={}",
        modelInputs.paths.size(),
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

bool applySceneHistory(woby::SceneHistory& history, woby::UiState& state,
    const woby::SceneDocument& cleanDocument, std::vector<LoadedModelRuntime>& runtimes,
    const bgfx::VertexLayout& layout, const bgfx::VertexLayout& pointLayout, bool redo)
{
    auto prepared = woby::loadSceneHistoryStep(history, state, cleanDocument, redo);
    if (!prepared) { return false; }
    std::vector<LoadedModelRuntime> staged(prepared->files.size());
    std::vector<size_t> reuse(prepared->files.size(), woby::invalidSceneNodeIndex);
    try {
        for (size_t index = 0; index < prepared->files.size(); ++index) {
            const auto& file = prepared->files[index];
            for (size_t old = 0; old < state.files.size(); ++old) {
                if (state.files[old].objectId == file.objectId) { reuse[index] = old; break; }
            }
            if (reuse[index] == woby::invalidSceneNodeIndex) {
                staged[index].gpuMesh = createGpuMesh(file.mesh, layout, pointLayout);
            }
        }
    } catch (...) {
        destroyModelRuntimes(staged);
        throw;
    }
    for (size_t index = 0; index < staged.size(); ++index) {
        if (reuse[index] != woby::invalidSceneNodeIndex) {
            staged[index] = std::exchange(runtimes[reuse[index]], LoadedModelRuntime{});
        }
    }
    destroyModelRuntimes(runtimes);
    runtimes = std::move(staged);
    woby::commitSceneHistoryStep(history, state, std::move(*prepared), redo);
    return true;
}

void pushDroppedPath(DragDropState& state, const char* data)
{
    if (data == nullptr) {
        return;
    }

    if (state.active) {
        state.batchPaths.push_back(woby::pathFromUtf8(data));
    } else {
        state.pendingPaths.push_back(woby::pathFromUtf8(data));
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
            LoadedModelFileWithRuntime loaded = loadModelFile(modelPath, meshLayout, pointSpriteLayout, colorIndex, &record);
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
    try {
        auto prepared = woby::prepareSceneReplacement(state, std::move(loadedFiles), document);
        destroyModelRuntimes(runtimes);
        runtimes = std::move(loadedRuntimes);
        state = std::move(prepared);
    } catch (...) {
        destroyModelRuntimes(loadedRuntimes);
        throw;
    }
    spdlog::info(
        "perf scene_load path=\"{}\" files={} read_ms={} files_ms={} apply_ms={} total_ms={}",
        scenePath.string(),
        state.files.size(),
        readMilliseconds,
        filesMilliseconds,
        elapsedMilliseconds(applyStart),
        elapsedMilliseconds(totalStart));
}

void resetSceneToUntitled(
    woby::UiState& state,
    std::vector<LoadedModelRuntime>& runtimes,
    std::optional<std::filesystem::path>& currentScenePath,
    woby::SceneDocument& cleanSceneDocument)
{
    auto document = woby::createSceneDocument(woby::UiState{});
    document.camera.reset(); // New scenes frame the default display bounds.
    auto prepared = woby::prepareSceneReplacement(state, {}, document);
    auto clean = woby::createSceneDocument(prepared);
    destroyModelRuntimes(runtimes);
    state = std::move(prepared);
    cleanSceneDocument = std::move(clean);
    currentScenePath.reset();
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
    if (const auto error = woby::saveSceneState(state, currentScenePath, cleanSceneDocument, requestedScenePath, true)) {
        throw std::runtime_error(error->message);
    }
    return *currentScenePath;
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
    size_t firstColorIndex,
    AsyncLoadKind kind = AsyncLoadKind::appendModel,
    std::filesystem::path folderTreeRoot = {})
{
    if (load.active) {
        return false;
    }

    if (load.worker.joinable()) {
        load.worker.join();
    }

    resetBackgroundLoadProgress(load, kind, modelPaths.size());
    load.cancelRequested.store(false);
    load.active = true;
    try {
        load.worker = std::thread([
            &load,
            modelPaths = std::move(modelPaths),
            firstColorIndex,
            kind,
            folderTreeRoot = std::move(folderTreeRoot)
        ]() mutable {
            AsyncLoadOutcome outcome;
            outcome.kind = kind;
            outcome.folderTreeRoot = std::move(folderTreeRoot);
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
    } catch (...) {
        load.active = false;
        throw;
    }
    return true;
}

bool startAppendFolderTreeBackgroundLoad(
    BackgroundLoadRuntime& load,
    const std::filesystem::path& folder,
    size_t firstColorIndex,
    std::string& status)
{
    if (load.active) {
        return false;
    }

    std::vector<std::filesystem::path> modelPaths;
    try {
        appendFolderModelPaths(folder, modelPaths);
    } catch (const std::exception& exception) {
        status = std::string("Open folder tree failed: ") + exception.what();
        return true;
    }

    return startAppendModelBackgroundLoad(
        load,
        std::move(modelPaths),
        firstColorIndex,
        AsyncLoadKind::appendFolderTree,
        normalizedPath(folder));
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
    try {
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
    } catch (...) {
        load.active = false;
        throw;
    }
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
    if (load.cancelRequested.exchange(false) && outcome->kind == AsyncLoadKind::openScene) {
        outcome->scene.canceled = true;
    }
    return outcome;
}

void startGpuFinalize(GpuFinalizeRuntime& finalize, AsyncLoadOutcome outcome)
{
    finalize = GpuFinalizeRuntime{};
    finalize.kind = outcome.kind;
    finalize.folderTreeRoot = std::move(outcome.folderTreeRoot);
    if (isAppendModelLoadKind(outcome.kind)) {
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
    if (isAppendModelLoadKind(finalize.kind)) {
        const bool addedAnyFiles = !finalize.finalizedFiles.empty();
        const size_t firstFileIndex = state.files.size();
        state.files.insert(
            state.files.end(),
            std::make_move_iterator(finalize.finalizedFiles.begin()),
            std::make_move_iterator(finalize.finalizedFiles.end()));
        runtimes.insert(
            runtimes.end(),
            std::make_move_iterator(finalize.finalizedRuntimes.begin()),
            std::make_move_iterator(finalize.finalizedRuntimes.end()));
        if (addedAnyFiles) {
            if (finalize.kind == AsyncLoadKind::appendFolderTree) {
                woby::appendFolderTreeSceneNode(
                    state,
                    finalize.folderTreeRoot,
                    firstFileIndex,
                    finalize.finalizedFiles.size());
            } else {
                woby::appendDefaultSceneNodesForFiles(state, firstFileIndex);
            }
            woby::recalculateSceneBounds(state);
            woby::frameCameraToScene(state);
            woby::markSceneDirty(state);
        }
        setToastMessage(toast, appendFinalizeStatus(finalize));
    } else {
        auto prepared = woby::prepareSceneReplacement(state, std::move(finalize.finalizedFiles), finalize.sceneDocument);
        auto clean = woby::createSceneDocument(prepared);
        auto path = finalize.scenePath;
        auto message = "Opened scene " + fileDisplayName(finalize.scenePath);
        destroyModelRuntimes(runtimes);
        runtimes = std::move(finalize.finalizedRuntimes);
        state = std::move(prepared);
        currentScenePath = std::move(path);
        cleanSceneDocument = std::move(clean);
        setToastMessage(toast, std::move(message));
    }

    finalize = GpuFinalizeRuntime{};
}

std::optional<std::string> processGpuFinalizeStep(
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
        return {};
    }

    if (finalize.nextFileIndex >= finalize.files.size()) {
        try {
            commitGpuFinalize(finalize, state, runtimes, currentScenePath, cleanSceneDocument, toast);
            return std::string{};
        } catch (const std::exception& exception) {
            const std::string error = exception.what();
            setToastMessage(toast, "Open scene failed: " + error);
            abortGpuFinalize(finalize);
            return error;
        }
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
            return std::string(exception.what());
        }
    }
    return {};
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

void drawToastMessage(const ToastMessage& toast, const woby::SceneViewport& viewport, uint32_t drawableWidth)
{
    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - toast.startedAt).count();
    if (toast.text.empty() || elapsed >= toastDurationSeconds) {
        return;
    }
    const float alpha = std::clamp((toastDurationSeconds - elapsed) / 0.35f, 0.0f, 1.0f);
    // Rendering uses drawable pixels; ImGui positions use logical window coordinates.
    const float windowScale = ImGui::GetIO().DisplaySize.x / static_cast<float>(std::max(drawableWidth, 1u));
    const float centerX = (static_cast<float>(viewport.x) + static_cast<float>(viewport.width) * 0.5f) * windowScale;
    const float textWidth = std::max(1.0f, static_cast<float>(viewport.width) * windowScale
        - uiSize(toastMargin * 2.0f + 24.0f));
    ImGui::SetNextWindowBgAlpha(0.86f * alpha);
    ImGui::SetNextWindowPos(ImVec2(centerX, uiSize(toastMargin)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(uiSize(12.0f), uiSize(8.0f)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, alpha));
    if (ImGui::Begin("##ToastMessage", nullptr, ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs)) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
        ImGui::TextUnformatted(toast.text.c_str());
        ImGui::PopTextWrapPos();
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
            ImGui::TextUnformatted(backgroundLoadDescription(kind));
            if (!progress.currentPath.empty()) {
                ImGui::Text(
                    "%s",
                    fileDisplayName(progress.currentPath).c_str());
            }
            if (progress.totalCount > 0u) {
                const float fraction = (static_cast<float>(std::min(progress.completedCount, progress.totalCount)) + progress.currentFileFraction)
                    / static_cast<float>(progress.totalCount);
                ImGui::ProgressBar(
                    fraction,
                    ImVec2(260.0f, 0.0f),
                    (std::to_string(progress.completedCount) + " / " + std::to_string(progress.totalCount)).c_str());
            } else {
                ImGui::TextUnformatted("Preparing...");
            }
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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

void drawHoveredVertexOverlay(const std::optional<HoveredVertex>& hoveredVertex,
    const woby::SceneViewport& viewport, uint32_t drawableWidth)
{
    if (!hoveredVertex.has_value()) {
        return;
    }

    const float windowScale = ImGui::GetIO().DisplaySize.x / static_cast<float>(std::max(drawableWidth, 1u));
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::SetNextWindowPos(
        ImVec2(
            static_cast<float>(viewport.x + viewport.width) * windowScale - uiSize(toastMargin),
            static_cast<float>(viewport.height) * windowScale - uiSize(toastMargin)),
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
    woby::initializeConsole();
    bool sdlInitialized = false;
    bool bgfxInitialized = false;
    woby::AutomationOwner automation(nullptr, &woby::stopAutomation);

    try {
        const auto commandLine = woby::parseCommandLine(argc, argv);
        if (commandLine.showHelp) {
            woby::printCommandLineHelp();
            return 0;
        }
        if (commandLine.update.command != woby::UpdateCommand::none) {
            return woby::runUpdateCommand(commandLine.update, WOBY_VERSION);
        }
        if (commandLine.control.command != woby::ControlCommand::none) {
            return woby::runAutomationCommand(commandLine.control);
        }
        initializeLogging(commandLine);
        const auto startupStart = woby::PerformanceClock::now();
        if (commandLine.showVersion) {
            std::printf("%s\n", WOBY_VERSION);
            spdlog::shutdown();
            return 0;
        }

        auto deploymentGuard = woby::guardViewerDeployment();
        woby::UpdateUiRuntime updateRuntime;
        updateRuntime.state.currentVersion = WOBY_VERSION;
        updateRuntime.state.managedDeployment = std::filesystem::is_regular_file(
            woby::updateExecutablePath().parent_path() / woby::packageManifestName);
        automation = woby::startAutomation(commandLine.instanceId);
        const std::string instanceId = woby::automationInstanceId(*automation);

        const auto sdlStart = woby::PerformanceClock::now();
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
        sdlInitialized = true;

        std::filesystem::path importerSettingsPath;
        std::vector<std::filesystem::path> rememberedImporters;
        bool importerLoadFailed = false;
        const auto reportImporterError = [&](const std::string& message) {
            importerLoadFailed = true;
            std::fprintf(stderr, "%s\n", message.c_str());
            spdlog::warn("{}", message);
        };
        // CLI entries take precedence over saved registrations; each folder is sorted.
        const auto tryLoadImporter = [&](const std::filesystem::path& path) {
            try { woby::loadImporter(path); }
            catch (const std::exception& error) { reportImporterError(path.string() + ": " + error.what()); }
        };
        for (const auto& source : commandLine.pluginPaths) {
            if (source.folder) {
                try {
                    for (const auto& path : woby::discoverImporterFiles(source.path)) { tryLoadImporter(path); }
                } catch (const std::exception& error) { reportImporterError(error.what()); }
            } else {
                tryLoadImporter(source.path);
            }
        }
        if (char* preferencePath = SDL_GetPrefPath("woby", "woby")) {
            importerSettingsPath = woby::pathFromUtf8(preferencePath) / "importers.txt";
            SDL_free(preferencePath);
            try {
                rememberedImporters = woby::readImporterSettings(importerSettingsPath);
                for (const auto& path : rememberedImporters) { tryLoadImporter(path); }
            } catch (const std::exception& error) { reportImporterError(error.what()); }
        } else {
            reportImporterError("Importer settings directory is unavailable; registrations are session-only.");
        }

        SDL_Window* rawWindow = SDL_CreateWindow(("woby " WOBY_VERSION " [" + instanceId + "]").c_str(), 1280, 720, SDL_WINDOW_RESIZABLE);
        if (rawWindow == nullptr) {
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
        }

        std::unique_ptr<SDL_Window, SdlDeleter> window(rawWindow);

        const auto assets = assetRoot();
        const auto iconPath = woby::pathToUtf8(assets / "icons" / "woby.bmp");
        SDL_Surface* icon = SDL_LoadBMP(iconPath.c_str());
        if (icon != nullptr) {
            if (!SDL_SetWindowIcon(window.get(), icon)) {
                spdlog::warn("Could not set window icon: {}", SDL_GetError());
            }
            SDL_DestroySurface(icon);
        } else {
            spdlog::warn("Could not load window icon: {}", SDL_GetError());
        }

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

        bgfx::setViewClear(clearView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20242aff, 1.0f, 0);
        bgfx::setViewClear(sceneView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
        bgfx::setViewClear(helperView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
        bgfx::setDebug(BGFX_DEBUG_TEXT);
        woby::logDuration("startup_bgfx", elapsedMilliseconds(bgfxStart));

        const auto modelPathsStart = woby::PerformanceClock::now();
        const auto modelInputs = resolveModelInputs(commandLine);
        woby::logDuration("startup_resolve_model_paths", elapsedMilliseconds(modelPathsStart));
        const auto layout = meshVertexLayout();
        const auto pointLayout = pointSpriteVertexLayout();
        const auto helperLayout = helperLineVertexLayout();
        woby::UiState ui;
        woby::ComparisonRuntimes comparison;
        woby::ComparisonNameEdit comparisonNameEdit;
        woby::ViewNameEdit viewNameEdit;
        std::vector<LoadedModelRuntime> runtimes;
        std::optional<std::filesystem::path> currentScenePath;
        woby::SceneDocument cleanSceneDocument = woby::createSceneDocument(ui);

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
            appendInitialModelFiles(modelInputs, layout, pointLayout, ui, runtimes);
        } else {
            ui.files = loadModelFiles(modelInputs.paths, layout, pointLayout, runtimes);
            appendSceneNodesForResolvedInputs(ui, modelInputs, 0u);
            woby::recalculateSceneBounds(ui);
            woby::frameCameraToScene(ui);
            woby::updateSceneDirty(ui, cleanSceneDocument);
        }
        woby::logDuration("startup_initial_scene", elapsedMilliseconds(initialLoadStart));
        woby::SceneHistory sceneHistory;
        woby::resetSceneHistory(sceneHistory, ui);

        const auto shaderStart = woby::PerformanceClock::now();
        bgfx::ProgramHandle meshProgram = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
        bgfx::ProgramHandle colorProgram = woby::loadProgram(assets, "vs_color.bin", "fs_color.bin");
        bgfx::ProgramHandle pointSpriteProgram = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_point_sprite.bin");
        bgfx::UniformHandle colorUniform = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
        bgfx::UniformHandle pointParamsUniform = bgfx::createUniform("u_pointParams", bgfx::UniformType::Vec4);
        comparison.program = woby::loadProgram(assets, "vs_comparison.bin", "fs_comparison.bin");
        comparison.parameters = bgfx::createUniform("u_comparison", bgfx::UniformType::Vec4);
        woby::logDuration("startup_shaders", elapsedMilliseconds(shaderStart));

        const auto imguiStart = woby::PerformanceClock::now();
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        loadAppFont(assets);
        configureAppStyle();
        const ImGuiStyle baseStyle = ImGui::GetStyle();
        const auto preferencePath = uiPreferencePath();
        float savedUiScale = 1.0f;
        if (std::ifstream preference{preferencePath}; preference >> savedUiScale) { woby::setUiScale(ui, savedUiScale); }
        updateUiScale(window.get(), ui, baseStyle);

        if (!ImGui_ImplSDL3_InitForOther(window.get())) {
            throw std::runtime_error("ImGui_ImplSDL3_InitForOther failed.");
        }
        woby::imgui_bgfx::init(assets, imguiView);
        woby::logDuration("startup_imgui", elapsedMilliseconds(imguiStart));

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
        std::optional<woby::AutomationCommandId> automationScreenshotCommandId;
        std::optional<woby::AutomationCommandId> automationOpenCommandId;
        std::optional<AutomationAppendRuntime> automationAppend;
        std::optional<AutomationComparisonRuntime> automationComparison;
        const woby::ObjectIdFormatter formatObjectId = [&](woby::SceneObjectId id) { return woby::automationObjectId(*automation, id); };
        auto completeAppend = [&]() {
            if (!automationAppend) { return; }
            nlohmann::json outcomes = nlohmann::json::array(), addedIds = nlohmann::json::array();
            size_t failed = 0, skipped = 0;
            for (const auto& input : automationAppend->outcomes) {
                nlohmann::json result = {{"path", woby::pathToUtf8(input.path)}, {"state", input.state}};
                if (!input.error.empty()) { result["error"] = input.error; }
                if (input.state == "failed") { ++failed; }
                if (input.state == "skipped") { ++skipped; }
                if (automationAppend->canceled && (input.state == "loaded" || input.state == "not-started")) { result["state"] = "canceled"; }
                else if (input.state == "loaded") {
                    for (size_t index = automationAppend->firstFileIndex; index < ui.files.size(); ++index) {
                        if (ui.files[index].path == input.path) {
                            result["state"] = "added";
                            result["id"] = formatObjectId(ui.files[index].objectId);
                            addedIds.push_back(result["id"]);
                            break;
                        }
                    }
                }
                outcomes.push_back(std::move(result));
            }
            woby::updateSceneDirty(ui, cleanSceneDocument);
            woby::completeAutomationCommand(*automation, automationAppend->id, woby::AutomationControlResult{
                {{"outcomes", outcomes}, {"addedIds", addedIds}, {"requestedCount", outcomes.size()},
                    {"addedCount", addedIds.size()}, {"failedCount", failed}, {"skippedCount", skipped},
                    {"canceled", automationAppend->canceled}, {"dirty", ui.isDirty}}});
            automationAppend.reset();
        };
        auto completeLifecycleError = [&](woby::AutomationCommandId id, const woby::SceneLifecycleError& error) {
            woby::completeAutomationCommand(*automation, id,
                woby::AutomationCommandError{error.message, error.code, error, currentScenePath, ui.isDirty});
        };
        DragDropState dragDropState;
        std::optional<std::filesystem::path> pendingDirtyOpenScenePath;
        bool requestDirtyOpenWarning = false;
        bool requestDirtyQuitWarning = false;
        bool requestDirtyNewWarning = false;
        ToastMessage toast;
        if (importerLoadFailed) {
            setToastMessage(toast, "Some importers could not be loaded. See the log for details.");
        }
        uint64_t observedModelFileDialogStatusVersion = 0;
        uint64_t observedSceneFileDialogStatusVersion = 0;
        uint64_t observedSceneScreenshotDialogStatusVersion = 0;
        auto previousFrame = std::chrono::steady_clock::now();
        auto fpsWindowStart = previousFrame;
        int fpsFrameCount = 0;
        float fps = 0.0f;
        woby::FrameTimingAccumulator frameTimingAccumulator;
        woby::FrameTimings lastFrameTimings;
        uint64_t frameIndex = 0;
        HoverPickCache hoverPickCache;
        woby::SceneDimensionsCache dimensionsCache;
        woby::ScenePointerGesture scenePointer;
        std::optional<woby::ScenePickView> presentedPickView;
        woby::SceneViewport presentedViewport;
        bool scenePointerAvailable = false;
        std::vector<woby::SceneObjectId> canvasSelectionPath;
        bool documentShortcutHeld = false;
        woby::setAutomationReady(*automation);
        while (running) {
            woby::pollUiUpdate(updateRuntime);
            if (updateRuntime.state.closeRequested) {
                woby::requestQuit(ui);
                break;
            }
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
                if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                    scenePointer = {};
                    presentedPickView.reset();
                    getDrawableSize(window.get(), width, height);
                    bgfx::reset(width, height, resetFlags);
                }
                if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    scenePointer = {};
                    woby::setCameraOrbiting(ui, false);
                    woby::setCameraRolling(ui, false);
                    woby::setCameraPanning(ui, false);
                }
                const auto inputLayout = canvasLayout(window.get(), ui);
                const bool buttonEvent = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP;
                const auto inputMouse = buttonEvent
                    ? mousePositionInPixels(window.get(), event.button.x, event.button.y)
                    : mousePositionInPixels(window.get());
                const bool canvasInput = woby::contains(inputLayout.viewport, inputMouse.x, inputMouse.y);
                const bool pointerAllowed = scenePointerAvailable && !backgroundLoad.active && !gpuFinalize.active
                    && !modelFileDialogIsOpen(modelFileDialogState) && !sceneFileDialogIsOpen(sceneFileDialogState)
                    && !sceneScreenshotDialogIsOpen(sceneScreenshotDialogState) && !ImGui::GetIO().WantCaptureMouse;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && canvasInput && pointerAllowed) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        if (!cameraInput.panning) {
                            const auto modifiers = SDL_GetModState();
                            woby::beginScenePointer(scenePointer, {event.button.x, event.button.y},
                                (modifiers & SDL_KMOD_ALT) != 0u, (modifiers & SDL_KMOD_CTRL) != 0u);
                        }
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        scenePointer = {};
                        woby::setCameraOrbiting(ui, false);
                        woby::setCameraRolling(ui, false);
                        woby::setCameraPanning(ui, true);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (event.button.button == SDL_BUTTON_LEFT) {
                        const auto click = woby::endScenePointer(scenePointer, {event.button.x, event.button.y},
                            pointerAllowed && presentedPickView && woby::contains(presentedViewport, inputMouse.x, inputMouse.y));
                        if (click) {
                            // Resolve against the displayed camera/layout before selection opens Properties.
                            auto parts = woby::scenePickParts(ui);
                            woby::appendVisibleComparisonPickParts(parts, ui, comparison);
                            const auto id = woby::pickSceneObject(parts, *presentedPickView,
                                {inputMouse.x - static_cast<float>(presentedViewport.x), inputMouse.y});
                            if (id != woby::invalidSceneObjectId) {
                                woby::selectSceneObject(ui, id, click->toggle);
                                canvasSelectionPath = woby::sceneObjectSelected(ui, id)
                                    ? woby::sceneSelectionPath(ui, id) : std::vector<woby::SceneObjectId>{};
                            } else if (!click->toggle) {
                                woby::clearSceneSelection(ui);
                                canvasSelectionPath.clear();
                            }
                        }
                        woby::setCameraOrbiting(ui, false);
                        woby::setCameraRolling(ui, false);
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT || event.button.button == SDL_BUTTON_MIDDLE) {
                        woby::setCameraPanning(ui, false);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_MOTION) {
                    const bool wasDragging = scenePointer.dragging;
                    if (scenePointer.active && (!scenePointerAvailable || backgroundLoad.active || gpuFinalize.active)) {
                        scenePointer = {};
                        woby::setCameraOrbiting(ui, false);
                        woby::setCameraRolling(ui, false);
                    }
                    const bool dragging = woby::moveScenePointer(scenePointer, {event.motion.x, event.motion.y});
                    if (dragging) {
                        woby::setCameraOrbiting(ui, !scenePointer.alt);
                        woby::setCameraRolling(ui, scenePointer.alt);
                    }
                    const float dx = dragging && !wasDragging ? event.motion.x - scenePointer.start[0] : event.motion.xrel;
                    const float dy = dragging && !wasDragging ? event.motion.y - scenePointer.start[1] : event.motion.yrel;
                    if (cameraInput.orbiting) {
                        woby::orbitUiCamera(ui, dx, dy);
                    }
                    if (cameraInput.rolling) {
                        woby::rollUiCamera(ui, dx);
                    }
                    if (cameraInput.panning) {
                        const float aspect = static_cast<float>(inputLayout.viewport.width) / static_cast<float>(inputLayout.viewport.height);
                        const float panScale = 1.0f / std::min(aspect, 1.0f);
                        woby::panUiCamera(ui, event.motion.xrel * panScale, event.motion.yrel * panScale, inputLayout.height);
                    }
                }
                if (event.type == SDL_EVENT_MOUSE_WHEEL && canvasInput && !ImGui::GetIO().WantCaptureMouse) {
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
                    if (automationAppend) {
                        woby::completeAutomationCommand(*automation, automationAppend->id, woby::AutomationCommandError{outcome->error});
                        automationAppend.reset();
                    }
                    setToastMessage(toast, std::string(backgroundLoadFailurePrefix(outcome->kind)) + outcome->error);
                    if (automationOpenCommandId) {
                        completeLifecycleError(*automationOpenCommandId, {"open_failed", outcome->error, -32016});
                        automationOpenCommandId.reset();
                    }
                } else if (isAppendModelLoadKind(outcome->kind)) {
                    if (automationAppend) {
                        automationAppend->outcomes = outcome->modelBatch.outcomes;
                        automationAppend->canceled = outcome->modelBatch.canceled;
                    }
                    if (outcome->modelBatch.canceled || outcome->modelBatch.files.empty()) {
                        setToastMessage(toast, outcome->modelBatch.status);
                        completeAppend();
                    } else {
                        try { startGpuFinalize(gpuFinalize, std::move(outcome.value())); }
                        catch (const std::exception& error) {
                            abortGpuFinalize(gpuFinalize);
                            if (automationAppend) {
                                woby::completeAutomationCommand(*automation, automationAppend->id, woby::AutomationCommandError{error.what()});
                                automationAppend.reset();
                            } else { setToastMessage(toast, error.what()); }
                        }
                    }
                } else if (outcome->scene.canceled) {
                    setToastMessage(toast, "Open scene canceled");
                    if (automationOpenCommandId) {
                        completeLifecycleError(*automationOpenCommandId, {"scene_canceled", "Open scene canceled.", -32017});
                        automationOpenCommandId.reset();
                    }
                } else {
                    try {
                        startGpuFinalize(gpuFinalize, std::move(outcome.value()));
                    } catch (const std::exception& exception) {
                        abortGpuFinalize(gpuFinalize);
                        setToastMessage(toast, std::string("Open scene failed: ") + exception.what());
                        if (automationOpenCommandId) {
                            completeLifecycleError(*automationOpenCommandId, {"open_failed", exception.what(), -32016});
                            automationOpenCommandId.reset();
                        }
                    }
                }
            }

            const auto previousGpuFailures = gpuFinalize.gpuFailedCount;
            const auto finalizingPath = gpuFinalize.active && gpuFinalize.nextFileIndex < gpuFinalize.files.size()
                ? gpuFinalize.files[gpuFinalize.nextFileIndex].path : std::filesystem::path{};
            const auto finalized = processGpuFinalizeStep(
                gpuFinalize,
                layout,
                pointLayout,
                ui,
                runtimes,
                currentScenePath,
                cleanSceneDocument,
                toast);
            if (finalized) {
                woby::finishSceneHistoryInteraction(sceneHistory);
                woby::recordSceneHistory(sceneHistory, ui);
            }
            if (automationAppend && gpuFinalize.gpuFailedCount > previousGpuFailures) {
                for (auto& input : automationAppend->outcomes) {
                    if (input.path == finalizingPath) { input.state = "failed"; input.error = gpuFinalize.lastError; }
                }
            }
            if (finalized && automationAppend) {
                if (finalized->empty()) { completeAppend(); }
                else {
                    woby::completeAutomationCommand(*automation, automationAppend->id, woby::AutomationCommandError{*finalized});
                    automationAppend.reset();
                }
            }
            if (finalized && automationOpenCommandId) {
                if (finalized->empty()) {
                    woby::completeAutomationCommand(*automation, *automationOpenCommandId,
                        woby::AutomationSceneResult{currentScenePath, ui.isDirty});
                } else {
                    completeLifecycleError(*automationOpenCommandId, {"open_failed", *finalized, -32016});
                }
                automationOpenCommandId.reset();
            }

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
            const auto pendingFolderTreeRoots = takePendingModelFolderTreeRoots(modelFileDialogState);
            for (const auto& folderTreeRoot : pendingFolderTreeRoots) {
                if (backgroundLoad.active || gpuFinalize.active) {
                    setModelFileDialogStatus(modelFileDialogState, "Already processing files");
                    break;
                }

                std::string folderTreeStatus;
                if (!startAppendFolderTreeBackgroundLoad(
                        backgroundLoad,
                        folderTreeRoot,
                        woby::totalGroupCount(ui),
                        folderTreeStatus)) {
                    folderTreeStatus = "Open folder tree failed: already processing files";
                }
                if (!folderTreeStatus.empty()) {
                    setModelFileDialogStatus(modelFileDialogState, std::move(folderTreeStatus));
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
            const auto saveDocument = [&](const std::filesystem::path& path) {
                woby::recordSceneHistory(sceneHistory, ui, woby::sceneHistoryInteraction());
                woby::finishSceneHistoryInteraction(sceneHistory);
                try {
                    const auto savedPath = saveSceneToPath(path, ui, currentScenePath, cleanSceneDocument);
                    setToastMessage(toast, "Saved scene " + fileDisplayName(savedPath));
                } catch (const std::exception& exception) {
                    setToastMessage(toast, std::string("Save scene failed: ") + exception.what());
                }
            };
            if (const auto path = takePendingSaveScenePath(sceneFileDialogState)) {
                saveDocument(*path);
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
                    requestSceneScreenshotCapture(sceneScreenshot, pendingSceneScreenshotPath.value(), ui.screenshotSettings);
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

            updateAppWindowTitle(window.get(), currentScenePath, ui.isDirty, instanceId);

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

            updateUiScale(window.get(), ui, baseStyle);
            const float minViewerPaneWidth = minimumViewerPaneWidth();
            const float maxViewerPaneWidth = std::max(
                minViewerPaneWidth,
                canvasLayout(window.get(), ui).width - minSceneViewportWidth);
            woby::setViewerPaneWidth(ui, viewerPaneWidth, minViewerPaneWidth, maxViewerPaneWidth);

            bgfx::dbgTextClear();
            recordFrameStage(frameTimings, woby::FrameStage::stateUpdate, stageStart);

            const bool popupWasOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            bool modalDialogOpen = ImGui::IsPopupOpen("Settings");
            bool requestSettings = false;
            const auto newScene = [&]() {
                try {
                    resetSceneToUntitled(ui, runtimes, currentScenePath, cleanSceneDocument);
                    setToastMessage(toast, "Created new scene");
                } catch (const std::exception& error) {
                    setToastMessage(toast, std::string("New scene failed: ") + error.what());
                }
            };
            if (requestDirtyNewWarning) {
                ImGui::OpenPopup("Unsaved scene changes##new");
                requestDirtyNewWarning = false;
            }
            if (ImGui::BeginPopupModal("Unsaved scene changes##new", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                modalDialogOpen = true;
                ImGui::TextUnformatted("The current scene has unsaved changes.");
                ImGui::TextUnformatted("Create a new scene and discard them?");
                if (ImGui::Button("New scene")) {
                    newScene();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ImGui::CloseCurrentPopup(); }
                ImGui::EndPopup();
            }
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
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
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
                if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (drawProcessingDialog(backgroundLoad, gpuFinalize)) {
                modalDialogOpen = true;
            }
            // Recheck after each action: opening a dialog blocks other entry points in this frame.
            const auto fileActionsDisabled = [&]() {
                return modalDialogOpen || backgroundLoad.active || gpuFinalize.active
                    || modelFileDialogIsOpen(modelFileDialogState)
                    || sceneFileDialogIsOpen(sceneFileDialogState)
                    || sceneScreenshotDialogIsOpen(sceneScreenshotDialogState);
            };
            const auto documentCommand = [&](woby::SceneAction action) {
                if (fileActionsDisabled()) {
                    return;
                }
                if (action == woby::SceneAction::open) {
                    showOpenSceneDialog(window.get(), sceneFileDialogState);
                } else if (action == woby::SceneAction::saveAs || !currentScenePath) {
                    showSaveSceneDialog(window.get(), sceneFileDialogState);
                } else if (action == woby::SceneAction::save) {
                    saveDocument(*currentScenePath);
                }
            };
            const auto panelLayout = canvasLayout(window.get(), ui);
            auto historyCommand = woby::SceneHistoryCommand::none;
            if (ui.viewerPaneVisible) {
                const float availableHeight = panelLayout.height;
                ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
                ImGui::SetNextWindowSize(
                    ImVec2(panelLayout.leftWidth, availableHeight),
                    ImGuiCond_Always);
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2(std::min(minViewerPaneWidth, panelLayout.leftWidth), availableHeight),
                    ImVec2(panelLayout.maxLeftWidth, availableHeight));
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

                    drawViewerPaneToggleButton(ui);
                    ImGui::SameLine();
                    ImGui::TextUnformatted("Scene controls");
                    ImGui::SameLine();
                    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x
                        - 3.0f * renderModeButtonSize() - 2.0f * ImGui::GetStyle().ItemSpacing.x);
                    historyCommand = woby::drawSceneHistoryToolbar(
                        woby::canUndoScene(sceneHistory), woby::canRedoScene(sceneHistory),
                        fileActionsDisabled() || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending);
                    ImGui::SameLine();
                    if (woby::drawSettingsButton(fileActionsDisabled())) { requestSettings = true; }
                    ImGui::Separator();
                    const float statusHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f
                        + ImGui::GetStyle().ItemSpacing.y + 1.0f;
                    const float actionWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                    ImGui::BeginDisabled(fileActionsDisabled()
                        || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending);
                    if (ImGui::Button("New scene##new_scene", ImVec2(actionWidth, 0.0f))) {
                        woby::updateSceneDirty(ui, cleanSceneDocument);
                        if (ui.isDirty) {
                            requestDirtyNewWarning = true;
                            modalDialogOpen = true;
                        } else {
                            newScene();
                        }
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Open scene...##open_scene", ImVec2(actionWidth, 0.0f))) {
                        documentCommand(woby::SceneAction::open);
                    }
                    setLastItemTooltip("Open scene (Ctrl+O)");
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Save scene##save_scene", ImVec2(actionWidth, 0.0f))) {
                        documentCommand(woby::SceneAction::save);
                    }
                    setLastItemTooltip("Save scene (Ctrl+S)");
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Save scene as...##save_scene_as", ImVec2(actionWidth, 0.0f))) {
                        documentCommand(woby::SceneAction::saveAs);
                    }
                    setLastItemTooltip("Save scene as (Ctrl+Shift+S)");
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Add models...##add_model_file", ImVec2(actionWidth, 0.0f))) {
                        showModelFileDialog(window.get(), modelFileDialogState);
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Add model folder...##add_model_folder_tree", ImVec2(actionWidth, 0.0f))) {
                        showModelFolderTreeDialog(window.get(), modelFileDialogState);
                    }
                    ImGui::EndDisabled();
                    ImGui::Separator();
                    const bool scenePaneOpen = woby::drawInformationHeader("Display", "Display settings",
                        "Inspection presets apply to all current parts and hide grid and origin. "
                        "Visibility and transforms stay as set.\n\nVertex size sets the base vertex point size for all groups.");
                if (scenePaneOpen) {
                    ImGui::SetNextItemWidth(-1.0f);
                    if (ImGui::BeginCombo("##inspection_preset", "Inspection presets")) {
                        if (ImGui::Selectable("Solid")) { woby::applyInspectionPreset(ui, woby::UiInspectionPreset::solid); }
                        if (ImGui::Selectable("Solid + edges")) { woby::applyInspectionPreset(ui, woby::UiInspectionPreset::edges); }
                        if (ImGui::Selectable("Solid + edges + vertices")) { woby::applyInspectionPreset(ui, woby::UiInspectionPreset::vertices); }
                        ImGui::EndCombo();
                    }
                    const float sceneContentHeight = renderModeButtonSize() * 3.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
                    if (ImGui::BeginChild(
                            "SceneContent",
                            ImVec2(0.0f, sceneContentHeight),
                            ImGuiChildFlags_None)) {
                        drawCameraToolbar(ui);
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
                        ImGui::SameLine();
                        ImGui::BeginDisabled(fileActionsDisabled()
                            || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending);
                        if (drawRenderModeIconButton("scene_screenshot", screenshotIcon,
                                "Export PNG", RenderModeState::off, false)) {
                            ImGui::OpenPopup("Export PNG");
                        }
                        ImGui::EndDisabled();
                        if (woby::drawSceneScreenshotOptions(ui)) {
                            showSaveSceneScreenshotDialog(window.get(), sceneScreenshotDialogState);
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
                                "Triangle edges",
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
                    }
                    ImGui::EndChild();
                }

                if (scenePaneOpen) {
                    bool showDimensions = ui.showDimensions;
                    if (ImGui::Checkbox("Show dimensions", &showDimensions)) {
                        woby::setShowDimensions(ui, showDimensions);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Size of visible selected geometry, including parent transforms.");
                    }
                    if (showDimensions && ui.selectedSceneObjects.empty()) {
                        ImGui::TextDisabled("Select a mesh to see its dimensions.");
                    }
                }

                ImGui::BeginDisabled(fileActionsDisabled()
                    || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending);
                woby::drawViews(ui, viewNameEdit);
                ImGui::EndDisabled();

                const std::string filesPaneTitle = "Objects (" + std::to_string(files.size()) + " files)##Files";
                if (!canvasSelectionPath.empty() && !woby::sceneObjectSelected(ui, canvasSelectionPath.back())) { canvasSelectionPath.clear(); }
                if (!canvasSelectionPath.empty()) { ImGui::SetNextItemOpen(true); }
                const bool filesPaneOpen = ImGui::CollapsingHeader(
                    filesPaneTitle.c_str(),
                    ImGuiTreeNodeFlags_DefaultOpen);
                if (filesPaneOpen) {
                    const float filesContentHeight = std::max(
                        ImGui::GetContentRegionAvail().y - statusHeight - ImGui::GetStyle().ItemSpacing.y,
                        ImGui::GetFrameHeight());
                    if (ImGui::BeginChild(
                            "FilesContent",
                            ImVec2(0.0f, filesContentHeight),
                            ImGuiChildFlags_None)) {
                        if (files.empty() && ui.comparisons.empty()) {
                            ImGui::TextDisabled("No objects yet.");
                        }
                        std::optional<size_t> removeFileIndex;
                        for (size_t nodeIndex = 0; nodeIndex < ui.sceneNodes.size(); ++nodeIndex) {
                            ImGui::PushID(static_cast<int>(nodeIndex));
                            drawSceneTreeNode(ui, runtimes, ui.sceneNodes[nodeIndex], removeFileIndex, canvasSelectionPath);
                            ImGui::PopID();
                        }
                        woby::drawComparisonObjects(ui, comparisonNameEdit);
                        canvasSelectionPath.clear();
                        if (removeFileIndex.has_value() && removeFileIndex.value() < files.size()) {
                            const std::string removedName = fileDisplayName(files[removeFileIndex.value()].path);
                            removeModelFile(ui, runtimes, removeFileIndex.value());
                            setToastMessage(toast, "Removed " + removedName);
                        }
                    }
                    ImGui::EndChild();
                }
                ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(),
                    ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y
                        - statusHeight));
                ImGui::Separator();
                size_t vertexCountTotal = 0;
                size_t triangleCountTotal = 0;
                for (const auto& file : files) {
                    vertexCountTotal += file.mesh.vertices.size();
                    triangleCountTotal += file.mesh.indices.size() / 3u;
                }
                ImGui::TextDisabled("%zu vertices | %zu triangles", vertexCountTotal, triangleCountTotal);
                ImGui::TextDisabled("%s | %.1f FPS", bgfx::getRendererName(bgfx::getRendererType()), fps);
            }
                ImGui::End();
            }
            drawPaneToggles(ui, panelLayout.width, fileActionsDisabled(), requestSettings);
            if (files.empty() && ui.comparisons.empty() && !backgroundLoad.active && !gpuFinalize.active) {
                const float viewportWidth = panelLayout.width - panelLayout.leftWidth
                    - (ui.propertiesPaneVisible ? panelLayout.rightWidth : 0.0f);
                ImGui::SetNextWindowPos(
                    ImVec2(panelLayout.leftWidth + viewportWidth * 0.5f, panelLayout.height * 0.5f),
                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(std::max(120.0f, std::min(420.0f, viewportWidth - 24.0f)), 0.0f));
                if (ImGui::Begin("##EmptyScene", nullptr, ImGuiWindowFlags_NoDecoration
                    | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove
                    | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
                    ImGui::TextWrapped("Start a scene");
                    ImGui::TextWrapped("Add models to inspect, or open a saved scene.");
                    ImGui::Spacing();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Add models", ImVec2(-1.0f, 0.0f))) {
                        showModelFileDialog(window.get(), modelFileDialogState);
                    }
                    ImGui::EndDisabled();
                    ImGui::BeginDisabled(fileActionsDisabled());
                    if (ImGui::Button("Open scene", ImVec2(-1.0f, 0.0f))) {
                        documentCommand(woby::SceneAction::open);
                    }
                    setLastItemTooltip("Open scene (Ctrl+O)");
                    ImGui::EndDisabled();
                    ImGui::Spacing();
                    ImGui::TextWrapped("Models: OBJ, STL and installed importer formats. Scenes: .woby.");
                    ImGui::TextWrapped("Tip: drop model files, folders or a .woby scene into this window.");
                }
                ImGui::End();
            }
            drawPropertiesPane(ui, comparison, panelLayout, dimensionsCache);
            const auto settings = woby::drawSettingsDialog(ui, requestSettings, updateRuntime.state);
            modalDialogOpen = modalDialogOpen || settings.open;
            if (settings.updateCommand != woby::UpdateCommand::none) {
                woby::startUiUpdate(updateRuntime, settings.updateCommand, ui.isDirty, deploymentGuard);
            }
            if (settings.scaleChanged && !preferencePath.empty()) {
                std::ofstream preference(preferencePath);
                if (!(preference << ui.uiScale)) { setToastMessage(toast, "Could not save UI scale preference"); }
            }
            recordFrameStage(frameTimings, woby::FrameStage::imguiBuild, stageStart);
            if (woby::recordSceneHistory(sceneHistory, ui, woby::sceneHistoryInteraction())) {
                woby::updateSceneDirty(ui, cleanSceneDocument);
            }

            woby::recalculateSceneBounds(ui);
            updateAppWindowTitle(window.get(), currentScenePath, ui.isDirty, instanceId);
            // Both UI and CTL use the same restoration and failure-consumption path.
            const auto runSceneHistory = [&](bool redo) {
                woby::finishSceneHistoryInteraction(sceneHistory);
                try {
                    const bool applied = applySceneHistory(sceneHistory, ui, cleanSceneDocument, runtimes, layout, pointLayout, redo);
                    if (applied) {
                        setToastMessage(toast, redo ? "Redid scene edit" : "Undid scene edit");
                        updateAppWindowTitle(window.get(), currentScenePath, ui.isDirty, instanceId);
                    }
                    return applied;
                } catch (const std::exception& error) {
                    woby::skipSceneHistoryStep(sceneHistory, ui, redo);
                    const auto message = std::string(redo ? "Redo skipped: " : "Undo skipped: ") + error.what();
                    setToastMessage(toast, message);
                    throw std::runtime_error(message);
                }
            };
            // UI and loading commits have finished. No logical edits occur between
            // executing commands here and submitting their screenshots below.
            if (automationComparison) {
                const auto& pending = *automationComparison;
                const auto it = comparison.objects.find(pending.objectId);
                const bool both = woby::enabledComparisonPartCount(ui, woby::ComparisonSide::a, pending.objectId) != 0
                    && woby::enabledComparisonPartCount(ui, woby::ComparisonSide::b, pending.objectId) != 0;
                const bool changed = pending.sceneGeneration != ui.sceneGeneration
                    || !woby::findComparison(ui, pending.objectId)
                    || pending.signature != woby::comparisonGeometrySignature(ui, pending.objectId)
                    || pending.stages != woby::requestedComparisonStages(woby::comparisonSettings(ui, pending.objectId), both, true);
                const bool ready = !changed && it != comparison.objects.end()
                    && woby::comparisonResultsReady(it->second, ui, pending.objectId, true);
                const bool failed = it != comparison.objects.end() && !it->second.error.empty()
                    && it->second.attemptedSignature == pending.signature;
                if (changed || ready || failed) {
                    try {
                        if (changed) { throw std::runtime_error("Analysis inputs or detectors changed while results were being requested; retry analysis.results."); }
                        if (!ready) { throw std::runtime_error(it->second.error); }
                        woby::setComparisonDuplicateEnabled(it->second.result, woby::comparisonSettings(ui, pending.objectId).duplicates);
                        auto result = woby::controlComparisonResults(it->second.result, pending.tolerance);
                        result["target"] = pending.target;
                        woby::completeAutomationCommand(*automation, pending.id, woby::AutomationControlResult{std::move(result)});
                    } catch (const std::exception& error) {
                        woby::completeAutomationCommand(*automation, pending.id, woby::AutomationCommandError{error.what()});
                    }
                    if (it != comparison.objects.end()) { it->second.fullResultsRequested = false; }
                    automationComparison.reset();
                }
            }
            for (size_t executed = 0; executed < woby::maxAutomationCommands; ++executed) {
                const auto command = woby::takeAutomationCommand(*automation);
                if (!command) {
                    break;
                }
                const auto beforeCommandRevision = ui.sceneEditRevision;
                const auto beforeCommandGeneration = ui.sceneGeneration;
                try {
                    std::visit([&](const auto& payload) {
                        using Command = std::decay_t<decltype(payload)>;
                        if constexpr (std::is_same_v<Command, woby::AutomationScreenshotCommand>) {
                            if (backgroundLoad.active || gpuFinalize.active) {
                                throw std::runtime_error("Cannot capture while model files are being processed.");
                            }
                            requestSceneScreenshotCapture(sceneScreenshot, payload.outputPath, ui.screenshotSettings);
                            automationScreenshotCommandId = command->id;
                        } else if constexpr (std::is_same_v<Command, woby::AutomationObjectsCommand>) {
                            woby::completeAutomationCommand(*automation, command->id,
                                woby::AutomationObjectsResult{woby::sceneObjects(ui)});
                        } else if constexpr (std::is_same_v<Command, woby::ControlOperation>) {
                            using A = woby::ControlAction;
                            using Json = nlohmann::json;
                            const bool historyOperation = payload.action == A::sceneUndo || payload.action == A::sceneRedo;
                            const bool busy = backgroundLoad.active || gpuFinalize.active
                                || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending
                                || modalDialogOpen || modelFileDialogIsOpen(modelFileDialogState)
                                || sceneFileDialogIsOpen(sceneFileDialogState) || sceneScreenshotDialogIsOpen(sceneScreenshotDialogState)
                                || (historyOperation && (ImGui::IsAnyItemActive() || ImGui::GetIO().WantTextInput));
                            for (const auto id : {payload.objectId, payload.aId, payload.bId, payload.memberId}) {
                                if (id != woby::invalidSceneObjectId && !woby::findSceneObject(ui, id)) {
                                    woby::completeAutomationCommand(*automation, command->id,
                                        woby::AutomationCommandError{"Unknown or stale object ID.", -32005});
                                    return;
                                }
                            }
                            if (busy && (woby::controlMethod(payload.action).mutating || payload.action == A::comparisonResults)) {
                                woby::completeAutomationCommand(*automation, command->id,
                                    woby::AutomationCommandError{"Scene is busy loading, capturing, displaying a dialog, or editing a widget.", -32014});
                                return;
                            }
                            Json result;
                            if (historyOperation) {
                                const bool redo = payload.action == A::sceneRedo;
                                const bool applied = runSceneHistory(redo);
                                result = {{"action", redo ? "redo" : "undo"}, {"applied", applied}, {"dirty", ui.isDirty}};
                            } else if (payload.action == A::comparisonResults) {
                                const auto* source = woby::findComparison(ui, payload.objectId);
                                if (!source) { throw std::invalid_argument("analysis.results requires an analysis ID."); }
                                if (!woby::canInspectComparison(ui, payload.objectId)) {
                                    throw std::invalid_argument("Analysis needs at least one populated input and no missing references.");
                                }
                                if (automationComparison) { throw std::runtime_error("Another analysis.results request is still computing."); }
                                AutomationComparisonRuntime pending;
                                pending.id = command->id;
                                pending.target = payload.target;
                                pending.objectId = payload.objectId;
                                pending.tolerance = source->settings.tolerance;
                                pending.signature = woby::comparisonGeometrySignature(ui, payload.objectId);
                                pending.sceneGeneration = ui.sceneGeneration;
                                const bool both = woby::enabledComparisonPartCount(ui, woby::ComparisonSide::a, payload.objectId) != 0
                                    && woby::enabledComparisonPartCount(ui, woby::ComparisonSide::b, payload.objectId) != 0;
                                pending.stages = woby::requestedComparisonStages(source->settings, both, true);
                                comparison.objects[payload.objectId].fullResultsRequested = true;
                                automationComparison.emplace(std::move(pending));
                                return;
                            } else if (payload.action == A::status) {
                                result = woby::automationInstanceInfo(*automation);
                                result.update({{"path", currentScenePath ? Json(woby::pathToUtf8(*currentScenePath)) : Json(nullptr)},
                                    {"dirty", ui.isDirty}, {"loading", backgroundLoad.active}, {"gpuFinalizing", gpuFinalize.active},
                                    {"capturing", sceneScreenshot.captureRequested || sceneScreenshot.readbackPending},
                                    {"busy", busy}, {"version", WOBY_VERSION},
                                    {"pane", {{"visible", ui.viewerPaneVisible}, {"width", ui.viewerPaneWidth}}}});
                            } else if (payload.action == A::capabilities) {
                                result = woby::controlCapabilities();
                                result["limits"] = {{"admittedCommands", woby::maxAutomationCommands},
                                    {"retainedResults", woby::maxAutomationHistory}, {"requestKeys", woby::maxAutomationRequestKeys},
                                    {"timeoutSeconds", {1, 3600}}, {"vertexPixels", {woby::minVertexPointSize, woby::maxVertexPointSize}},
                                    {"scale", {woby::minGroupScale, woby::maxGroupScale}}};
                                result["importers"] = woby::controlImporterInfo(rememberedImporters);
                            } else if (payload.action == A::modelAdd || payload.action == A::folderAdd) {
                                auto paths = payload.action == A::folderAdd ? woby::collectModelPathsRecursive(payload.path)
                                    : std::vector<std::filesystem::path>{payload.path};
                                if (!startAppendModelBackgroundLoad(backgroundLoad, std::move(paths), woby::totalGroupCount(ui),
                                    payload.tree ? AsyncLoadKind::appendFolderTree : AsyncLoadKind::appendModel,
                                    payload.tree ? payload.path : std::filesystem::path{})) {
                                    throw std::runtime_error("Already processing files.");
                                }
                                automationAppend = AutomationAppendRuntime{command->id, ui.files.size(), {}, false};
                                return;
                            } else if (payload.action == A::modelRemove) {
                                const auto found = std::find_if(ui.files.begin(), ui.files.end(), [&](const auto& file) { return file.objectId == payload.objectId; });
                                if (found == ui.files.end()) { throw std::invalid_argument("model.remove requires a file ID."); }
                                removeModelFile(ui, runtimes, static_cast<size_t>(found - ui.files.begin()));
                                woby::updateSceneDirty(ui, cleanSceneDocument);
                                result = {{"removed", payload.target}, {"dirty", ui.isDirty}};
                            } else if (payload.action == A::importersList || payload.action == A::importersAdd
                                || payload.action == A::importersScan || payload.action == A::importersForget) {
                                result = woby::applyControlImporterOperation(payload, importerSettingsPath, rememberedImporters);
                            } else if (payload.action == A::performance) {
                                result = {{"frameIndex", lastFrameTimings.frameIndex}, {"fps", fps},
                                    {"frameMilliseconds", lastFrameTimings.totalMilliseconds},
                                    {"cpuFrameMilliseconds", lastFrameTimings.bgfxCpuFrameMilliseconds},
                                    {"cpuSubmitMilliseconds", lastFrameTimings.bgfxCpuSubmitMilliseconds},
                                    {"gpuFrameMilliseconds", lastFrameTimings.hasBgfxGpuFrameMilliseconds ? Json(lastFrameTimings.bgfxGpuFrameMilliseconds) : Json(nullptr)}};
                                result["stagesMilliseconds"] = Json::object();
                                for (size_t stage = 0; stage < lastFrameTimings.stageMilliseconds.size(); ++stage) {
                                    result["stagesMilliseconds"][woby::frameStageName(static_cast<woby::FrameStage>(stage))] = lastFrameTimings.stageMilliseconds[stage];
                                }
                            } else {
                                result = woby::applyControlSceneOperation(ui, cleanSceneDocument, payload, formatObjectId, minViewerPaneWidth, maxViewerPaneWidth);
                                if (payload.action == A::sceneInfo) { result["path"] = currentScenePath ? Json(woby::pathToUtf8(*currentScenePath)) : Json(nullptr); }
                                if (payload.action == A::stats) { result["renderer"] = bgfx::getRendererName(bgfx::getRendererType()); result["fps"] = fps; }
                            }
                            woby::completeAutomationCommand(*automation, command->id, woby::AutomationControlResult{std::move(result)});
                        } else if constexpr (std::is_same_v<Command, woby::SceneLifecycleCommand>) {
                            const bool busy = backgroundLoad.active || gpuFinalize.active
                                || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending
                                || modalDialogOpen || modelFileDialogIsOpen(modelFileDialogState)
                                || sceneFileDialogIsOpen(sceneFileDialogState)
                                || sceneScreenshotDialogIsOpen(sceneScreenshotDialogState);
                            if (const auto error = woby::beginSceneLifecycle(payload, ui, currentScenePath, cleanSceneDocument, busy)) {
                                completeLifecycleError(command->id, *error);
                                return;
                            }
                            if (payload.action == woby::SceneAction::open) {
                                if (!startOpenSceneBackgroundLoad(backgroundLoad, payload.path)) {
                                    completeLifecycleError(command->id, {"scene_busy", "Already processing files.", -32014});
                                } else {
                                    automationOpenCommandId = command->id;
                                }
                                return;
                            }
                            if (payload.action == woby::SceneAction::newScene) {
                                resetSceneToUntitled(ui, runtimes, currentScenePath, cleanSceneDocument);
                            }
                            const bool quit = payload.action == woby::SceneAction::quit;
                            woby::completeAutomationCommand(*automation, command->id,
                                woby::AutomationSceneResult{currentScenePath, ui.isDirty, quit});
                            if (quit) { woby::requestQuit(ui); }
                        } else {
                            static_assert(std::is_same_v<Command, woby::AutomationObjectCommand>);
                            const auto object = woby::findSceneObject(ui, payload.objectId);
                            if (object) {
                                woby::completeAutomationCommand(*automation, command->id,
                                    woby::AutomationObjectResult{*object, woby::controlObjectDetails(ui, payload.objectId, formatObjectId)});
                            } else {
                                woby::completeAutomationCommand(*automation, command->id,
                                    woby::AutomationCommandError{"Unknown or stale object ID.", -32005});
                            }
                        }
                    }, command->payload);
                } catch (const std::invalid_argument& exception) {
                    woby::completeAutomationCommand(*automation, command->id, woby::AutomationCommandError{exception.what(), -32602});
                } catch (const std::exception& exception) {
                    const auto* lifecycle = std::get_if<woby::SceneLifecycleCommand>(&command->payload);
                    if (lifecycle && lifecycle->action == woby::SceneAction::open) {
                        completeLifecycleError(command->id, {"open_failed", exception.what(), -32016});
                    } else {
                        woby::completeAutomationCommand(*automation, command->id, woby::AutomationCommandError{exception.what()});
                    }
                }
                if (beforeCommandRevision != ui.sceneEditRevision
                    || beforeCommandGeneration != ui.sceneGeneration) {
                    woby::finishSceneHistoryInteraction(sceneHistory);
                    woby::recordSceneHistory(sceneHistory, ui);
                }
                if (const auto* lifecycle = std::get_if<woby::SceneLifecycleCommand>(&command->payload);
                    lifecycle && (lifecycle->action == woby::SceneAction::save
                        || lifecycle->action == woby::SceneAction::saveAs)) {
                    woby::finishSceneHistoryInteraction(sceneHistory);
                }
            }
            recordFrameStage(frameTimings, woby::FrameStage::sceneState, stageStart);

            // Global document commands still work when the scene controls are hidden.
            // Run after widgets have applied edits so Save includes this frame's changes.
            const auto historyShortcut = woby::sceneHistoryShortcut(fileActionsDisabled()
                || sceneScreenshot.captureRequested || sceneScreenshot.readbackPending
                || (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) == 0u);
            if (historyShortcut != woby::SceneHistoryCommand::none) { historyCommand = historyShortcut; }
            if (historyCommand != woby::SceneHistoryCommand::none && !fileActionsDisabled()
                && !sceneScreenshot.captureRequested && !sceneScreenshot.readbackPending) {
                try {
                    (void)runSceneHistory(historyCommand == woby::SceneHistoryCommand::redo);
                } catch (const std::exception&) {
                    // The shared adapter already consumed the action and displayed the error.
                }
            }
            if (!fileActionsDisabled() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
                if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, ImGuiInputFlags_RouteGlobal)) {
                    documentCommand(woby::SceneAction::open);
                    documentShortcutHeld = true;
                } else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
                    documentCommand(woby::SceneAction::saveAs);
                    documentShortcutHeld = true;
                } else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) {
                    documentCommand(woby::SceneAction::save);
                    documentShortcutHeld = true;
                }
            }
            if (!ImGui::IsKeyDown(ImGuiKey_S) && !ImGui::IsKeyDown(ImGuiKey_O)) {
                documentShortcutHeld = false;
            }
            // Read keyboard input after widgets have claimed it, before drawing the scene.
            woby::dismissPopupOnEscape();
            const auto& keyboardIo = ImGui::GetIO();
            const bool sceneKeyboardAvailable = !fileActionsDisabled() && !documentShortcutHeld
                && (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) != 0u
                && !keyboardIo.WantCaptureKeyboard && !keyboardIo.WantTextInput
                && !ImGui::IsAnyItemActive()
                && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
            if (sceneKeyboardAvailable) {
                if (!keyboardIo.KeyCtrl && !keyboardIo.KeyAlt && !keyboardIo.KeySuper) {
                    if (!popupWasOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                        woby::clearSceneSelection(ui);
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                        woby::fitCameraToScene(ui);
                    }
                }
                if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_B)) {
                    woby::toggleViewerPaneVisible(ui);
                }
                woby::updateCameraFromKeyboard(ui, deltaSeconds);
            }

            woby::updateComparisonRuntimes(comparison, ui);

            const auto viewport = canvasLayout(window.get(), ui).viewport;
            const uint32_t sceneViewportWidth = viewport.width;
            bgfx::setViewRect(clearView, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
            bgfx::touch(clearView);
            bgfx::setViewRect(
                sceneView,
                static_cast<uint16_t>(viewport.x),
                0,
                static_cast<uint16_t>(sceneViewportWidth),
                static_cast<uint16_t>(height));
            bgfx::setViewRect(
                helperView,
                static_cast<uint16_t>(viewport.x),
                0,
                static_cast<uint16_t>(sceneViewportWidth),
                static_cast<uint16_t>(height));
            bgfx::touch(sceneView);
            bgfx::touch(helperView);

            const bool homogeneousDepth = bgfx::getCaps()->homogeneousDepth;
            const auto currentPickView = woby::scenePickView(camera, ui.upAxis, sceneBounds,
                sceneViewportWidth, height, homogeneousDepth,
                static_cast<float>(width) / canvasLayout(window.get(), ui).width);
            const auto* view = currentPickView.view.data();
            const auto* projection = currentPickView.projection.data();
            presentedPickView = currentPickView;
            presentedViewport = viewport;
            bgfx::setViewTransform(sceneView, view, projection);
            bgfx::setViewTransform(helperView, view, projection);
            recordFrameStage(frameTimings, woby::FrameStage::viewSetup, stageStart);

            std::optional<HoveredVertex> hoveredVertex;
            const MousePosition windowMouse = mousePositionInPixels(window.get());
            const bool mouseInsideViewport = woby::contains(viewport, windowMouse.x, windowMouse.y);
            const MousePosition mouse{windowMouse.x - static_cast<float>(viewport.x), windowMouse.y};
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
            scenePointerAvailable = !dialogOpen && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)
                && (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) != 0u;
            if (!scenePointerAvailable) {
                scenePointer = {};
                woby::setCameraOrbiting(ui, false);
                woby::setCameraRolling(ui, false);
                woby::setCameraPanning(ui, false);
            }
            const bool hoverPickingEnabled = mouseInsideViewport
                && !ImGui::GetIO().WantCaptureMouse
                && !cameraInteractionActive
                && !dialogOpen;
            if (!hoverPickingEnabled) {
                hoverPickCache.hoveredVertex.reset();
                hoverPickCache.valid = false;
            } else {
                const uint64_t hoverSignature = hoverPickSignature(
                    files,
                    ui.sceneNodes,
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
                        ui.sceneNodes,
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

            bgfx::setViewMode(sceneView, bgfx::ViewMode::Sequential);
            {
                submitSceneFiles(
                    sceneView,
                    files,
                    ui.sceneNodes,
                    runtimes,
                    masterVertexPointSize,
                    meshProgram,
                    colorProgram,
                    pointSpriteProgram,
                    colorUniform,
                    pointParamsUniform,
                    sceneViewportWidth,
                    height);
            }
            woby::submitComparisonScenes(sceneView, ui, comparison, colorProgram, colorUniform);
            recordFrameStage(frameTimings, woby::FrameStage::submitScene, stageStart);

            submitSceneHelpers(helperView, ui, helperLayout, colorProgram, colorUniform);
            if (!ui.selectedSceneObjects.empty()) {
                auto selectedParts = woby::scenePickParts(ui);
                woby::appendVisibleComparisonPickParts(selectedParts, ui, comparison);
                woby::submitSceneSelection(helperView, selectedParts, helperLayout, colorProgram, colorUniform);
                if (ui.showDimensions) {
                    woby::updateSceneDimensions(dimensionsCache, selectedParts, ui.sceneGeneration, ui.sceneEditRevision);
                }
            }
            if (ui.showGrid || ui.showDimensions) {
                const float pixelScale = 1.0f / currentPickView.pixelScale;
                woby::drawSceneScaleOverlay(*ImGui::GetBackgroundDrawList(), ui,
                    ui.selectedSceneObjects.empty() ? std::nullopt : dimensionsCache.dimensions,
                    currentPickView, {static_cast<float>(viewport.x) * pixelScale, 0}, pixelScale, ImGui::GetFontSize());
            }
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
                    homogeneousDepth,
                    &comparison);
            } catch (const std::exception& exception) {
                failSceneScreenshotCapture(sceneScreenshot);
                if (automationScreenshotCommandId) {
                    woby::completeAutomationCommand(*automation, *automationScreenshotCommandId,
                        woby::AutomationCommandError{exception.what()});
                    automationScreenshotCommandId.reset();
                }
                setToastMessage(toast, std::string("Save screenshot failed: ") + exception.what());
            }

            drawToastMessage(toast, viewport, width);
            drawHoveredVertexOverlay(hoveredVertex, viewport, width);
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
                    if (automationScreenshotCommandId) {
                        woby::completeAutomationCommand(*automation, *automationScreenshotCommandId,
                            woby::AutomationScreenshotResult{sceneScreenshot.outputPath});
                        automationScreenshotCommandId.reset();
                    }
                }
            } catch (const std::exception& exception) {
                sceneScreenshot.readbackPending = false;
                if (automationScreenshotCommandId) {
                    woby::completeAutomationCommand(*automation, *automationScreenshotCommandId,
                        woby::AutomationCommandError{exception.what()});
                    automationScreenshotCommandId.reset();
                }
                setToastMessage(toast, std::string("Save screenshot failed: ") + exception.what());
            }
            recordFrameStage(frameTimings, woby::FrameStage::bgfxFrame, stageStart);
            frameTimings.totalMilliseconds = woby::millisecondsBetween(frameStart, woby::PerformanceClock::now());
            copyBgfxStats(frameTimings);
            lastFrameTimings = frameTimings;
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

        updateRuntime.worker.request_stop();
        if (updateRuntime.worker.joinable()) { updateRuntime.worker.join(); }
        automation.reset();
        automationComparison.reset();
        backgroundLoad.cancelRequested.store(true);
        if (backgroundLoad.worker.joinable()) {
            backgroundLoad.worker.join();
        }
        abortGpuFinalize(gpuFinalize);
        woby::unloadImporters();

        woby::imgui_bgfx::shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();

        bgfx::destroy(pointParamsUniform);
        woby::destroyComparisonRuntimes(comparison);
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
        automation.reset();
        woby::unloadImporters();
        std::fprintf(stderr, "%s\n", exception.what());
        if (!woby::hasStandardError()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Woby could not continue", exception.what(), nullptr);
        }
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
