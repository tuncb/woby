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

// Gesture-owned immutable projection cache. Contains no borrowed mesh pointers.
struct AnnotationProjection {
    SceneObjectId targetId = 0;
    AnnotationGeometry definition;
    std::vector<AnnotationProjectedTriangle> triangles;
    std::vector<size_t> order;
    std::vector<AnnotationProjectionNode> nodes;
};

[[nodiscard]] std::array<float, 2> annotationNdc(const ScenePickView& view, PickPoint point);
[[nodiscard]] AnnotationProjection annotationProjection(std::span<const ScenePickPart> parts,
    const ScenePickView& view, SceneObjectId target);
[[nodiscard]] AnnotationProjection annotationEditProjection(const ScenePickPart& target,
    const AnnotationGeometry& geometry);
[[nodiscard]] SceneObjectId pickAnnotationSurface(const AnnotationProjection& projection,
    std::array<float, 2> point);
// Throws for gaps, occlusion, disconnected layers, or ambiguous intersections.
[[nodiscard]] AnnotationGeometry projectAnnotation(const AnnotationProjection& projection,
    AnnotationShape shape, std::array<float, 2> start, std::array<float, 2> end);
[[nodiscard]] std::string annotationFingerprint(const Mesh& mesh, size_t offset, size_t count);
[[nodiscard]] std::array<float, 3> annotationPosition(const Mesh& mesh, size_t offset,
    uint32_t triangle, const std::array<float, 3>& bary);
// Model-local endpoints, or four corners in outline order, shared by handles and Properties.
[[nodiscard]] std::vector<std::array<float, 3>> annotationControlPositions(const Mesh& mesh,
    size_t offset, const AnnotationGeometry& geometry);
[[nodiscard]] std::array<float, 4> annotationTransform(const PickMatrix& matrix, const std::array<float, 4>& point);
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
