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

struct ComparisonInspectorInput {
    std::vector<ComparisonTreeNode> roots;
    ComparisonInputSummary summary;
    size_t triangleCount = 0;
    // Include missing references in the root checkbox, so they can be disabled.
    size_t memberCount = 0, enabledMemberCount = 0;
};

// Runtime-only derived data. Geometry/membership/settings invalidate inputs; labels
// refresh the tree without rehashing geometry. Appearance, visibility, navigation and
// result publication are independent. New/Open changes sceneGeneration.
struct ComparisonInspectorCache {
    const UiState* owner = nullptr;
    SceneObjectId objectId = invalidSceneObjectId;
    uint64_t generation = 0, builds = 0;
    uint64_t geometryRevision = 0, analysisRevision = 0, labelsRevision = 0, signatureBuilds = 0;
    uint64_t signature = 0;
    std::array<ComparisonInspectorInput, 2> inputs;
    std::string sources;
};
[[nodiscard]] bool comparisonInspectorCacheCurrent(const ComparisonInspectorCache& cache,
    const UiState& state, SceneObjectId id);
const ComparisonInspectorCache& updateComparisonInspectorCache(ComparisonInspectorCache& cache,
    const UiState& state, SceneObjectId id);

// A filtered view of the source hierarchy; empty branches are omitted.
// Membership is owned by the comparison; source objects are never reparented.
[[nodiscard]] std::vector<ComparisonTreeNode> comparisonTree(const UiState& state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);

// Group comparison uses the same hierarchy transforms as the renderer,
// independently of ordinary scene visibility and appearance settings.
[[nodiscard]] Mesh comparisonWorldMesh(const UiState &state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] uint64_t comparisonGeometrySignature(const UiState &state, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] std::optional<Bounds> comparisonDisplayBounds(const UiState& state, SceneObjectId id);
// Source triangle in scene coordinates, independent of analysis display/layout and visibility.
[[nodiscard]] std::optional<std::array<Coordinate, 3>> comparisonSourceTriangle(
    const UiState& state, SceneObjectId analysisId, SceneObjectId partId, size_t triangle);

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
