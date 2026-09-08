#pragma once

#include "mesh_comparison.h"
#include "ui_state.h"

#include <optional>
#include <span>

namespace woby {

using PickMatrix = std::array<float, 16>;
using PickPoint = std::array<float, 2>;

// Runtime input state, in window coordinates. A drag remains a drag even if
// the pointer returns to its starting position. Alt gestures never select.
struct ScenePointerGesture {
    bool active = false, dragging = false, alt = false, toggle = false;
    PickPoint start{};
};
struct SceneClick { PickPoint position{}; bool toggle = false; };
void beginScenePointer(ScenePointerGesture& gesture, PickPoint point, bool alt, bool toggle);
bool moveScenePointer(ScenePointerGesture& gesture, PickPoint point);
[[nodiscard]] std::optional<SceneClick> endScenePointer(
    ScenePointerGesture& gesture, PickPoint point, bool allowed);

// Borrowed geometry in submission order. No graphics handles or UI adapters.
// These views must not survive mesh replacement or comparison recomputation.
struct ScenePickPart {
    SceneObjectId objectId = invalidSceneObjectId;
    const Mesh* mesh = nullptr;
    size_t indexOffset = 0, indexCount = 0;
    PickMatrix model{};
    std::optional<Bounds> bounds;
    bool solid = false, edges = false, vertices = false, selected = false;
    bool edgeXray = true, surfaceLessEqual = false;
    float opacity = 1.0f, pointSize = 4.0f;
    std::span<const DiagnosticEdge> diagnosticEdges;
};

struct ScenePickView {
    PickMatrix view{}, projection{};
    uint32_t width = 1, height = 1;
    bool homogeneousDepth = false;
    // Drawable pixels per window coordinate, for a DPI-independent line tolerance.
    float pixelScale = 1.0f;
};

[[nodiscard]] ScenePickView scenePickView(const SceneCamera& camera, SceneUpAxis upAxis,
    const Bounds& bounds, uint32_t width, uint32_t height, bool homogeneousDepth, float pixelScale);
[[nodiscard]] std::vector<ScenePickPart> scenePickParts(const UiState& state);
[[nodiscard]] std::vector<SceneObjectId> sceneSelectionPath(const UiState& state, SceneObjectId id);
void appendComparisonPickParts(std::vector<ScenePickPart>& parts, const UiComparison& comparison,
    const ComparisonSettings& settings, const MeshComparison& result, bool selected);
// Opaque surfaces occlude; x-ray lines follow draw order. Transparent surfaces
// are selectable but do not write depth, matching the scene's rendering policy.
[[nodiscard]] SceneObjectId pickSceneObject(std::span<const ScenePickPart> parts,
    const ScenePickView& view, PickPoint point);
[[nodiscard]] std::vector<std::array<float, 3>> sceneSelectionLines(std::span<const ScenePickPart> parts);

} // namespace woby
