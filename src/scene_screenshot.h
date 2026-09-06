#pragma once

#include "camera.h"
#include "scene_renderer.h"
#include "ui_state.h"

#include <bgfx/bgfx.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace woby {

struct ComparisonRuntimes;

struct SceneScreenshotRuntime {
    ScreenshotSettings options;
    uint16_t width = 0;
    uint16_t height = 0;
    bgfx::FrameBufferHandle frameBuffer = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle colorTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle depthTexture = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readbackTexture = BGFX_INVALID_HANDLE;
    std::vector<uint8_t> pixels;
    std::filesystem::path outputPath;
    uint32_t readFrame = 0;
    bool captureRequested = false;
    bool readbackPending = false;
};

void destroySceneScreenshotFramebuffer(SceneScreenshotRuntime& screenshot);
void requestSceneScreenshotCapture(SceneScreenshotRuntime& screenshot, const std::filesystem::path& outputPath,
    ScreenshotSettings options = {});
// Returns true when the user chooses Save PNG in the export options popup.
bool drawSceneScreenshotOptions(UiState& state);
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
    const ComparisonRuntimes* comparison = nullptr);
void failSceneScreenshotCapture(SceneScreenshotRuntime& screenshot);
[[nodiscard]] std::optional<std::string> completeSceneScreenshotReadback(
    SceneScreenshotRuntime& screenshot,
    uint32_t frameNumber);

} // namespace woby
