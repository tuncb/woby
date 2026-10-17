#pragma once

#include "ui_state.h"

namespace woby {

struct ComparisonSourceTransform {
    SceneObjectId objectId = invalidSceneObjectId;
    size_t fileIndex = invalidSceneNodeIndex, groupIndex = invalidSceneNodeIndex;
    CoordinateMatrix parent{}, model{};
    Coordinate origin{};
};

// Small transform records only; no geometry work or UI dependencies.
std::vector<ComparisonSourceTransform> comparisonSourceTransforms(const UiState& state);
Coordinate comparisonAlignmentOffset(const UiComparison& comparison, ComparisonSide side,
    const std::vector<ComparisonSourceTransform>& transforms);
SceneComparisonPartRecord::Placement comparisonPartPlacement(const UiState& state,
    const ComparisonSourceTransform& transform, const Coordinate& offset);
void resolveComparisonPlacement(const UiState& state, const UiFileState& file,
    const SceneComparisonPartRecord::Placement& placement, CoordinateMatrix& parent, CoordinateMatrix& model);
bool validComparisonPlacement(const SceneComparisonPartRecord::Placement& placement);
bool comparisonOriginCandidate(const UiState& state, const UiComparison& comparison,
    ComparisonSide side, SceneObjectId objectId);
// Called at operation/load boundaries. Existing locked parts keep their placement.
void synchronizeComparisonPlacement(UiState& state, UiComparison& comparison);
SceneComparisonOriginRecord comparisonOriginReference(const UiState& state, SceneObjectId id);
SceneObjectId comparisonOriginObject(const UiState& state, const SceneComparisonOriginRecord& reference);

} // namespace woby
