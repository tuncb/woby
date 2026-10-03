#pragma once
#include "gpu_runtime.h"
#include <imgui.h>
#include <optional>

namespace mesh_lab {
inline constexpr const char* sampleFiles[] = {"uv-seam.obj", "shared-quad.obj", "hard-cube.obj", "large-coordinates.obj", "concave-polygon.obj"};
inline constexpr const char* sampleNames[] = {"UV seam", "Shared quad", "Hard cube", "Large coordinates", "Concave polygon"};
struct UiRuntime {
    ImFont* regular = nullptr;
    ImFont* mono = nullptr;
    ImFont* title = nullptr;
    char pathInput[2048]{};
    std::optional<std::filesystem::path> requestedPath;
    std::string error;
    bool loading = false;
    const Trace* lastTrace = nullptr;
    Stage lastStage = Stage::vertices;
    size_t lastTriangle = 0, lastCorner = 0;
};
void configureStyle(UiRuntime& runtime, const std::filesystem::path& assets);
void drawUi(UiRuntime& runtime, UiState& state, const Trace& trace, const GpuCapture& gpu, Viewport& view);
} // namespace mesh_lab
