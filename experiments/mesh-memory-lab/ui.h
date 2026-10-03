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
struct OpenWorkflowRequest { size_t workflow = noWorkflow, pane = 0; };
struct UiActions {
    std::optional<OpenWorkflowRequest> open;
    std::optional<size_t> close, saveCopy;
    bool reload = false;
};
void configureStyle(UiRuntime& runtime, const std::filesystem::path& assets);
void drawUi(UiRuntime& runtime, WorkspaceState& state, const WorkflowLibrary& library,
    const std::array<GpuCapture, 2>& gpu, Viewport& view, UiActions& actions);
} // namespace mesh_lab
