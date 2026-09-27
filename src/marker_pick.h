#pragma once

#include "hover_pick.h"
#include "marker_pick_logic.h"
#include "scene_viewport.h"

namespace woby {

// Explicit draw context keeps viewport picking out of screenshot submissions.
struct MarkerDrawContext {
    MarkerDrawList list;
    bgfx::UniformHandle baseUniform = BGFX_INVALID_HANDLE;
};

struct MarkerReadback {
    bool pending = false, requested = false;
    uint32_t issuedFrame = 0, readyFrame = 0;
    uint64_t epoch = 0, sequence = 0;
    MarkerPixel pixel{};
    std::vector<MarkerDraw> draws;
    bgfx::TextureHandle staging = BGFX_INVALID_HANDLE;
};

struct GpuMarkerPicker {
    bool initialized = false, unavailable = false, active = false;
    uint16_t width = 0, height = 0;
    uint32_t frame = 0;
    int samples = 4;
    uint64_t epoch = 0, sequence = 0, latestSequence = 0;
    uint64_t sceneGeneration = 0, sceneRevision = 0;
    std::array<float, 4> query{}, options{};
    MarkerDrawContext context;
    std::array<MarkerReadback, 8> requests;
    std::optional<HoveredVertex> coordinates;
    bgfx::TextureHandle color = BGFX_INVALID_HANDLE, ids = BGFX_INVALID_HANDLE,
        depth = BGFX_INVALID_HANDLE, result = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle point = BGFX_INVALID_HANDLE, mesh = BGFX_INVALID_HANDLE,
        line = BGFX_INVALID_HANDLE, comparison = BGFX_INVALID_HANDLE, composite = BGFX_INVALID_HANDLE;
    std::array<bgfx::ProgramHandle, 2> lookup = {{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}},
        highlight = {{{bgfx::kInvalidHandle}, {bgfx::kInvalidHandle}}};
    bgfx::UniformHandle queryUniform = BGFX_INVALID_HANDLE, optionsUniform = BGFX_INVALID_HANDLE,
        colorSampler = BGFX_INVALID_HANDLE, idSampler = BGFX_INVALID_HANDLE, resultSampler = BGFX_INVALID_HANDLE;
};

[[nodiscard]] bool supportsGpuMarkerPicking(const bgfx::Caps& caps);
// Call every viewport frame, including disabled frames, to invalidate old results.
[[nodiscard]] bool beginGpuMarkerPicking(GpuMarkerPicker& picker, const std::filesystem::path& assets,
    const UiState& state, const SceneViewport& viewport, MousePosition mouse, bool enabled, int samples = 4);
void submitGpuMarkerPicking(GpuMarkerPicker& picker, const SceneViewport& viewport);
void pollGpuMarkerPicking(GpuMarkerPicker& picker, uint32_t frame, const UiState& state,
    const std::vector<LoadedModelRuntime>& runtimes);
// Flush readbacks before their CPU storage is released. Call before bgfx::shutdown.
void destroyGpuMarkerPicker(GpuMarkerPicker& picker);
[[nodiscard]] std::optional<HoveredVertex> resolveMarkerCoordinates(uint32_t id,
    std::span<const MarkerDraw> draws, const UiState& state, const std::vector<LoadedModelRuntime>& runtimes);
void setMarkerRenderState(uint64_t state, bool markerIds);

} // namespace woby
