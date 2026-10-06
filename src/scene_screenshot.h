#pragma once

#include "camera.h"
#include "scene_renderer.h"
#include "ui_state.h"

#include "graphics.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace woby {

struct ComparisonRuntimes;

struct SceneScreenshotRuntime {
    SceneRenderScratch renderScratch;
    ScreenshotSettings options;
    uint16_t width = 0;
    uint16_t height = 0;
    woby::graphics::FrameBufferHandle frameBuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::TextureHandle colorTexture = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::TextureHandle depthTexture = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::TextureHandle readbackTexture = WOBY_GPU_INVALID_HANDLE;
    std::vector<uint8_t> pixels;
    std::filesystem::path outputPath;
    uint32_t readFrame = 0;
    bool captureRequested = false;
    bool readbackPending = false;
};

// Fail before advertising a headless instance as ready when capture is unavailable.
void validateSceneScreenshotRenderer();
void destroySceneScreenshotFramebuffer(SceneScreenshotRuntime& screenshot);
void requestSceneScreenshotCapture(SceneScreenshotRuntime& screenshot, const std::filesystem::path& outputPath,
    ScreenshotSettings options = {});
// Returns true when the user chooses Save PNG in the export options dialog.
bool drawSceneScreenshotOptions(UiState& state);
void submitSceneScreenshotCapture(
    SceneScreenshotRuntime& screenshot,
    const std::vector<LoadedModelRuntime>& runtimes,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle annotationProgram,
    woby::graphics::ProgramHandle lineSpriteProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    const TriangleEdgePrograms& triangleEdgePrograms,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    const UiState& ui,
    const woby::graphics::VertexLayout& helperLayout,
    const Bounds& sceneBounds,
    const SceneCamera& camera,
    bool homogeneousDepth,
    const ComparisonRuntimes* comparison = nullptr,
    const TransparentSurfacePrograms* transparency = nullptr);
void failSceneScreenshotCapture(SceneScreenshotRuntime& screenshot);
[[nodiscard]] std::optional<std::string> completeSceneScreenshotReadback(
    SceneScreenshotRuntime& screenshot,
    uint32_t frameNumber);

} // namespace woby
