#include "scene_screenshot.h"
#include "annotation_ui.h"
#include "scene_scale_overlay.h"
#include "comparison_view.h"
#include "comparison_scene.h"
#include "comparison_legend.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "imgui_graphics.h"

#include <SDL3/SDL.h>
#include <bx/math.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace woby {
namespace {

constexpr woby::graphics::ViewId screenshotSceneView = 16;
constexpr woby::graphics::ViewId screenshotHelperView = 17;
constexpr woby::graphics::ViewId screenshotAnnotationView = 18;
constexpr woby::graphics::ViewId screenshotReadbackView = 19;

std::string fileDisplayName(const std::filesystem::path& path)
{
    const auto filenameUtf8 = path.filename().u8string();
    const std::string filename(filenameUtf8.begin(), filenameUtf8.end());
    if (!filename.empty()) {
        return filename;
    }
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path pngPath(const std::filesystem::path& path)
{
    const auto extensionUtf8 = path.extension().u8string();
    std::string extension(extensionUtf8.begin(), extensionUtf8.end());
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

    std::filesystem::path outputPath = path;
    if (extension != ".png") {
        outputPath.replace_extension(".png");
    }

    return outputPath;
}

void ensureSceneScreenshotFramebuffer(SceneScreenshotRuntime& screenshot)
{
    if (woby::graphics::isValid(screenshot.frameBuffer)
        && screenshot.width == screenshot.options.width && screenshot.height == screenshot.options.height) {
        return;
    }
    destroySceneScreenshotFramebuffer(screenshot);
    screenshot.width = static_cast<uint16_t>(screenshot.options.width);
    screenshot.height = static_cast<uint16_t>(screenshot.options.height);
    if (screenshot.width > woby::graphics::getCaps()->limits.maxTextureSize
        || screenshot.height > woby::graphics::getCaps()->limits.maxTextureSize) {
        throw std::runtime_error("Export resolution exceeds the renderer texture limit.");
    }

    validateSceneScreenshotRenderer();

    constexpr uint64_t colorFlags = WOBY_GPU_TEXTURE_RT
        | WOBY_GPU_SAMPLER_U_CLAMP
        | WOBY_GPU_SAMPLER_V_CLAMP;
    constexpr uint64_t depthFlags = WOBY_GPU_TEXTURE_RT
        | WOBY_GPU_SAMPLER_U_CLAMP
        | WOBY_GPU_SAMPLER_V_CLAMP;
    constexpr uint64_t readbackFlags = WOBY_GPU_TEXTURE_BLIT_DST
        | WOBY_GPU_TEXTURE_READ_BACK
        | WOBY_GPU_SAMPLER_U_CLAMP
        | WOBY_GPU_SAMPLER_V_CLAMP;
    if (!woby::graphics::isTextureValid(0, false, 1, woby::graphics::TextureFormat::BGRA8, colorFlags)) {
        throw std::runtime_error("Renderer cannot create screenshot color target.");
    }
    if (!woby::graphics::isTextureValid(0, false, 1, woby::graphics::TextureFormat::D24S8, depthFlags)) {
        throw std::runtime_error("Renderer cannot create screenshot depth target.");
    }
    if (!woby::graphics::isTextureValid(0, false, 1, woby::graphics::TextureFormat::BGRA8, readbackFlags)) {
        throw std::runtime_error("Renderer cannot create screenshot readback target.");
    }

    screenshot.colorTexture = woby::graphics::createTexture2D(
        screenshot.width,
        screenshot.height,
        false,
        1,
        woby::graphics::TextureFormat::BGRA8,
        colorFlags);
    screenshot.depthTexture = woby::graphics::createTexture2D(
        screenshot.width,
        screenshot.height,
        false,
        1,
        woby::graphics::TextureFormat::D24S8,
        depthFlags);
    screenshot.readbackTexture = woby::graphics::createTexture2D(
        screenshot.width,
        screenshot.height,
        false,
        1,
        woby::graphics::TextureFormat::BGRA8,
        readbackFlags);
    if (!woby::graphics::isValid(screenshot.colorTexture)
        || !woby::graphics::isValid(screenshot.depthTexture)
        || !woby::graphics::isValid(screenshot.readbackTexture)) {
        destroySceneScreenshotFramebuffer(screenshot);
        throw std::runtime_error("Failed to create screenshot textures.");
    }

    const woby::graphics::TextureHandle textures[] = {
        screenshot.colorTexture,
        screenshot.depthTexture,
    };
    screenshot.frameBuffer = woby::graphics::createFrameBuffer(
        static_cast<uint8_t>(std::size(textures)),
        textures,
        false);
    if (!woby::graphics::isValid(screenshot.frameBuffer)) {
        destroySceneScreenshotFramebuffer(screenshot);
        throw std::runtime_error("Failed to create screenshot framebuffer.");
    }

    woby::graphics::setName(screenshot.frameBuffer, "Scene Screenshot Framebuffer");
    woby::graphics::setName(screenshot.colorTexture, "Scene Screenshot Color");
    woby::graphics::setName(screenshot.depthTexture, "Scene Screenshot Depth");
    woby::graphics::setName(screenshot.readbackTexture, "Scene Screenshot Readback");
    screenshot.pixels.resize(static_cast<size_t>(screenshot.width) * screenshot.height * 4u);
}

void writeSceneScreenshotPng(const SceneScreenshotRuntime& screenshot)
{
    const std::filesystem::path parentPath = screenshot.outputPath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }

    const auto utf8 = screenshot.outputPath.u8string();
    const std::string outputPath(utf8.begin(), utf8.end());
    SDL_Surface* surface = SDL_CreateSurfaceFrom(screenshot.width, screenshot.height,
        SDL_PIXELFORMAT_BGRA32, const_cast<uint8_t*>(screenshot.pixels.data()), screenshot.width * 4);
    if (!surface) throw std::runtime_error(std::string("Cannot create PNG surface: ") + SDL_GetError());
    const bool saved = SDL_SavePNG(surface, outputPath.c_str());
    const std::string error = saved ? "" : SDL_GetError();
    SDL_DestroySurface(surface);
    if (!saved) throw std::runtime_error("Failed to write screenshot PNG: " + outputPath + ": " + error);
}

} // namespace

void validateSceneScreenshotRenderer()
{
    if (woby::graphics::getRendererType() == woby::graphics::RendererType::Noop) {
        throw std::runtime_error("A graphics renderer is required for screenshots; graphics Noop is not supported.");
    }
    if ((woby::graphics::getCaps()->supported & WOBY_GPU_CAPS_TEXTURE_READ_BACK) == 0u) {
        throw std::runtime_error("Renderer does not support texture readback.");
    }
    if ((woby::graphics::getCaps()->supported & WOBY_GPU_CAPS_TEXTURE_BLIT) == 0u) {
        throw std::runtime_error("Renderer does not support texture blit for screenshot readback.");
    }
}

void destroySceneScreenshotFramebuffer(SceneScreenshotRuntime& screenshot)
{
    if (woby::graphics::isValid(screenshot.frameBuffer)) {
        woby::graphics::destroy(screenshot.frameBuffer);
    }
    if (woby::graphics::isValid(screenshot.depthTexture)) {
        woby::graphics::destroy(screenshot.depthTexture);
    }
    if (woby::graphics::isValid(screenshot.readbackTexture)) {
        woby::graphics::destroy(screenshot.readbackTexture);
    }
    if (woby::graphics::isValid(screenshot.colorTexture)) {
        woby::graphics::destroy(screenshot.colorTexture);
    }

    screenshot.frameBuffer = WOBY_GPU_INVALID_HANDLE;
    screenshot.depthTexture = WOBY_GPU_INVALID_HANDLE;
    screenshot.readbackTexture = WOBY_GPU_INVALID_HANDLE;
    screenshot.colorTexture = WOBY_GPU_INVALID_HANDLE;
    screenshot.pixels.clear();
}

void requestSceneScreenshotCapture(SceneScreenshotRuntime& screenshot, const std::filesystem::path& outputPath,
    ScreenshotSettings options)
{
    if (screenshot.captureRequested || screenshot.readbackPending) {
        throw std::runtime_error("A screenshot is already pending.");
    }

    screenshot.options = normalizedScreenshotSettings(options);
    screenshot.outputPath = pngPath(outputPath);
    screenshot.captureRequested = true;
}

void submitSceneScreenshotCapture(
    SceneScreenshotRuntime& screenshot,
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    float masterVertexPointSize,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle annotationProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    const UiState& ui,
    const woby::graphics::VertexLayout& helperLayout,
    const Bounds& sceneBounds,
    const SceneCamera& camera,
    bool homogeneousDepth,
    const ComparisonRuntimes* comparison)
{
    if (!screenshot.captureRequested) {
        return;
    }
    if (comparison != nullptr && !comparisonsReadyForScreenshot(ui, *comparison)) {
        return; // Keep the request pending until all visible results are ready.
    }

    const bool visibleResults = std::any_of(ui.comparisons.begin(), ui.comparisons.end(),
        [](const auto& item) { return item.settings.enabled; });
    if (screenshot.options.resultsOnly && !visibleResults) {
        throw std::runtime_error("No visible analysis results to export.");
    }
    if (visibleResults && comparison == nullptr) {
        throw std::runtime_error("Analysis results are unavailable for export.");
    }
    ensureSceneScreenshotFramebuffer(screenshot);
    const auto& options = screenshot.options;
    const bool annotations = visibleResults && (options.legend || options.comparisonName || options.sources
        || options.direction || options.tolerance);
    const auto panelWidth = annotations ? static_cast<uint16_t>(screenshot.width * .43f) : uint16_t{0};
    const auto sceneWidth = static_cast<uint16_t>(screenshot.width - panelWidth);
    const bool scaleOverlay = !options.resultsOnly && (ui.showGrid || ui.showDimensions);
    // Build and validate all annotations before submitting a readback. Never clip metadata silently.
    ImDrawList annotationDraw(ImGui::GetDrawListSharedData());
    annotationDraw._ResetForNewFrame();
    annotationDraw.PushClipRect({0, 0}, {static_cast<float>(screenshot.width), static_cast<float>(screenshot.height)});
    annotationDraw.PushTexture(ImGui::GetIO().Fonts->TexRef);
    if (annotations) {
        const float fontSize = 20.0f;
        const float x = static_cast<float>(sceneWidth) + 24;
        const float wrap = static_cast<float>(panelWidth) - 48;
        float y = 24;
        annotationDraw.AddRectFilled({static_cast<float>(sceneWidth), 0},
            {static_cast<float>(screenshot.width), static_cast<float>(screenshot.height)}, IM_COL32(24, 28, 34, 255));
        const auto line = [&](const std::string& text) {
            const auto size = ImGui::GetFont()->CalcTextSizeA(fontSize, 100000, wrap, text.c_str());
            if (y + size.y > static_cast<float>(screenshot.height) - 24) {
                throw std::runtime_error("Export annotations do not fit. Increase image height or export fewer visible analyses.");
            }
            const auto color = text.starts_with("SATURATED:") ? IM_COL32(255, 170, 65, 255) : IM_COL32(235, 239, 245, 255);
            annotationDraw.AddText(ImGui::GetFont(), fontSize, {x, y}, color,
                text.c_str(), nullptr, wrap);
            y += size.y + 8;
        };
        line(options.resultsOnly ? "Woby | Visible analysis results" : "Woby | Scene and visible results");
        for (const auto& item : ui.comparisons) {
            if (!item.settings.enabled) { continue; }
            y += 12;
            const auto a = comparisonInputSummary(ui, ComparisonSide::a, item.objectId);
            const auto b = comparisonInputSummary(ui, ComparisonSide::b, item.objectId);
            const auto settings = effectiveComparisonSettings(ui, item.objectId);
            for (const auto& text : comparisonReportLines(item.name, a.enabledPartCount == 0 ? "" : a.sourceNames,
                     b.enabledPartCount == 0 ? "" : b.sourceNames, settings,
                     comparison->objects.at(item.objectId).result, options)) { line(text); }
            if (options.legend && (settings.mode == ComparisonMode::distance || settings.mode == ComparisonMode::surfaceQuality)) {
                const float used = settings.mode == ComparisonMode::surfaceQuality ?
                    drawSurfaceQualityLegend(annotationDraw, {x, y}, wrap, fontSize, settings.quality.metric,
                        comparison->objects.at(item.objectId).result.qualityDistributions.at(static_cast<size_t>(settings.quality.metric))) :
                    drawComparisonLegend(annotationDraw, {x, y}, wrap, fontSize, settings);
                y += used;
                if (y > static_cast<float>(screenshot.height) - 24) {
                    throw std::runtime_error("Export legends do not fit. Increase image height or export fewer visible analyses.");
                }
            }
        }
    }
    if (scaleOverlay) {
        auto parts = scenePickParts(ui);
        if (comparison) { appendVisibleComparisonPickParts(parts, ui, *comparison); }
        const auto dimensions = ui.showDimensions ? sceneDimensions(parts) : std::nullopt;
        const auto pickView = scenePickView(camera, ui.upAxis, sceneBounds,
            sceneWidth, screenshot.height, homogeneousDepth, 1);
        drawSceneScaleOverlay(annotationDraw, ui, dimensions, pickView, {0, 0}, 1, 20);
    }
    annotationDraw.PopTexture();
    annotationDraw.PopClipRect();

    woby::graphics::setViewName(screenshotSceneView, "Scene Screenshot");
    woby::graphics::setViewName(screenshotHelperView, "Scene Screenshot Helpers");
    woby::graphics::setViewName(screenshotReadbackView, "Scene Screenshot Readback");
    woby::graphics::setViewFrameBuffer(screenshotSceneView, screenshot.frameBuffer);
    woby::graphics::setViewFrameBuffer(screenshotHelperView, screenshot.frameBuffer);
    woby::graphics::setViewClear(screenshotSceneView, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 0x20242aff, 1.0f, 0);
    woby::graphics::setViewClear(screenshotHelperView, WOBY_GPU_CLEAR_NONE, 0x00000000, 1.0f, 0);
    woby::graphics::setViewRect(screenshotSceneView, 0, 0, sceneWidth, screenshot.height);
    woby::graphics::setViewRect(screenshotHelperView, 0, 0, sceneWidth, screenshot.height);
    woby::graphics::touch(screenshotSceneView);
    woby::graphics::touch(screenshotHelperView);

    const auto captureView = scenePickView(camera, ui.upAxis, sceneBounds,
        sceneWidth, screenshot.height, homogeneousDepth, 1);
    const auto* view = captureView.view.data();
    const auto* projection = captureView.renderProjection.data();
    woby::graphics::setViewTransform(screenshotSceneView, view, projection, true);
    woby::graphics::setViewTransform(screenshotHelperView, view, projection, true);

    woby::graphics::setViewMode(screenshotSceneView, woby::graphics::ViewMode::Sequential);
    if (!options.resultsOnly) {
        submitSceneFiles(
            screenshotSceneView,
            files,
            ui.sceneNodes,
            runtimes,
            masterVertexPointSize,
            meshProgram,
            uvGridUniform,
            colorProgram,
            pointSpriteProgram,
            colorUniform,
            pointParamsUniform,
            sceneWidth,
            screenshot.height);
    }
    if (comparison != nullptr) { submitComparisonScenes(screenshotSceneView, ui, *comparison, colorProgram, colorUniform, screenshot.renderScratch); }
    if (!options.resultsOnly) {
        submitSceneHelpers(screenshotHelperView, ui, helperLayout, colorProgram, colorUniform);
        submitSceneAnnotations(screenshotHelperView, ui, captureView,
            helperLayout, annotationProgram, colorUniform, screenshot.renderScratch);
    }
    if (annotations || scaleOverlay) {
        // Export runs before ImGui::Render/EndFrame refreshes PlatformIO.Textures.
        // Annotation text can grow the font atlas during this frame, so use the
        // textures actually referenced by this draw list, including new atlases.
        ImVector<ImTextureData*> textures;
        for (const auto& command : annotationDraw.CmdBuffer) {
            auto* texture = command.TexRef._TexData;
            if (texture != nullptr && !textures.contains(texture)) { textures.push_back(texture); }
        }
        ImDrawData drawData;
        drawData.Valid = true;
        drawData.DisplayPos = {0, 0};
        drawData.DisplaySize = {static_cast<float>(screenshot.width), static_cast<float>(screenshot.height)};
        drawData.FramebufferScale = {1, 1};
        drawData.Textures = &textures;
        drawData.AddDrawList(&annotationDraw);
        woby::graphics::setViewFrameBuffer(screenshotAnnotationView, screenshot.frameBuffer);
        woby::graphics::setViewClear(screenshotAnnotationView, WOBY_GPU_CLEAR_NONE);
        imgui_graphics::renderToView(&drawData, screenshotAnnotationView);
    }

    woby::graphics::blit(
        screenshotReadbackView,
        screenshot.readbackTexture,
        0,
        0,
        screenshot.colorTexture,
        0,
        0,
        screenshot.width,
        screenshot.height);
    screenshot.readFrame = woby::graphics::readTexture(screenshot.readbackTexture, screenshot.pixels.data());
    screenshot.captureRequested = false;
    screenshot.readbackPending = true;
}

bool drawSceneScreenshotOptions(UiState& state)
{
    bool save = false;
    bool open = true;
    ImGui::SetNextWindowSize({uiSize(440.0f), 0}, ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Export PNG", &open,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoMove)) {
        const bool editing = ImGui::IsAnyItemActive();
        auto options = state.screenshotSettings;
        const auto initial = options;
        ImGui::TextUnformatted("Image");
        ImGui::SameLine();
        drawInformationIcon("export_info", "Export PNG",
            "Choose a width from 960 to 7680 pixels and a height from 720 to 4320 pixels.\n\n"
            "Visible results only exports analysis results. Otherwise the image includes the scene, helpers and visible results. "
            "Uses the current camera.\n\nExport waits for complete visible results.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(uiSize(140.0f));
        ImGui::InputInt("Width (px)", &options.width, 0);
        ImGui::SetNextItemWidth(uiSize(140.0f));
        ImGui::InputInt("Height (px)", &options.height, 0);
        ImGui::Checkbox("Visible results only", &options.resultsOnly);
        ImGui::Separator();
        ImGui::TextUnformatted("Analysis annotations");
        ImGui::SameLine();
        drawInformationIcon("annotations_info", "Analysis annotations", "Distance legends always include tolerance.");
        drawVisibilityField("Numeric legend and statistics", options.legend);
        drawVisibilityField("Analysis name", options.comparisonName);
        drawVisibilityField("A / B sources", options.sources);
        drawVisibilityField("Measurement direction", options.direction);
        ImGui::BeginDisabled(options.legend);
        drawVisibilityField("Tolerance", options.tolerance);
        ImGui::EndDisabled();
        if (options != initial) { setScreenshotSettings(state, options); }
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Save PNG...")) { save = true; ImGui::CloseCurrentPopup(); }
        setLastItemTooltip("Choose where to save a PNG screenshot with these display options.");
        ImGui::SameLine();
        const bool escape = !editing && !ImGui::IsAnyItemActive()
            && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)
            && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ImGui::Button("Close", ImVec2(uiSize(80.0f), 0.0f)) || escape) {
            ImGui::CloseCurrentPopup();
        }
        setLastItemTooltip("Close and return to the scene (Esc).");
        ImGui::EndPopup();
    }
    return save;
}

void failSceneScreenshotCapture(SceneScreenshotRuntime& screenshot)
{
    screenshot.captureRequested = false;
    screenshot.readbackPending = false;
}

std::optional<std::string> completeSceneScreenshotReadback(
    SceneScreenshotRuntime& screenshot,
    uint32_t frameNumber)
{
    if (!screenshot.readbackPending || frameNumber < screenshot.readFrame) {
        return {};
    }

    writeSceneScreenshotPng(screenshot);
    screenshot.readbackPending = false;
    return "Saved screenshot " + fileDisplayName(screenshot.outputPath);
}

} // namespace woby
