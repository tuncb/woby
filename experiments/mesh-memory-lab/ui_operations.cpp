#include "ui_state.h"
#include <algorithm>
#include <cmath>

namespace mesh_lab {
void openWorkflowPane(WorkspaceState& state, const WorkflowLibrary& library, size_t workflow, size_t pane)
{
    if (pane >= state.panes.size() || workflow >= library.entries.size() || !library.entries[workflow].document) { return; }
    if (pane == 1 && state.panes[0].workflow == noWorkflow) { pane = 0; }
    selectWorkflow(state.panes[pane], library, workflow);
    state.paneCount = std::max(state.paneCount, pane + 1);
    state.activePane = pane;
    state.chooseComparison = false;
}
void closeWorkflowPane(WorkspaceState& state, size_t pane)
{
    if (state.paneCount != 2 || pane >= state.paneCount) { return; }
    if (pane == 0) { state.panes[0] = state.panes[1]; }
    state.panes[1] = {};
    state.paneCount = 1; state.activePane = 0; state.chooseComparison = false;
}
void focusWorkflowPane(WorkspaceState& state, size_t pane)
{
    if (pane < state.paneCount) { state.activePane = pane; }
}
void setComparisonSelection(WorkspaceState& state, bool enabled) { state.chooseComparison = enabled; }
void setWorkflowInspectorVisible(WorkspaceState& state, bool visible) { state.showInspector = visible; }
void setWorkflowLibraryVisible(WorkspaceState& state, bool visible) { state.showLibrary = visible; }
void setInspectorFraction(WorkspaceState& state, float fraction)
{
    if (std::isfinite(fraction)) { state.inspectorFraction = std::clamp(fraction,.35f,.70f); }
}
void selectInspectorTab(UiState& state, InspectorTab tab, bool hasMesh)
{
    if (tab == InspectorTab::stage || tab == InspectorTab::mesh || tab == InspectorTab::bytes) {
        state.inspectorTab = !hasMesh && tab == InspectorTab::bytes ? InspectorTab::stage : tab;
    }
}
void setDiagramFit(UiState& state, bool fit) { state.fitDiagram = fit; }
float workflowDiagramScale(const Workflow& workflow, std::array<float, 2> available, bool fit)
{
    if (!fit) { return 1; }
    float scale = 1;
    for (size_t axis = 0; axis < available.size(); ++axis) {
        if (std::isfinite(available[axis])) {
            scale = std::min(scale, std::max(0.0f, available[axis]) / (workflow.extent[axis] + 4));
        }
    }
    // Keep very large arbitrary graphs navigable by scrolling at a usable size.
    return std::clamp(scale, .35f, 1.0f);
}

WorkspaceState reconcileWorkspace(const WorkspaceState& state, const WorkflowLibrary& previous, const WorkflowLibrary& next)
{
    WorkspaceState result;
    result.showInspector = state.showInspector;
    result.showLibrary = state.showLibrary;
    setInspectorFraction(result,state.inspectorFraction);
    size_t retained = 0;
    for (size_t pane = 0; pane < state.paneCount; ++pane) {
        const auto& old = state.panes[pane];
        if (old.workflow >= previous.entries.size()) { continue; }
        const auto& previousEntry = previous.entries[old.workflow];
        for (size_t i = 0; i < next.entries.size(); ++i) {
            const auto& entry = next.entries[i];
            if (!entry.document || entry.relativePath != previousEntry.relativePath) { continue; }
            auto& restored = result.panes[retained];
            selectWorkflow(restored, next, i);
            restored.yaw = old.yaw; restored.pitch = old.pitch; restored.zoom = old.zoom;
            selectInspectorTab(restored, old.inspectorTab, entry.trace != nullptr); setDiagramFit(restored, old.fitDiagram);
            selectComponent(restored, old.component); selectCorner(restored, old.corner);
            if (entry.trace) { selectTriangle(restored, *entry.trace, old.triangle); }
            if (previousEntry.document && old.workflowNode < previousEntry.document->nodes.size()) {
                const auto& nodeId = previousEntry.document->nodes[old.workflowNode].id;
                for (size_t n = 0; n < entry.document->nodes.size(); ++n) {
                    if (entry.document->nodes[n].id == nodeId) { selectWorkflowNode(restored, *entry.document, n); break; }
                }
            }
            if (pane == state.activePane) { result.activePane = retained; }
            ++retained; break;
        }
    }
    result.paneCount = std::max(size_t{1}, retained);
    if (retained == 0) { selectWorkflow(result.panes[0], next, findWorkflow(next, {})); }
    return result;
}
void selectWorkflowNode(UiState& state, const Workflow& workflow, size_t index)
{
    if (index >= workflow.nodes.size()) { return; }
    state.workflowNode = index;
    if (workflow.nodes[index].inspector) { selectNode(state, *workflow.nodes[index].inspector); }
}
void selectWorkflow(UiState& state, const WorkflowLibrary& library, size_t index)
{
    if (index >= library.entries.size() || !library.entries[index].document) { return; }
    state = {};
    state.workflow = index;
    selectWorkflowNode(state, *library.entries[index].document, library.entries[index].document->initialNode);
}
void selectTriangle(UiState& state, const Trace& trace, size_t triangle)
{
    state.triangle = trace.triangles.empty() ? 0 : std::min(triangle, trace.triangles.size() - 1);
}
void selectCorner(UiState& state, size_t corner) { state.corner = std::min(corner, size_t{2}); }
void selectComponent(UiState& state, size_t component) { state.component = std::min(component, size_t{7}); }
void selectVertex(UiState& state, const Trace& trace, size_t vertex)
{
    const auto found = std::find(trace.mesh.indices.begin(), trace.mesh.indices.end(), vertex);
    if (found != trace.mesh.indices.end()) {
        const auto index = static_cast<size_t>(found - trace.mesh.indices.begin());
        selectTriangle(state, trace, index / 3);
        selectCorner(state, index % 3);
    }
}
void selectLine(UiState& state, const Trace& trace, size_t line)
{
    if (line >= trace.lines.size()) { return; }
    const auto& source = trace.lines[line];
    if (source.face >= 0) { selectTriangle(state, trace, trace.faces[static_cast<size_t>(source.face)].firstTriangle); return; }
    for (size_t t = 0; t < trace.triangles.size(); ++t) {
        for (size_t c = 0; c < 3; ++c) {
            const auto& key = trace.triangles[t].corners[c];
            if ((source.position >= 0 && source.position == key.position)
                || (source.texcoord >= 0 && source.texcoord == key.texcoord)
                || (source.normal >= 0 && source.normal == key.normal)) {
                selectTriangle(state, trace, t); selectCorner(state, c); return;
            }
        }
    }
}
void selectNode(UiState& state, Node node)
{
    if (node >= Node::source && node < Node::count) { state.node = node; }
}
void selectPosition(UiState& state, const Trace& trace, size_t position)
{
    if (position < trace.positionVertices.size() && !trace.positionVertices[position].empty()) {
        selectVertex(state, trace, trace.positionVertices[position].front());
    }
}
void orbit(UiState& state, float yawDelta, float pitchDelta, float zoomFactor)
{
    if (!std::isfinite(yawDelta) || !std::isfinite(pitchDelta) || !std::isfinite(zoomFactor) || zoomFactor <= 0) { return; }
    state.yaw = std::remainder(state.yaw + yawDelta, 6.2831853f);
    state.pitch = std::clamp(state.pitch + pitchDelta, -1.45f, 1.45f);
    state.zoom = std::clamp(state.zoom * zoomFactor, 0.4f, 4.0f);
}
uint32_t selectedVertex(const UiState& state, const Trace& trace)
{
    return trace.mesh.indices.at(state.triangle * 3 + state.corner);
}
} // namespace mesh_lab
