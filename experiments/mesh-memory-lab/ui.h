#pragma once
#include "gpu_runtime.h"
#include <imgui.h>

namespace mesh_lab {
struct UiRuntime {
    ImFont* regular = nullptr;
    ImFont* mono = nullptr;
    ImFont* title = nullptr;
};
void configureStyle(UiRuntime& runtime, const std::filesystem::path& assets);
void drawUi(UiRuntime& runtime, UiState& state, const Trace& trace, const GpuCapture& gpu, Viewport& view);
} // namespace mesh_lab
