#pragma once
#include "workflow.h"

namespace mesh_lab {
enum class InspectorTab { stage, mesh, bytes };
// Inspector state only. The production mesh is an immutable capture, never a
// second editable scene. GPU handles, jobs and ImGui state live in Runtime.
struct UiState {
    size_t workflow = noWorkflow, workflowNode = 0;
    Node node = Node::pack;
    size_t triangle = 0, corner = 0, component = 0;
    float yaw = 0.6f, pitch = 0.35f, zoom = 1.0f;
    InspectorTab inspectorTab = InspectorTab::stage;
    bool fitDiagram = true;
};
struct WorkspaceState {
    std::array<UiState, 2> panes;
    size_t paneCount = 1, activePane = 0;
    bool chooseComparison = false, showInspector = true, showLibrary = true;
    float inspectorFraction = .55f;
};
void openWorkflowPane(WorkspaceState& state, const WorkflowLibrary& library, size_t workflow, size_t pane);
void closeWorkflowPane(WorkspaceState& state, size_t pane);
void focusWorkflowPane(WorkspaceState& state, size_t pane);
void setComparisonSelection(WorkspaceState& state, bool enabled);
void setWorkflowInspectorVisible(WorkspaceState& state, bool visible);
void setWorkflowLibraryVisible(WorkspaceState& state, bool visible);
void setInspectorFraction(WorkspaceState& state, float fraction);
void selectInspectorTab(UiState& state, InspectorTab tab, bool hasMesh = true);
void setDiagramFit(UiState& state, bool fit);
[[nodiscard]] float workflowDiagramScale(const Workflow& workflow, std::array<float, 2> available, bool fit);
[[nodiscard]] WorkspaceState reconcileWorkspace(const WorkspaceState& state, const WorkflowLibrary& previous, const WorkflowLibrary& next);
void selectWorkflow(UiState& state, const WorkflowLibrary& library, size_t index);
void selectWorkflowNode(UiState& state, const Workflow& workflow, size_t index);
void selectTriangle(UiState& state, const Trace& trace, size_t triangle);
void selectCorner(UiState& state, size_t corner);
void selectComponent(UiState& state, size_t component);
void selectVertex(UiState& state, const Trace& trace, size_t vertex);
void selectLine(UiState& state, const Trace& trace, size_t line);
void selectNode(UiState& state, Node node);
void selectPosition(UiState& state, const Trace& trace, size_t position);
void orbit(UiState& state, float yawDelta, float pitchDelta, float zoomFactor);
[[nodiscard]] uint32_t selectedVertex(const UiState& state, const Trace& trace);
} // namespace mesh_lab
