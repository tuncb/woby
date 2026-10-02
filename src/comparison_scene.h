#pragma once

#include "ui_state.h"
#include <optional>
#include <stop_token>

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

struct ComparisonInputPart {
    size_t fileIndex = 0, groupIndex = 0;
    CoordinateMatrix parent{};
};

// Owns only participating files. Workers never borrow the live scene's vectors.
struct ComparisonInputSnapshot {
    std::vector<UiFileState> files;
    std::array<std::vector<ComparisonInputPart>, 2> parts;
    ComparisonSettings settings;
    Coordinate origin{};
};

[[nodiscard]] ComparisonInputSnapshot snapshotComparisonInputs(const UiState& state, SceneObjectId id);
[[nodiscard]] Mesh comparisonWorldMesh(const ComparisonInputSnapshot& snapshot, ComparisonSide side,
    std::stop_token stop = {});

struct PreparedComparisonSource {
    std::vector<Vertex> quality;
    std::vector<uint32_t> lines;
};
struct PreparedComparisonInputs {
    std::shared_ptr<const std::array<Mesh, 2>> meshes;
    std::array<PreparedComparisonSource, 2> buffers;
};
// CPU-only preparation; the runtime uploads these buffers on the graphics thread.
[[nodiscard]] PreparedComparisonInputs prepareUvComparisonInputs(const ComparisonInputSnapshot& snapshot,
    std::stop_token stop = {});

} // namespace woby
