#pragma once

#include "model_mesh.h"

#include <array>
#include <cstddef>
#include <limits>
#include <stop_token>
#include <vector>

namespace woby {

enum class SurfaceQualityMetric { longestEdge, equivalentSize, shape, sizeJump };
inline constexpr size_t surfaceQualityMetricCount = 4;
inline constexpr size_t surfaceQualityBinCount = 12;

struct SurfaceQualitySettings {
    SurfaceQualityMetric metric = SurfaceQualityMetric::longestEdge;
    bool onOriginal = false;
    bool minimumEnabled = false;
    bool maximumEnabled = false;
    float minimumSize = 0;
    float maximumSize = 1;
    friend bool operator==(const SurfaceQualitySettings&, const SurfaceQualitySettings&) = default;
};

struct TriangleQuality {
    // Source face order. NaN means unavailable (no valid manifold neighbor).
    std::array<double, surfaceQualityMetricCount> values{};
    double area = 0;
    bool degenerate = false;
};

struct QualityStatistics {
    size_t count = 0;
    double minimum = 0, percentile5 = 0, median = 0, percentile95 = 0, maximum = 0;
};

struct SurfaceMeshQuality {
    std::vector<TriangleQuality> triangles;
    std::array<QualityStatistics, surfaceQualityMetricCount> statistics{};
    size_t degenerateTriangles = 0;
    // Valid faces sorted by longest edge; prefix areas allow O(log N) size-limit queries.
    std::vector<double> sortedSizes;
    std::vector<double> prefixAreas;
};

struct QualityDistribution {
    double minimum = 0, maximum = 1;
    std::array<std::array<size_t, surfaceQualityBinCount>, 2> bins{};
    std::array<size_t, 2> counts{};
};

struct QualitySizeLimits {
    size_t below = 0, above = 0, validTriangles = 0;
    double trianglePercent = 0, areaPercent = 0;
};

[[nodiscard]] SurfaceQualitySettings normalizedSurfaceQualitySettings(SurfaceQualitySettings settings);
[[nodiscard]] const char* surfaceQualityMetricName(SurfaceQualityMetric metric);
[[nodiscard]] const char* surfaceQualityMetricKey(SurfaceQualityMetric metric);
[[nodiscard]] SurfaceQualityMetric parseSurfaceQualityMetric(const std::string& key);
[[nodiscard]] SurfaceMeshQuality inspectSurfaceMeshQuality(const Mesh& mesh, std::stop_token stop = {});
[[nodiscard]] std::array<QualityDistribution, surfaceQualityMetricCount> surfaceQualityDistributions(
    const SurfaceMeshQuality& a, const SurfaceMeshQuality& b, std::stop_token stop = {});
[[nodiscard]] QualitySizeLimits surfaceQualitySizeLimits(const SurfaceMeshQuality& quality,
    const SurfaceQualitySettings& settings);
// Flat per-face values; invalid faces = -1, unavailable metrics = -2.
// Values are normalized in double precision before conversion for GPU storage.
[[nodiscard]] std::vector<Vertex> surfaceQualityVertices(const Mesh& mesh, const SurfaceMeshQuality& quality,
    SurfaceQualityMetric metric, const QualityDistribution& distribution);
[[nodiscard]] std::array<float, 4> surfaceQualityColor(double normalizedValue, SurfaceQualityMetric metric);

} // namespace woby
