#pragma once

#include "scene_dimensions.h"
#include "surface_annotation.h"
#include "comparison_scene.h"
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace woby {
struct ComparisonRuntimes;

// Every cache includes both the live state owner and document generation.
struct SceneQueryStamp {
    const UiState* owner = nullptr;
    uint64_t generation = 0, geometry = 0, visibility = 0;
    bool operator==(const SceneQueryStamp&) const = default;
};
[[nodiscard]] SceneQueryStamp sceneQueryStamp(const UiState& state);

struct SceneTreeSummary { size_t parts = 0, visible = 0; };
struct SceneTreeQueries {
    SceneQueryStamp stamp;
    boost::unordered_flat_map<SceneObjectId, SceneTreeSummary> nodes;
    std::array<boost::unordered_flat_set<SceneObjectId>, 2> members;
    SceneObjectId comparison = 0;
    uint64_t analysis = 0, builds = 0, membershipBuilds = 0;
    bool membershipValid = false;
};

// No borrowed pointers survive preparation. Meshes are resolved by index/ID
// when a consumer requests a short-lived view, including after vector relocation.
struct ScenePartRecord {
    ScenePickPart value; // mesh/pointCloud/diagnosticEdges are always empty.
    std::vector<SceneObjectId> ancestors;
    SceneObjectId fileId = 0;
    uint64_t contentRevision = 0;
    bool visible = false, framingVisible = false;
};
struct ScenePartQueries {
    SceneQueryStamp stamp;
    std::vector<ScenePartRecord> records;
    std::vector<SceneObjectId> selection;
    uint64_t picking = 0, builds = 0, selectionBuilds = 0, displayBuilds = 0;
};

struct AnnotationProjectionSource { size_t partIndex = 0; PickMatrix transform{}; };
struct AnnotationQuery {
    SceneObjectId id = 0;
    uint64_t revision = 0, builds = 0;
    bool visible = false, valid = false;
    std::vector<DiagnosticEdge> lines;
    // Camera-dependent tessellation is independent of world-space geometry.
    ScenePickView view;
    float width = 0;
    uint64_t projectedBuild = 0, projectionBuilds = 0;
    // Retained scratch stores indices/transforms, never borrowed part pointers.
    std::vector<AnnotationProjectionSource> sources;
    std::vector<std::array<float, 3>> projected;
};
struct SceneAnnotationQueries {
    SceneQueryStamp stamp;
    uint64_t revision = 0, builds = 0;
    boost::unordered_flat_map<SceneObjectId, AnnotationQuery> objects;
};

struct ComparisonQuery {
    ComparisonInspectorCache inputs;
    uint64_t boundsSignature = 0, boundsBuilds = 0;
    bool boundsValid = false;
    std::optional<Bounds> bounds;
};
struct SceneSelectionQueries {
    SceneQueryStamp stamp;
    std::vector<SceneObjectId> selection;
    uint64_t annotations = 0, analysis = 0, presentation = 0;
    uint64_t boundsBuilds = 0, dimensionBuilds = 0;
    uint64_t comparisonKey = 0;
    std::optional<Bounds> bounds;
    std::optional<SceneDimensions> dimensions;
};
struct SceneBoundsQueries {
    SceneQueryStamp stamp;
    uint64_t analysis = 0, presentation = 0, builds = 0;
};
struct SceneQueryRuntime {
    SceneBoundsQueries bounds;
    SceneTreeQueries tree;
    ScenePartQueries parts;
    SceneAnnotationQueries annotations;
    SceneSelectionQueries selection;
    boost::unordered_flat_map<SceneObjectId, ComparisonQuery> comparisons;
    const UiState* comparisonOwner = nullptr;
    uint64_t comparisonGeneration = 0, comparisonAnalysis = 0;
};

[[nodiscard]] uint64_t sceneComparisonRevision(const UiState& state, const ComparisonRuntimes& comparisons);
void updateSceneBoundsQuery(SceneQueryRuntime& runtime, UiState& state);
void updateSceneTreeQueries(SceneTreeQueries& cache, const UiState& state);
[[nodiscard]] SceneTreeSummary sceneTreeSummary(const SceneTreeQueries& cache, SceneObjectId id);
[[nodiscard]] bool sceneTreeMember(const SceneTreeQueries& cache, SceneObjectId id, ComparisonSide side);
void updateScenePartQueries(ScenePartQueries& cache, const UiState& state);
void resolveSceneParts(SceneQueryRuntime& runtime, const UiState& state,
    std::vector<ScenePickPart>& parts, bool includeHidden = false);
void updateSceneAnnotationQueries(SceneQueryRuntime& runtime, const UiState& state);
void updateAnnotationProjection(AnnotationQuery& cache, const UiAnnotation& item,
    std::span<const ScenePickPart> parts, const ScenePickView& view);
void appendSceneAnnotationParts(SceneQueryRuntime& runtime, const UiState& state, std::vector<ScenePickPart>& parts);
ComparisonQuery& updateSceneComparisonQuery(SceneQueryRuntime& runtime, const UiState& state, SceneObjectId id);
void updateSceneSelectionQueries(SceneQueryRuntime& runtime, const UiState& state,
    const ComparisonRuntimes* comparisons = nullptr);
} // namespace woby
