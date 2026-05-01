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

struct SceneScreenshotRuntime {
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
void requestSceneScreenshotCapture(SceneScreenshotRuntime& screenshot, const std::filesystem::path& outputPath);
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
    bool homogeneousDepth);
void failSceneScreenshotCapture(SceneScreenshotRuntime& screenshot);
[[nodiscard]] std::optional<std::string> completeSceneScreenshotReadback(
    SceneScreenshotRuntime& screenshot,
    uint32_t frameNumber);

} // namespace woby
