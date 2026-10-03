#include "ui_state.h"
#include <algorithm>
#include <cmath>

namespace mesh_lab {
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
