#pragma once
#include "gpu_runtime.h"
#include <imgui.h>

namespace mesh_lab {
struct UiRuntime {
    ImFont* regular = nullptr;
    ImFont* mono = nullptr;
    ImFont* title = nullptr;
    bool loading = false;
    std::string message;
};
struct UiActions {
    std::optional<size_t> workflow;
    bool reload = false, saveCopy = false;
};
void configureStyle(UiRuntime& runtime, const std::filesystem::path& assets);
void drawUi(UiRuntime& runtime, UiState& state, const WorkflowLibrary& library, const GpuCapture& gpu, Viewport& view, UiActions& actions);
} // namespace mesh_lab
