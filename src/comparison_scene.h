#pragma once

#include "ui_state.h"
#include <optional>

namespace woby
{

struct ComparisonTreeNode
{
    SceneObjectId objectId = invalidSceneObjectId;
    UiSceneNodeKind kind = UiSceneNodeKind::folder;
    std::string name;
    size_t partCount = 0;
    size_t enabledPartCount = 0;
    size_t triangleCount = 0;
    std::vector<ComparisonTreeNode> children;
};

struct ComparisonInputSummary
{
    size_t partCount = 0;
    size_t enabledPartCount = 0;
    std::string sourceNames;
    std::string issue;
};

[[nodiscard]] ComparisonInputSummary comparisonInputSummary(
    const UiState& state, ComparisonSide side, SceneObjectId id);

// A filtered view of the source hierarchy; empty branches are omitted.
// Membership is owned by the comparison; source objects are never reparented.
[[nodiscard]] std::vector<ComparisonTreeNode> comparisonTree(const UiState& state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);

// Group comparison uses the same hierarchy transforms as the renderer,
// independently of ordinary scene visibility and appearance settings.
[[nodiscard]] Mesh comparisonWorldMesh(const UiState &state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] uint64_t comparisonGeometrySignature(const UiState &state, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] std::optional<Bounds> comparisonDisplayBounds(const UiState& state, SceneObjectId id);

} // namespace woby
