#include "surface_mesh_quality.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace woby {
namespace {
using Point = std::array<double, 3>;
void canceled(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
}
Point difference(const Point& a, const Point& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
double squared(const Point& p) { return p[0]*p[0] + p[1]*p[1] + p[2]*p[2]; }
QualityStatistics statistics(std::vector<double> values, std::stop_token stop)
{
    if (values.empty()) { return {}; }
    std::sort(values.begin(), values.end(), [&](double a, double b) { canceled(stop); return a < b; });
    const auto quantile = [&](double fraction) {
        const double position = fraction * static_cast<double>(values.size() - 1);
        const auto lower = static_cast<size_t>(position);
        const auto upper = std::min(lower + 1, values.size() - 1);
        return std::lerp(values[lower], values[upper], position - static_cast<double>(lower));
    };
    return {values.size(), values.front(), quantile(.05), quantile(.5), quantile(.95), values.back()};
}
}

SurfaceQualitySettings normalizedSurfaceQualitySettings(SurfaceQualitySettings settings)
{
    if (static_cast<unsigned>(settings.metric) >= surfaceQualityMetricCount) {
        settings.metric = SurfaceQualityMetric::longestEdge;
    }
    settings.minimumSize = std::isfinite(settings.minimumSize) ? std::clamp(settings.minimumSize, 0.0f, 1e12f) : 0;
    settings.maximumSize = std::isfinite(settings.maximumSize) ? std::clamp(settings.maximumSize, 0.0f, 1e12f) : 1;
    if (settings.minimumEnabled && settings.maximumEnabled) {
        settings.maximumSize = std::max(settings.minimumSize, settings.maximumSize);
    }
    return settings;
}
const char* surfaceQualityMetricName(SurfaceQualityMetric metric)
{
    switch (metric) {
    case SurfaceQualityMetric::longestEdge: return "Longest edge";
    case SurfaceQualityMetric::equivalentSize: return "Equivalent size";
    case SurfaceQualityMetric::shape: return "Shape quality";
    case SurfaceQualityMetric::sizeJump: return "Local size jump";
    }
    throw std::invalid_argument("Unknown surface mesh quality metric.");
}
const char* surfaceQualityMetricKey(SurfaceQualityMetric metric)
{
    switch (metric) {
    case SurfaceQualityMetric::longestEdge: return "longest_edge";
    case SurfaceQualityMetric::equivalentSize: return "equivalent_size";
    case SurfaceQualityMetric::shape: return "shape";
    case SurfaceQualityMetric::sizeJump: return "size_jump";
    }
    throw std::invalid_argument("Unknown surface mesh quality metric.");
}
SurfaceQualityMetric parseSurfaceQualityMetric(const std::string& key)
{
    for (size_t i = 0; i < surfaceQualityMetricCount; ++i) {
        const auto metric = static_cast<SurfaceQualityMetric>(i);
        if (key == surfaceQualityMetricKey(metric)) { return metric; }
    }
    throw std::invalid_argument("Unknown surface mesh quality metric.");
}

SurfaceMeshQuality inspectSurfaceMeshQuality(const Mesh& mesh, std::stop_token stop)
{
    canceled(stop);
    if (mesh.indices.size() % 3 != 0) { throw std::invalid_argument("Quality requires triangle indices."); }
    SurfaceMeshQuality result;
    std::map<Point, size_t> vertices;
    struct EdgeUse { size_t count = 0; size_t first = 0, second = 0; };
    std::map<std::pair<size_t, size_t>, EdgeUse> edges;
    result.triangles.reserve(mesh.indices.size() / 3);
    for (size_t face = 0; face < mesh.indices.size() / 3; ++face) {
        canceled(stop);
        std::array<Point, 3> p;
        for (size_t k = 0; k < 3; ++k) {
            const auto index = mesh.indices[face * 3 + k];
            if (index >= mesh.vertices.size() || !finitePosition(mesh.vertices[index].position)) {
                throw std::invalid_argument("Quality requires valid indices and finite coordinates.");
            }
            const auto& v = mesh.vertices[index].position;
            p[k] = {v[0], v[1], v[2]};
        }
        const auto u = difference(p[1], p[0]), v = difference(p[2], p[0]);
        const Point cross = {u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
        const std::array<double, 3> edgeSquared = {squared(u), squared(v), squared(difference(p[2], p[1]))};
        const double maximumSquared = *std::max_element(edgeSquared.begin(), edgeSquared.end());
        const double crossSquared = squared(cross);
        TriangleQuality t;
        t.area = .5 * std::sqrt(crossSquared);
        t.degenerate = crossSquared <= maximumSquared * maximumSquared * 1e-24;
        t.values = {std::sqrt(maximumSquared), std::sqrt(4 * t.area / std::sqrt(3.0)),
            t.degenerate ? 0 : std::clamp(4 * std::sqrt(3.0) * t.area /
                (edgeSquared[0] + edgeSquared[1] + edgeSquared[2]), 0.0, 1.0),
            std::numeric_limits<double>::quiet_NaN()};
        result.triangles.push_back(t);
        if (t.degenerate) { ++result.degenerateTriangles; continue; }
        std::array<size_t, 3> ids;
        for (size_t k = 0; k < 3; ++k) {
            ids[k] = vertices.emplace(p[k], vertices.size()).first->second;
        }
        for (size_t k = 0; k < 3; ++k) {
            auto& use = edges[std::minmax(ids[k], ids[(k + 1) % 3])];
            if (use.count == 0) { use.first = face; }
            else if (use.count == 1) { use.second = face; }
            ++use.count;
        }
    }
    // Exact geometric welding matches OBJ/STL seam handling in topology inspection.
    // Boundary/non-manifold edges do not define a unique neighbor pair.
    for (const auto& [edge, use] : edges) {
        (void)edge;
        canceled(stop);
        if (use.count != 2) { continue; }
        auto& a = result.triangles[use.first];
        auto& b = result.triangles[use.second];
        const double jump = std::max(a.values[1], b.values[1]) / std::min(a.values[1], b.values[1]);
        for (auto* t : {&a, &b}) {
            t->values[3] = std::isfinite(t->values[3]) ? std::max(t->values[3], jump) : jump;
        }
    }
    std::vector<std::pair<double, double>> sizes;
    for (size_t metric = 0; metric < surfaceQualityMetricCount; ++metric) {
        std::vector<double> values;
        values.reserve(result.triangles.size());
        for (const auto& t : result.triangles) {
            canceled(stop);
            if (!t.degenerate && std::isfinite(t.values[metric])) { values.push_back(t.values[metric]); }
            if (metric == 0 && !t.degenerate) { sizes.emplace_back(t.values[0], t.area); }
        }
        result.statistics[metric] = statistics(std::move(values), stop);
    }
    std::sort(sizes.begin(), sizes.end(), [&](const auto& a, const auto& b) { canceled(stop); return a < b; });
    result.prefixAreas.reserve(sizes.size() + 1);
    result.sortedSizes.reserve(sizes.size());
    result.prefixAreas.push_back(0);
    for (const auto& [size, area] : sizes) {
        canceled(stop);
        result.sortedSizes.push_back(size);
        result.prefixAreas.push_back(result.prefixAreas.back() + area);
    }
    return result;
}

std::array<QualityDistribution, surfaceQualityMetricCount> surfaceQualityDistributions(
    const SurfaceMeshQuality& a, const SurfaceMeshQuality& b, std::stop_token stop)
{
    std::array<QualityDistribution, surfaceQualityMetricCount> result;
    const std::array<const SurfaceMeshQuality*, 2> sides = {&a, &b};
    for (size_t metric = 0; metric < surfaceQualityMetricCount; ++metric) {
        auto& d = result[metric];
        // Size starts at zero, shape always spans [0,1], growth starts at one.
        d.minimum = metric == 3 ? 1 : 0;
        d.maximum = d.minimum;
        for (const auto* side : sides) {
            d.maximum = std::max(d.maximum, side->statistics[metric].maximum);
        }
        if (metric == 2) { d.maximum = 1; }
        if (d.maximum <= d.minimum) { d.maximum = d.minimum + 1; }
        for (size_t side = 0; side < sides.size(); ++side) {
            for (const auto& t : sides[side]->triangles) {
                canceled(stop);
                if (t.degenerate || !std::isfinite(t.values[metric])) { continue; }
                const double position = std::clamp((t.values[metric] - d.minimum) / (d.maximum - d.minimum), 0.0, 1.0);
                const auto bin = std::min(static_cast<size_t>(position * surfaceQualityBinCount), surfaceQualityBinCount - 1);
                ++d.bins[side][bin];
                ++d.counts[side];
            }
        }
    }
    return result;
}

QualitySizeLimits surfaceQualitySizeLimits(const SurfaceMeshQuality& quality, const SurfaceQualitySettings& input)
{
    const auto settings = normalizedSurfaceQualitySettings(input);
    QualitySizeLimits result;
    result.validTriangles = quality.sortedSizes.size();
    if (result.validTriangles == 0) { return result; }
    const size_t low = settings.minimumEnabled ? static_cast<size_t>(std::lower_bound(quality.sortedSizes.begin(),
        quality.sortedSizes.end(), settings.minimumSize) - quality.sortedSizes.begin()) : 0;
    const size_t high = settings.maximumEnabled ? static_cast<size_t>(std::upper_bound(quality.sortedSizes.begin(),
        quality.sortedSizes.end(), settings.maximumSize) - quality.sortedSizes.begin()) : result.validTriangles;
    result.below = low;
    result.above = result.validTriangles - high;
    result.trianglePercent = 100.0 * static_cast<double>(result.below + result.above) / static_cast<double>(result.validTriangles);
    const double area = quality.prefixAreas.back();
    result.areaPercent = area > 0 ? std::clamp(100.0 * (quality.prefixAreas[low] + area - quality.prefixAreas[high]) / area, 0.0, 100.0) : 0;
    return result;
}

std::vector<Vertex> surfaceQualityVertices(const Mesh& mesh, const SurfaceMeshQuality& quality,
    SurfaceQualityMetric metric, const QualityDistribution& distribution)
{
    std::vector<Vertex> result;
    result.reserve(mesh.indices.size());
    for (size_t i = 0; i < quality.triangles.size(); ++i) {
        const auto& t = quality.triangles[i];
        const double value = t.values.at(static_cast<size_t>(metric));
        const float encoded = t.degenerate ? -1.0f : !std::isfinite(value) ? -2.0f :
            static_cast<float>(std::clamp((value - distribution.minimum) / (distribution.maximum - distribution.minimum), 0.0, 1.0));
        const auto& a = mesh.vertices.at(mesh.indices.at(i * 3)).position;
        const auto& b = mesh.vertices.at(mesh.indices.at(i * 3 + 1)).position;
        const auto& c = mesh.vertices.at(mesh.indices.at(i * 3 + 2)).position;
        const auto normal = calculateFaceNormal(a, b, c);
        for (size_t k = 0; k < 3; ++k) {
            auto vertex = mesh.vertices.at(mesh.indices.at(i * 3 + k));
            vertex.normal = normal;
            vertex.texcoord = {encoded, 0};
            result.push_back(vertex);
        }
    }
    return result;
}

std::array<float, 4> surfaceQualityColor(double value, SurfaceQualityMetric metric)
{
    if (value < -1.5) { return {.56f, .61f, .67f, 1}; }
    if (value < 0) { return {1, 0, 1, 1}; }
    float amount = static_cast<float>(std::clamp(value, 0.0, 1.0));
    if (metric == SurfaceQualityMetric::shape) { amount = 1 - amount; }
    // Blue -> yellow -> red. Matches comparison.frag.sc; size colors are descriptive.
    const std::array<float, 3> low = {.18f, .48f, .85f}, middle = {1, .82f, .3f}, high = {.94f, .22f, .055f};
    const auto& from = amount <= .5f ? low : middle;
    const auto& to = amount <= .5f ? middle : high;
    const float t = amount <= .5f ? amount * 2 : (amount - .5f) * 2;
    return {std::lerp(from[0], to[0], t), std::lerp(from[1], to[1], t), std::lerp(from[2], to[2], t), 1};
}
} // namespace woby
