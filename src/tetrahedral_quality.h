#pragma once

#include "surface_mesh_quality.h"

namespace woby {
enum class TetrahedralQualityMetric { volume, scaledJacobian, meanRatio, minimumAngle, maximumAngle,
    jacobianLower, jacobianUpper, jacobianRatio };
inline constexpr size_t tetrahedralQualityMetricCount = 8;
enum class TetrahedralValidity { positive, reversed, folded, collapsed, unresolved, repeatedNodes };
inline constexpr size_t tetrahedralValidityCount = 6;

struct TetrahedralQualitySettings {
    bool enabled = false;
    TetrahedralQualityMetric metric = TetrahedralQualityMetric::meanRatio;
    bool minimumEnabled = false, maximumEnabled = false;
    float minimum = 0.2f, maximum = 1.0f;
    bool selectedOnly = false;
    friend bool operator==(const TetrahedralQualitySettings&, const TetrahedralQualitySettings&) = default;
};

struct TetrahedralQualityCell {
    std::array<double, tetrahedralQualityMetricCount> values{};
    TetrahedralValidity validity = TetrahedralValidity::unresolved;
    uint32_t firstFace = 0, faceCount = 0;
    // Source cell ordinal (all VTK cell types, or the importer's tet array).
    uint64_t sourceCell = 0, partId = 0;
    uint32_t nodeCount = 4;
};
struct TetrahedralMeshQuality {
    std::vector<TetrahedralQualityCell> cells;
    std::array<QualityStatistics, tetrahedralQualityMetricCount> statistics{};
    std::array<QualityDistribution, tetrahedralQualityMetricCount> distributions{};
    // Ascending finite values, with stable cell-index ties. No per-frame sorting.
    std::array<std::vector<uint32_t>, tetrahedralQualityMetricCount> sorted;
    std::array<size_t, tetrahedralValidityCount> validityCounts{};
    std::vector<uint32_t> validityFindings;
    size_t quadraticCells = 0;
};
struct TetrahedralQualityDisplay {
    std::vector<uint32_t> indices;
    std::vector<uint32_t> edges;
    std::array<std::vector<SurfaceQualityFace>, 2> faces;
};

[[nodiscard]] TetrahedralQualitySettings normalizedTetrahedralQualitySettings(TetrahedralQualitySettings settings);
[[nodiscard]] const char* tetrahedralQualityMetricKey(TetrahedralQualityMetric metric);
[[nodiscard]] const char* tetrahedralQualityMetricName(TetrahedralQualityMetric metric);
[[nodiscard]] TetrahedralQualityMetric parseTetrahedralQualityMetric(const std::string& key);
[[nodiscard]] const char* tetrahedralValidityName(TetrahedralValidity validity);
[[nodiscard]] bool tetrahedralMetricHigherIsBetter(TetrahedralQualityMetric metric);
// Shape/angle measures use corners. Validity and signed volume use all nodes.
// Jacobian bounds use longest-node-distance normalization and outward-rounded
// cubic Bernstein coefficients. Exhausted bounds remain unresolved.
[[nodiscard]] TetrahedralMeshQuality inspectTetrahedralQuality(const Mesh& mesh, std::stop_token stop = {});
[[nodiscard]] TetrahedralQualityDisplay tetrahedralQualityDisplay(const Mesh& mesh,
    const TetrahedralMeshQuality& quality, std::stop_token stop = {});
[[nodiscard]] std::array<size_t, 2> tetrahedralQualityOutsideLimits(const TetrahedralMeshQuality& quality,
    TetrahedralQualitySettings settings);
}
