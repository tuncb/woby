#include "text_annotation.h"
#include "surface_annotation.h"
#include "math_utils.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace woby {
TextAnnotationSettings normalizedTextAnnotationSettings(TextAnnotationSettings s)
{
    if (s.text.size() > 8192 || s.text.find('\0') != std::string::npos) {
        throw std::invalid_argument("Text must contain at most 8192 bytes and no NUL characters.");
    }
    for (const auto values : {s.position, s.rotationDegrees}) {
        for (const auto v : values) {
            if (!std::isfinite(v)) { throw std::invalid_argument("Text coordinates must be finite."); }
        }
    }
    if (!std::isfinite(s.size) || s.size <= 0) { throw std::invalid_argument("Text size must be finite and positive."); }
    s.size = std::clamp(s.size, 1e-6f, 1e6f);
    for (auto& v : s.rotationDegrees) { v = std::remainder(v, 360.0f); }
    return s;
}
PickMatrix textAnnotationTransform(const TextAnnotationSettings& text, const ScenePickView& view)
{
    PickMatrix model;
    if (text.faceCamera) {
        inverseMatrix(model.data(), view.view.data());
        for (size_t k = 0; k < 3; ++k) {
            model[k] *= text.size;
            model[4 + k] *= -text.size;
        }
        model[12] = text.position[0]; model[13] = text.position[1]; model[14] = text.position[2];
    } else {
        transformMatrix(model.data(), text.size, -text.size, text.size,
            radians(text.rotationDegrees[0]), radians(text.rotationDegrees[1]), radians(text.rotationDegrees[2]),
            text.position[0], text.position[1], text.position[2]);
    }
    return multiplyMatrices(multiplyMatrices(model, view.view), view.projection);
}
std::optional<PickPoint> projectTextAnnotationPoint(const PickMatrix& transform, const ScenePickView& view, PickPoint p)
{
    const auto clip = annotationTransform(transform, {p[0], p[1], 0, 1});
    if (clip[3] <= 0 || clip[2] < (view.homogeneousDepth ? -clip[3] : 0) || clip[2] > clip[3]) { return {}; }
    const PickPoint result{(clip[0] / clip[3] + 1) * .5f * static_cast<float>(view.width),
        (1 - clip[1] / clip[3]) * .5f * static_cast<float>(view.height)};
    if (!std::isfinite(result[0]) || !std::isfinite(result[1])) { return {}; }
    return result;
}
std::optional<std::array<float, 3>> textAnnotationPlanePoint(
    const ScenePickView& view, PickPoint point, const std::array<float, 3>& anchor)
{
    const auto camera = annotationTransform(view.view, {anchor[0], anchor[1], anchor[2], 1});
    const auto clip = annotationTransform(view.projection, camera);
    if (clip[3] <= 0) { return {}; }
    PickMatrix inverseProjection, inverseView;
    inverseMatrix(inverseProjection.data(), view.projection.data());
    inverseMatrix(inverseView.data(), view.view.data());
    const auto ndc = annotationNdc(view, point);
    AnnotationProjector projectionInverse{};
    std::copy(inverseProjection.begin(), inverseProjection.end(), projectionInverse.begin());
    auto a = annotationTransform(projectionInverse, {ndc[0], ndc[1], view.homogeneousDepth ? -1.0f : 0.0f, 1});
    auto b = annotationTransform(projectionInverse, {ndc[0], ndc[1], .5, 1});
    if (std::abs(a[3]) < 1e-20 || std::abs(b[3]) < 1e-20) { return {}; }
    for (size_t k = 0; k < 3; ++k) { a[k] /= a[3]; b[k] /= b[3]; }
    if (std::abs(b[2] - a[2]) < 1e-20) { return {}; }
    const double distance = (camera[2] - a[2]) / (b[2] - a[2]);
    std::array<double, 3> eye{};
    for (size_t k = 0; k < 3; ++k) { eye[k] = a[k] + distance * (b[k] - a[k]); }
    std::array<float, 3> result{};
    for (size_t row = 0; row < 3; ++row) {
        double value = inverseView[12 + row];
        for (size_t k = 0; k < 3; ++k) { value += inverseView[k * 4 + row] * eye[k]; }
        result[row] = static_cast<float>(value);
        if (!std::isfinite(result[row])) { return {}; }
    }
    return result;
}
std::optional<std::array<float, 3>> textAnnotationPlacement(std::span<const ScenePickPart> parts,
    const ScenePickView& view, PickPoint point, const std::array<float, 3>& fallback)
{
    SceneTriangleHit hit;
    (void)pickSceneObject(parts, view, point, &hit);
    if (hit.partId && hit.triangle) {
        for (const auto& part : parts) {
            if (part.objectId != hit.partId || part.analysisId != hit.analysisId || !part.mesh) { continue; }
            const auto indices = std::span(meshGeometry(*part.mesh).indices).subspan(part.indexOffset, part.indexCount);
            if ((hit.triangle - 1) * 3 + 2 >= indices.size()) { continue; }
            std::array<float, 3> local{};
            for (size_t v = 0; v < 3; ++v) {
                const auto p = meshPosition(*part.mesh, indices[(hit.triangle - 1) * 3 + v]);
                for (size_t k = 0; k < 3; ++k) { local[k] += static_cast<float>(p[k] * hit.barycentric[v]); }
            }
            const auto p = annotationTransform(part.model, {local[0], local[1], local[2], 1});
            return std::array<float, 3>{p[0], p[1], p[2]};
        }
    }
    return textAnnotationPlanePoint(view, point, fallback);
}
} // namespace woby
