#include "scene_dimensions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace woby {
namespace {

DimensionPoint transformed(const PickMatrix& model, const DimensionPoint& point)
{
    DimensionPoint result{};
    for (size_t row = 0; row < 3; ++row) {
        result[row] = model[12 + row];
        for (size_t axis = 0; axis < 3; ++axis) { result[row] += model[axis * 4 + row] * point[axis]; }
    }
    return result;
}

bool measurable(const ScenePickPart& part)
{
    return part.selected && part.mesh && part.indexCount != 0 && part.opacity > 0;
}

} // namespace

std::optional<SceneDimensions> sceneDimensions(std::span<const ScenePickPart> parts)
{
    const auto count = std::count_if(parts.begin(), parts.end(), measurable);
    if (count == 0) { return {}; }
    const bool local = count == 1;
    constexpr double infinity = std::numeric_limits<double>::infinity();
    DimensionPoint low{infinity, infinity, infinity}, high{-infinity, -infinity, -infinity};
    const ScenePickPart* single = nullptr;
    for (const auto& part : parts) {
        if (!measurable(part)) { continue; }
        single = &part;
        const auto& mesh = *part.mesh;
        const size_t begin = std::min(part.indexOffset, mesh.indices.size());
        const size_t end = begin + std::min(part.indexCount, mesh.indices.size() - begin);
        for (size_t i = begin; i < end; ++i) {
            const auto index = mesh.indices[i];
            if (index >= mesh.vertices.size() || !finitePosition(mesh.vertices[index].position)) { continue; }
            const auto& vertex = mesh.vertices[index].position;
            DimensionPoint point{vertex[0], vertex[1], vertex[2]};
            if (!local) { point = transformed(part.model, point); }
            if (!std::all_of(point.begin(), point.end(), [](double value) { return std::isfinite(value); })) { continue; }
            for (size_t axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], point[axis]);
                high[axis] = std::max(high[axis], point[axis]);
            }
        }
    }
    if (!std::isfinite(low[0])) { return {}; }
    SceneDimensions result;
    result.objectAxes = local;
    for (size_t corner = 0; corner < 8; ++corner) {
        for (size_t axis = 0; axis < 3; ++axis) {
            result.corners[corner][axis] = (corner & (size_t{1} << axis)) ? high[axis] : low[axis];
        }
        if (local) { result.corners[corner] = transformed(single->model, result.corners[corner]); }
        if (!std::all_of(result.corners[corner].begin(), result.corners[corner].end(),
                [](double value) { return std::isfinite(value); })) { return {}; }
    }
    for (size_t axis = 0; axis < 3; ++axis) {
        // Compute lengths from the linear transform, avoiding subtraction of
        // translated corners when a small object is far from the origin.
        const double scale = local ? std::hypot(single->model[axis * 4],
            single->model[axis * 4 + 1], single->model[axis * 4 + 2]) : 1.0;
        result.lengths[axis] = (high[axis] - low[axis]) * scale;
    }
    return result;
}

const std::optional<SceneDimensions>& updateSceneDimensions(SceneDimensionsCache& cache,
    std::span<const ScenePickPart> parts, uint64_t generation, uint64_t revision)
{
    std::vector<DimensionPartKey> keys;
    for (const auto& part : parts) {
        if (!measurable(part)) { continue; }
        keys.push_back({part.mesh->vertices.data(), part.mesh->indices.data(), part.mesh->vertices.size(),
            part.mesh->indices.size(), part.indexOffset, part.indexCount, part.model});
    }
    if (cache.keys != keys || cache.generation != generation || cache.revision != revision) {
        cache.dimensions = sceneDimensions(parts);
        cache.keys = std::move(keys);
        cache.generation = generation;
        cache.revision = revision;
    }
    return cache.dimensions;
}

SceneGrid sceneGrid(const Bounds& bounds, SceneUpAxis upAxis)
{
    const size_t second = upAxis == SceneUpAxis::y ? 2u : 1u;
    const float extent = std::max({std::abs(bounds.min[0]), std::abs(bounds.max[0]),
        std::abs(bounds.min[second]), std::abs(bounds.max[second]), defaultDisplayBoundsMax});
    const float rawSpacing = std::max(extent * 0.1f, 0.001f);
    const float magnitude = std::pow(10.0f, std::floor(std::log10(rawSpacing)));
    const float normalized = rawSpacing / magnitude;
    SceneGrid result;
    result.spacing = magnitude * (normalized <= 1 ? 1 : normalized <= 2 ? 2 : normalized <= 5 ? 5 : 10);
    result.radius = std::max(1, static_cast<int>(std::ceil(extent / result.spacing)));
    result.extent = static_cast<float>(result.radius) * result.spacing;
    return result;
}

std::optional<PickPoint> projectDimensionPoint(const DimensionPoint& point, const ScenePickView& view)
{
    if (view.width == 0 || view.height == 0) { return {}; }
    std::array<double, 4> clip{point[0], point[1], point[2], 1};
    for (const auto* matrix : {&view.view, &view.projection}) {
        std::array<double, 4> next{};
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = 0; column < 4; ++column) { next[row] += (*matrix)[column * 4 + row] * clip[column]; }
        }
        clip = next;
    }
    if (!std::all_of(clip.begin(), clip.end(), [](double value) { return std::isfinite(value); })
        || clip[3] <= 0 || clip[2] < (view.homogeneousDepth ? -clip[3] : 0)
        || clip[2] > clip[3] || std::abs(clip[0]) > clip[3] || std::abs(clip[1]) > clip[3]) { return {}; }
    return PickPoint{static_cast<float>((clip[0] / clip[3] * .5 + .5) * view.width),
        static_cast<float>((.5 - clip[1] / clip[3] * .5) * view.height)};
}

} // namespace woby
