#pragma once
#include "pipeline.h"

namespace mesh_lab {
// Inspector state only. The production mesh is an immutable capture, never a
// second editable scene. GPU handles, jobs and ImGui state live in Runtime.
struct UiState {
    Node node = Node::pack;
    size_t triangle = 0, corner = 0, component = 0;
    float yaw = 0.6f, pitch = 0.35f, zoom = 1.0f;
};
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
