#pragma once

#include "hover_pick.h"
#include "marker_pick_logic.h"
#include "scene_viewport.h"

namespace woby {

// Explicit draw context keeps viewport picking out of screenshot submissions.
struct MarkerDrawContext {
    MarkerDrawList list;
    woby::graphics::UniformHandle baseUniform = WOBY_GPU_INVALID_HANDLE;
};

struct MarkerReadback {
    bool pending = false, requested = false;
    uint32_t issuedFrame = 0, readyFrame = 0;
    uint64_t epoch = 0, sequence = 0;
    MarkerPixel pixel{};
    std::vector<MarkerDraw> draws;
    woby::graphics::TextureHandle staging = WOBY_GPU_INVALID_HANDLE;
};

struct GpuMarkerPicker {
    bool initialized = false, unavailable = false, active = false;
    uint16_t width = 0, height = 0;
    uint32_t frame = 0;
    int samples = 4;
    uint64_t epoch = 0, sequence = 0, latestSequence = 0;
    uint64_t sceneGeneration = 0, sceneRevision = 0;
    const UiState* owner = nullptr;
    uint64_t resourceSignature = 0;
    SceneCamera camera;
    SceneUpAxis upAxis = SceneUpAxis::z;
    std::array<float, 4> query{}, options{};
    MarkerDrawContext context;
    std::array<MarkerReadback, 8> requests;
    std::optional<HoveredVertex> coordinates;
    woby::graphics::TextureHandle color = WOBY_GPU_INVALID_HANDLE, ids = WOBY_GPU_INVALID_HANDLE,
        depth = WOBY_GPU_INVALID_HANDLE, result = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::FrameBufferHandle framebuffer = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::ProgramHandle point = WOBY_GPU_INVALID_HANDLE, mesh = WOBY_GPU_INVALID_HANDLE,
        line = WOBY_GPU_INVALID_HANDLE, lineSprite = WOBY_GPU_INVALID_HANDLE,
        comparison = WOBY_GPU_INVALID_HANDLE, composite = WOBY_GPU_INVALID_HANDLE;
    std::array<woby::graphics::ProgramHandle, 2> lookup = {{{woby::graphics::kInvalidHandle}, {woby::graphics::kInvalidHandle}}},
        highlight = {{{woby::graphics::kInvalidHandle}, {woby::graphics::kInvalidHandle}}};
    woby::graphics::UniformHandle queryUniform = WOBY_GPU_INVALID_HANDLE, optionsUniform = WOBY_GPU_INVALID_HANDLE,
        colorSampler = WOBY_GPU_INVALID_HANDLE, idSampler = WOBY_GPU_INVALID_HANDLE, resultSampler = WOBY_GPU_INVALID_HANDLE;
};

[[nodiscard]] bool supportsGpuMarkerPicking(const woby::graphics::Caps& caps);
// Call every viewport frame, including disabled frames, to invalidate old results.
[[nodiscard]] bool beginGpuMarkerPicking(GpuMarkerPicker& picker, const std::filesystem::path& assets,
    const UiState& state, const SceneViewport& viewport, MousePosition mouse, bool enabled, int samples = 4,
    uint64_t resourceSignature = 0);
void submitGpuMarkerPicking(GpuMarkerPicker& picker, const SceneViewport& viewport);
void pollGpuMarkerPicking(GpuMarkerPicker& picker, uint32_t frame, const UiState& state,
    const std::vector<LoadedModelRuntime>& runtimes, uint64_t resourceSignature = 0);
// Flush readbacks before their CPU storage is released. Call before woby::graphics::shutdown.
void destroyGpuMarkerPicker(GpuMarkerPicker& picker);
[[nodiscard]] std::optional<HoveredVertex> resolveMarkerCoordinates(uint32_t id,
    std::span<const MarkerDraw> draws, const UiState& state, const std::vector<LoadedModelRuntime>& runtimes);
void setMarkerRenderState(uint64_t state, bool markerIds);

} // namespace woby
