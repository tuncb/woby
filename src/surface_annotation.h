#pragma once

#include "annotation_types.h"
#include "scene_pick.h"

namespace woby {

struct AnnotationProjectedTriangle {
    SceneObjectId objectId = 0;
    uint32_t triangle = 0;
    std::array<std::array<double, 4>, 3> clip{};
    std::array<std::array<double, 3>, 3> bary{};
    std::array<std::array<float, 3>, 3> localTriangle{};
    std::array<double, 2> minimum{}, maximum{};
};

struct AnnotationProjectionNode {
    std::array<double, 2> minimum{}, maximum{};
    size_t begin = 0, end = 0, left = 0, right = 0;
};

struct AnnotationProjectionVertex {
    std::array<double, 4> clip{};
    std::array<float, 3> local{};
};

struct AnnotationProjectionFace {
    SceneObjectId objectId = 0;
    uint32_t triangle = 0;
    std::array<size_t, 3> vertices{};
    std::array<double, 2> minimum{}, maximum{};
    // Only frustum-clipped faces need explicit interpolated barycentric data.
    std::optional<size_t> clipped;
};

struct AnnotationProjectionBlock {
    SceneObjectId objectId = 0;
    size_t begin = 0, end = 0;
    std::array<float, 3> minimum{}, maximum{};
};

// Gesture-owned immutable projection cache. Contains no borrowed mesh pointers.
struct AnnotationProjection {
    SceneObjectId targetId = 0;
    std::vector<SceneObjectId> targetIds;
    AnnotationGeometry definition;
    std::vector<AnnotationProjectionVertex> vertices;
    std::vector<AnnotationProjectionFace> triangles;
    std::vector<AnnotationProjectedTriangle> clippedTriangles;
    std::vector<size_t> order;
    std::vector<AnnotationProjectionNode> nodes;
    // Gesture-local source-space bounds. They contain values, never mesh pointers.
    std::vector<AnnotationProjectionBlock> blocks;
    std::optional<std::array<float, 4>> region;
};

[[nodiscard]] std::array<float, 2> annotationNdc(const ScenePickView& view, PickPoint point);
[[nodiscard]] AnnotationProjection annotationProjection(std::span<const ScenePickPart> parts,
    const ScenePickView& view, SceneObjectId target, std::span<const SceneObjectId> targets = {});
[[nodiscard]] AnnotationProjection annotationGestureProjection(std::span<const ScenePickPart> parts,
    const ScenePickView& view, SceneObjectId target, std::array<float, 2> point);
void expandAnnotationGestureProjection(AnnotationProjection& projection,
    std::span<const ScenePickPart> parts, const ScenePickView& view,
    std::array<float, 2> start, std::array<float, 2> end);
void setAnnotationProjectionTargets(AnnotationProjection& projection,
    std::span<const ScenePickPart> parts, const ScenePickView& view,
    SceneObjectId target, std::span<const SceneObjectId> targets);
[[nodiscard]] std::vector<SceneObjectId> annotationGroupTargets(const UiState& state, SceneObjectId target);
[[nodiscard]] bool annotationHasTarget(const UiAnnotation& item, SceneObjectId target);
[[nodiscard]] bool annotationHasTarget(const AnnotationProjection& projection, SceneObjectId target);
[[nodiscard]] const ScenePickPart* annotationSourcePart(const UiAnnotation& item,
    std::span<const ScenePickPart> parts, uint32_t source);
[[nodiscard]] const AnnotationProjector& annotationSourceProjector(const AnnotationGeometry& geometry, uint32_t source);
[[nodiscard]] uint32_t annotationSourceIndex(const AnnotationProjection& projection, SceneObjectId target);
[[nodiscard]] AnnotationProjection annotationEditProjection(std::span<const ScenePickPart> parts, const UiAnnotation& item);
[[nodiscard]] std::vector<std::array<float, 3>> annotationControlWorldPositions(const UiAnnotation& item,
    std::span<const ScenePickPart> parts);
[[nodiscard]] AnnotationProjection annotationEditProjection(const ScenePickPart& target,
    const AnnotationGeometry& geometry);
// Reuse a target-discovery projection, removing transparent non-target parts.
void setAnnotationProjectionTarget(AnnotationProjection& projection,
    std::span<const ScenePickPart> parts, const ScenePickView& view, SceneObjectId target);
[[nodiscard]] SceneObjectId pickAnnotationSurface(const AnnotationProjection& projection,
    std::array<float, 2> point);
// Endpoints/corners must hit the target. Bridges empty gaps between surface
// fragments; throws for occlusion, disconnected layers, or ambiguous intersections.
[[nodiscard]] AnnotationGeometry projectAnnotation(const AnnotationProjection& projection,
    AnnotationShape shape, std::array<float, 2> start, std::array<float, 2> end);
// Bounded, sampled drag guide. Chords may leave the surface or miss narrow
// obstructions; always resolve with projectAnnotation before committing.
[[nodiscard]] AnnotationGeometry previewAnnotation(const AnnotationProjection& projection,
    AnnotationShape shape, std::array<float, 2> start, std::array<float, 2> end);
[[nodiscard]] std::string annotationFingerprint(const Mesh& mesh, size_t offset, size_t count);
void prepareAnnotationMeshCache(Mesh& mesh);
[[nodiscard]] std::array<float, 3> annotationPosition(const Mesh& mesh, size_t offset,
    uint32_t triangle, const std::array<float, 3>& bary);
// Model-local endpoints, or four corners in outline order, shared by handles and Properties.
[[nodiscard]] std::vector<std::array<float, 3>> annotationControlPositions(const Mesh& mesh,
    size_t offset, const AnnotationGeometry& geometry);
[[nodiscard]] std::array<float, 4> annotationTransform(const PickMatrix& matrix, const std::array<float, 4>& point);
[[nodiscard]] std::array<double, 4> annotationTransform(const AnnotationProjector& matrix, const std::array<float, 4>& point);
[[nodiscard]] PickMatrix annotationCompose(const PickMatrix& first, const PickMatrix& second);
struct AnnotationSurfaceHit {
    SceneObjectId objectId = 0;
    uint32_t triangle = 0;
    std::array<float, 3> bary{};
};
[[nodiscard]] std::optional<AnnotationSurfaceHit> annotationSurfaceHit(const AnnotationProjection& projection,
    std::array<float, 2> point);
[[nodiscard]] std::vector<DiagnosticEdge> annotationWorldLines(const UiAnnotation& item,
    std::span<const ScenePickPart> parts);
void appendAnnotationPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    std::vector<std::vector<DiagnosticEdge>>& storage);
[[nodiscard]] bool annotationEdgeHit(const UiAnnotation& item, std::span<const ScenePickPart> parts,
    const ScenePickView& view, PickPoint point);

} // namespace woby
