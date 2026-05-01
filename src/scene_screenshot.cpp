#include "scene_screenshot.h"

#include <bimg/bimg.h>
#include <bx/file.h>
#include <bx/math.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace woby {
namespace {

constexpr bgfx::ViewId screenshotSceneView = 2;
constexpr bgfx::ViewId screenshotHelperView = 3;
constexpr bgfx::ViewId screenshotReadbackView = 4;
constexpr uint16_t screenshotWidth = 1920;
constexpr uint16_t screenshotHeight = 1800;

std::string fileDisplayName(const std::filesystem::path& path)
{
    const auto filename = path.filename().string();
    if (!filename.empty()) {
        return filename;
    }
    return path.string();
}

std::filesystem::path pngPath(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
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
    if (bgfx::isValid(screenshot.frameBuffer)) {
        return;
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
        screenshotWidth,
        screenshotHeight,
        false,
        1,
        bgfx::TextureFormat::BGRA8,
        colorFlags);
    screenshot.depthTexture = bgfx::createTexture2D(
        screenshotWidth,
        screenshotHeight,
        false,
        1,
        bgfx::TextureFormat::D24S8,
        depthFlags);
    screenshot.readbackTexture = bgfx::createTexture2D(
        screenshotWidth,
        screenshotHeight,
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
    screenshot.pixels.resize(static_cast<size_t>(screenshotWidth) * screenshotHeight * 4u);
}

void writeSceneScreenshotPng(const SceneScreenshotRuntime& screenshot)
{
    const std::filesystem::path parentPath = screenshot.outputPath.parent_path();
    if (!parentPath.empty()) {
        std::filesystem::create_directories(parentPath);
    }

    bx::FileWriter writer;
    bx::Error error;
    const std::string outputPath = screenshot.outputPath.string();
    if (!writer.open(bx::FilePath(outputPath.c_str()), false, &error)) {
        throw std::runtime_error("Failed to open screenshot file: " + outputPath);
    }

    const bool yflip = bgfx::getCaps()->originBottomLeft;
    const int32_t result = bimg::imageWritePng(
        &writer,
        screenshotWidth,
        screenshotHeight,
        screenshotWidth * 4u,
        screenshot.pixels.data(),
        bimg::TextureFormat::BGRA8,
        yflip,
        &error);
    writer.close();
    if (result <= 0 || !error.isOk()) {
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

void requestSceneScreenshotCapture(SceneScreenshotRuntime& screenshot, const std::filesystem::path& outputPath)
{
    if (screenshot.captureRequested || screenshot.readbackPending) {
        throw std::runtime_error("A screenshot is already pending.");
    }

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
    bool homogeneousDepth)
{
    if (!screenshot.captureRequested) {
        return;
    }

    ensureSceneScreenshotFramebuffer(screenshot);

    bgfx::setViewName(screenshotSceneView, "Scene Screenshot");
    bgfx::setViewName(screenshotHelperView, "Scene Screenshot Helpers");
    bgfx::setViewName(screenshotReadbackView, "Scene Screenshot Readback");
    bgfx::setViewFrameBuffer(screenshotSceneView, screenshot.frameBuffer);
    bgfx::setViewFrameBuffer(screenshotHelperView, screenshot.frameBuffer);
    bgfx::setViewClear(screenshotSceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x20242aff, 1.0f, 0);
    bgfx::setViewClear(screenshotHelperView, BGFX_CLEAR_NONE, 0x00000000, 1.0f, 0);
    bgfx::setViewRect(screenshotSceneView, 0, 0, screenshotWidth, screenshotHeight);
    bgfx::setViewRect(screenshotHelperView, 0, 0, screenshotWidth, screenshotHeight);
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
        camera.verticalFovDegrees,
        static_cast<float>(screenshotWidth) / static_cast<float>(screenshotHeight),
        camera.nearPlane,
        cameraFarPlane(camera, sceneBounds),
        homogeneousDepth);
    bgfx::setViewTransform(screenshotSceneView, view, projection);
    bgfx::setViewTransform(screenshotHelperView, view, projection);

    submitSceneFiles(
        screenshotSceneView,
        files,
        runtimes,
        masterVertexPointSize,
        meshProgram,
        colorProgram,
        pointSpriteProgram,
        colorUniform,
        pointParamsUniform,
        screenshotWidth,
        screenshotHeight);
    submitSceneHelpers(screenshotHelperView, ui, helperLayout, colorProgram, colorUniform);

    bgfx::blit(
        screenshotReadbackView,
        screenshot.readbackTexture,
        0,
        0,
        screenshot.colorTexture,
        0,
        0,
        screenshotWidth,
        screenshotHeight);
    screenshot.readFrame = bgfx::readTexture(screenshot.readbackTexture, screenshot.pixels.data());
    screenshot.captureRequested = false;
    screenshot.readbackPending = true;
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
