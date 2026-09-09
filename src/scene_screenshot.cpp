#include "scene_screenshot.h"
#include "comparison_view.h"
#include "comparison_scene.h"
#include "comparison_legend.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "imgui_bgfx.h"

#include <bimg/bimg.h>
#include <bx/allocator.h>
#include <bx/math.h>
#include <bx/readerwriter.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

namespace woby {
namespace {

constexpr bgfx::ViewId screenshotSceneView = 3;
constexpr bgfx::ViewId screenshotHelperView = 4;
constexpr bgfx::ViewId screenshotAnnotationView = 5;
constexpr bgfx::ViewId screenshotReadbackView = 6;

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
    if (bgfx::isValid(screenshot.frameBuffer)
        && screenshot.width == screenshot.options.width && screenshot.height == screenshot.options.height) {
        return;
    }
    destroySceneScreenshotFramebuffer(screenshot);
    screenshot.width = static_cast<uint16_t>(screenshot.options.width);
    screenshot.height = static_cast<uint16_t>(screenshot.options.height);
    if (screenshot.width > bgfx::getCaps()->limits.maxTextureSize
        || screenshot.height > bgfx::getCaps()->limits.maxTextureSize) {
        throw std::runtime_error("Export resolution exceeds the renderer texture limit.");
    }

    if ((bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_READ_BACK) == 0u) {
        throw std::runtime_error("Renderer does not support texture readback.");
    }

    if ((bgfx::getCaps()->supported & BGFX_CAPS_TEXTURE_BLIT) == 0u) {
        throw std::runtime_error("Renderer does not support texture blit for screenshot readback.");
    }

    constexpr uint64_t colorFlags = BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP
        | BGFX_SAMPLER_V_CLAMP;
    constexpr uint64_t depthFlags = BGFX_TEXTURE_RT
        | BGFX_SAMPLER_U_CLAMP
        | BGFX_SAMPLER_V_CLAMP;
    constexpr uint64_t readbackFlags = BGFX_TEXTURE_BLIT_DST
        | BGFX_TEXTURE_READ_BACK
        | BGFX_SAMPLER_U_CLAMP
        | BGFX_SAMPLER_V_CLAMP;
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::BGRA8, colorFlags)) {
        throw std::runtime_error("Renderer cannot create screenshot color target.");
    }
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::D24S8, depthFlags)) {
        throw std::runtime_error("Renderer cannot create screenshot depth target.");
    }
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::BGRA8, readbackFlags)) {
        throw std::runtime_error("Renderer cannot create screenshot readback target.");
    }

    screenshot.colorTexture = bgfx::createTexture2D(
        screenshot.width,
        screenshot.height,
        false,
        1,
        bgfx::TextureFormat::BGRA8,
        colorFlags);
    screenshot.depthTexture = bgfx::createTexture2D(
        screenshot.width,
        screenshot.height,
        false,
        1,
        bgfx::TextureFormat::D24S8,
        depthFlags);
    screenshot.readbackTexture = bgfx::createTexture2D(
        screenshot.width,
        screenshot.height,
        false,
        1,
        bgfx::TextureFormat::BGRA8,
        readbackFlags);
    if (!bgfx::isValid(screenshot.colorTexture)
        || !bgfx::isValid(screenshot.depthTexture)
        || !bgfx::isValid(screenshot.readbackTexture)) {
        destroySceneScreenshotFramebuffer(screenshot);
        throw std::runtime_error("Failed to create screenshot textures.");
    }

    const bgfx::TextureHandle textures[] = {
        screenshot.colorTexture,
        screenshot.depthTexture,
    };
    screenshot.frameBuffer = bgfx::createFrameBuffer(
        static_cast<uint8_t>(std::size(textures)),
        textures,
        false);
    if (!bgfx::isValid(screenshot.frameBuffer)) {
        destroySceneScreenshotFramebuffer(screenshot);
        throw std::runtime_error("Failed to create screenshot framebuffer.");
    }

    bgfx::setName(screenshot.frameBuffer, "Scene Screenshot Framebuffer");
    bgfx::setName(screenshot.colorTexture, "Scene Screenshot Color");
    bgfx::setName(screenshot.depthTexture, "Scene Screenshot Depth");
    bgfx::setName(screenshot.readbackTexture, "Scene Screenshot Readback");
    screenshot.pixels.resize(static_cast<size_t>(screenshot.width) * screenshot.height * 4u);
}

void writeSceneScreenshotPng(const SceneScreenshotRuntime& screenshot)
{
    const std::filesystem::path parentPath = screenshot.outputPath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }

    // Encode to memory, then use filesystem-aware output so Windows Unicode paths work.
    // RGBA lets bimg write whole rows instead of making a writer call for every channel.
    auto rgbaPixels = screenshot.pixels;
    for (size_t index = 0; index < rgbaPixels.size(); index += 4u) {
        std::swap(rgbaPixels[index], rgbaPixels[index + 2u]);
    }
    bx::DefaultAllocator allocator;
    bx::MemoryBlock block(&allocator);
    block.more(static_cast<uint32_t>(rgbaPixels.size()) + screenshot.height * 16u + 1024u);
    bx::MemoryWriter writer(&block);
    bx::Error error;
    const auto outputPathUtf8 = screenshot.outputPath.u8string();
    const std::string outputPath(outputPathUtf8.begin(), outputPathUtf8.end());

    const bool yflip = bgfx::getCaps()->originBottomLeft;
    const int32_t result = bimg::imageWritePng(
        &writer,
        screenshot.width,
        screenshot.height,
        screenshot.width * 4u,
        rgbaPixels.data(),
        bimg::TextureFormat::RGBA8,
        yflip,
        &error);
    if (result <= 0 || !error.isOk()) {
        throw std::runtime_error("Failed to encode screenshot PNG: " + outputPath);
    }
    std::ofstream output(screenshot.outputPath, std::ios::binary | std::ios::trunc);
    output.write(static_cast<const char*>(block.more()), static_cast<std::streamsize>(writer.seek()));
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write screenshot PNG: " + outputPath);
    }
}

} // namespace

void destroySceneScreenshotFramebuffer(SceneScreenshotRuntime& screenshot)
{
    if (bgfx::isValid(screenshot.frameBuffer)) {
        bgfx::destroy(screenshot.frameBuffer);
    }
    if (bgfx::isValid(screenshot.depthTexture)) {
        bgfx::destroy(screenshot.depthTexture);
    }
    if (bgfx::isValid(screenshot.readbackTexture)) {
        bgfx::destroy(screenshot.readbackTexture);
    }
    if (bgfx::isValid(screenshot.colorTexture)) {
        bgfx::destroy(screenshot.colorTexture);
    }

    screenshot.frameBuffer = BGFX_INVALID_HANDLE;
    screenshot.depthTexture = BGFX_INVALID_HANDLE;
    screenshot.readbackTexture = BGFX_INVALID_HANDLE;
    screenshot.colorTexture = BGFX_INVALID_HANDLE;
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
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
    const UiState& ui,
    const bgfx::VertexLayout& helperLayout,
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
        throw std::runtime_error("No visible comparison results to export.");
    }
    if (visibleResults && comparison == nullptr) {
        throw std::runtime_error("Comparison results are unavailable for export.");
    }
    ensureSceneScreenshotFramebuffer(screenshot);
    const auto& options = screenshot.options;
    const bool annotations = visibleResults && (options.legend || options.comparisonName || options.sources
        || options.direction || options.tolerance);
    const auto panelWidth = annotations ? static_cast<uint16_t>(screenshot.width * .43f) : uint16_t{0};
    const auto sceneWidth = static_cast<uint16_t>(screenshot.width - panelWidth);
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
                throw std::runtime_error("Export annotations do not fit. Increase image height or export fewer visible comparisons.");
            }
            const auto color = text.starts_with("SATURATED:") ? IM_COL32(255, 170, 65, 255) : IM_COL32(235, 239, 245, 255);
            annotationDraw.AddText(ImGui::GetFont(), fontSize, {x, y}, color,
                text.c_str(), nullptr, wrap);
            y += size.y + 8;
        };
        line(options.resultsOnly ? "Woby | Visible comparison results" : "Woby | Scene and visible results");
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
                    throw std::runtime_error("Export legends do not fit. Increase image height or export fewer visible comparisons.");
                }
            }
        }
    }
    annotationDraw.PopTexture();
    annotationDraw.PopClipRect();

    bgfx::setViewName(screenshotSceneView, "Scene Screenshot");
    bgfx::setViewName(screenshotHelperView, "Scene Screenshot Helpers");
    bgfx::setViewName(screenshotReadbackView, "Scene Screenshot Readback");
    bgfx::setViewFrameBuffer(screenshotSceneView, screenshot.frameBuffer);
    bgfx::setViewFrameBuffer(screenshotHelperView, screenshot.frameBuffer);
    bgfx::setViewClear(screenshotSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20242aff, 1.0f, 0);
    bgfx::setViewClear(screenshotHelperView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    bgfx::setViewRect(screenshotSceneView, 0, 0, sceneWidth, screenshot.height);
    bgfx::setViewRect(screenshotHelperView, 0, 0, sceneWidth, screenshot.height);
    bgfx::touch(screenshotSceneView);
    bgfx::touch(screenshotHelperView);

    float view[16];
    float projection[16];
    bx::mtxLookAt(
        view,
        cameraEye(camera, ui.upAxis),
        cameraLookAt(camera),
        cameraUp(camera, ui.upAxis));
    bx::mtxProj(
        projection,
        cameraViewportFov(camera, static_cast<float>(sceneWidth) / static_cast<float>(screenshot.height)),
        static_cast<float>(sceneWidth) / static_cast<float>(screenshot.height),
        camera.nearPlane,
        cameraFarPlane(camera, sceneBounds),
        homogeneousDepth);
    bgfx::setViewTransform(screenshotSceneView, view, projection);
    bgfx::setViewTransform(screenshotHelperView, view, projection);

    bgfx::setViewMode(screenshotSceneView, bgfx::ViewMode::Sequential);
    if (!options.resultsOnly) {
        submitSceneFiles(
            screenshotSceneView,
            files,
            ui.sceneNodes,
            runtimes,
            masterVertexPointSize,
            meshProgram,
            colorProgram,
            pointSpriteProgram,
            colorUniform,
            pointParamsUniform,
            sceneWidth,
            screenshot.height);
    }
    if (comparison != nullptr) { submitComparisonScenes(screenshotSceneView, ui, *comparison, colorProgram, colorUniform); }
    if (!options.resultsOnly) {
        submitSceneHelpers(screenshotHelperView, ui, helperLayout, colorProgram, colorUniform);
    }
    if (annotations) {
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
        bgfx::setViewFrameBuffer(screenshotAnnotationView, screenshot.frameBuffer);
        bgfx::setViewClear(screenshotAnnotationView, BGFX_CLEAR_NONE);
        imgui_bgfx::renderToView(&drawData, screenshotAnnotationView);
    }

    bgfx::blit(
        screenshotReadbackView,
        screenshot.readbackTexture,
        0,
        0,
        screenshot.colorTexture,
        0,
        0,
        screenshot.width,
        screenshot.height);
    screenshot.readFrame = bgfx::readTexture(screenshot.readbackTexture, screenshot.pixels.data());
    screenshot.captureRequested = false;
    screenshot.readbackPending = true;
}

bool drawSceneScreenshotOptions(UiState& state)
{
    bool save = false;
    ImGui::SetNextWindowSize({440, 0}, ImGuiCond_Always);
    if (ImGui::BeginPopup("Export PNG")) {
        auto options = state.screenshotSettings;
        const auto initial = options;
        ImGui::TextUnformatted("Export PNG");
        ImGui::SameLine();
        drawInformationIcon("export_info", "Export PNG",
            "Choose a width from 960 to 7680 pixels and a height from 720 to 4320 pixels.\n\n"
            "Visible results only exports comparison results. Otherwise the image includes the scene, helpers and visible results. "
            "Uses the current camera.\n\nExport waits for complete visible results.");
        ImGui::Separator();
        ImGui::SetNextItemWidth(140);
        ImGui::InputInt("Width (px)", &options.width, 0);
        ImGui::SetNextItemWidth(140);
        ImGui::InputInt("Height (px)", &options.height, 0);
        ImGui::Checkbox("Visible results only", &options.resultsOnly);
        ImGui::Separator();
        ImGui::TextUnformatted("Comparison annotations");
        ImGui::SameLine();
        drawInformationIcon("annotations_info", "Comparison annotations", "Distance legends always include tolerance.");
        drawVisibilityField("Numeric legend and statistics", options.legend);
        drawVisibilityField("Comparison name", options.comparisonName);
        drawVisibilityField("A / B sources", options.sources);
        drawVisibilityField("Measurement direction", options.direction);
        ImGui::BeginDisabled(options.legend);
        drawVisibilityField("Tolerance", options.tolerance);
        ImGui::EndDisabled();
        if (options != initial) { setScreenshotSettings(state, options); }
        if (ImGui::Button("Save PNG...")) { save = true; ImGui::CloseCurrentPopup(); }
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
