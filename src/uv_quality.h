#pragma once

#include "comparison_settings.h"
#include "analysis_display.h"

#include <stop_token>

namespace woby {

using UvPoint = std::array<double, 2>;
struct UvTriangleQuality {
    uint64_t partId = 0;
    size_t triangle = 0; // One based, within the imported patch.
    size_t patch = 0; // Distinguishes domains even without assigned scene IDs.
    double angleDegrees = 0, areaLog2 = 0, surfaceArea = 0, uvArea = 0;
    // Surface -> UV; per-patch mode divides by sqrt(total UV / surface area).
    double minStretch = 0, maxStretch = 0, anisotropy = 1;
    std::array<UvPoint, 3> uv{};
    int orientation = 0;
    bool missing = false, collapsed = false, degenerateSurface = false, mixedOrientation = false;
    bool overlapping = false, crossPatchOverlap = false;
};

inline constexpr size_t uvHistogramBins = 12;
struct UvQualityStatistics {
    size_t count = 0, thresholdCount = 0, nearCollapseCount = 0, highlightedCount = 0;
    double minimum = 0, median = 0, percentile95 = 0, maximum = 0;
    double surfaceArea = 0, thresholdAreaPercent = 0, highlightedAreaPercent = 0;
    double histogramMinimum = 0, histogramMaximum = 1;
    std::array<size_t, uvHistogramBins> counts{};
    std::array<double, uvHistogramBins> areas{};
};
struct UvOverlapPair { size_t first = 0, second = 0; bool crossPatch = false; };
struct UvOverlapLimits {
    // Candidates overlap strictly on both UV axes. Search work bounds index updates and queries.
    size_t candidates = 2000000, pairs = 10000;
    std::optional<size_t> searchWork = std::nullopt; // Default: saturating 64 * valid triangles + 4 * candidates.
    friend bool operator==(const UvOverlapLimits&, const UvOverlapLimits&) = default;
};
enum class UvOverlapTruncation { none, candidates, pairs, searchWork };
[[nodiscard]] const char* uvOverlapTruncationKey(UvOverlapTruncation reason);

struct UvMeasurements {
    // Absolute values. Normalization and overlap flags are applied only by uvTriangleQuality.
    std::vector<UvTriangleQuality> triangles;
    std::vector<double> patchAreaLog2, patchStretchScale;
    size_t missing = 0, collapsed = 0, degenerateSurface = 0, mixedOrientationPatches = 0;
};
struct UvOverlapResult {
    std::shared_ptr<const UvMeasurements> measurements;
    UvOverlapScope scope = UvOverlapScope::perPatch;
    UvOverlapLimits limits;
    std::vector<uint8_t> flags; // Bit 0: within patch; bit 1: cross patch.
    std::vector<UvOverlapPair> pairs;
    size_t candidates = 0, overlappingTriangles = 0, crossPatchPairs = 0;
    size_t searchWork = 0, searchWorkLimit = 0;
    UvOverlapTruncation truncation = UvOverlapTruncation::none;
    bool truncated = false;
};

struct UvQuality {
    std::shared_ptr<const UvMeasurements> measurements;
    std::shared_ptr<const UvOverlapResult> overlap, overlapCache;
    UvOverlapLimits overlapLimits;
    // Metric distributions exclude threshold/range-dependent counts and findings.
    std::array<std::optional<UvQualityStatistics>, 12> distributions;
    // Computed once on the worker; UI navigation never scans all triangles.
    std::vector<size_t> findings;
    UvAreaNormalization normalization = UvAreaNormalization::perPatch;
    UvQualityMetric metric = UvQualityMetric::angle;
    ComparisonSettings settings;
    UvQualityStatistics statistics;
};

[[nodiscard]] const UvMeasurements& uvMeasurements(const UvQuality& quality);
[[nodiscard]] const UvOverlapResult& uvOverlapResult(const UvQuality& quality);
[[nodiscard]] UvTriangleQuality uvTriangleQuality(const UvQuality& quality, size_t index);

[[nodiscard]] UvQuality analyzeUvQuality(const Mesh& mesh, UvAreaNormalization normalization, UvQualityMetric metric,
    std::stop_token stop = {});
[[nodiscard]] std::vector<UvQualityFace> uvQualityFaces(const Mesh& display, const UvQuality& quality, std::stop_token stop = {});
[[nodiscard]] std::vector<Vertex> uvQualityVertices(const Mesh& display, std::stop_token stop = {});
[[nodiscard]] std::vector<Vertex> uvQualityVertices(const Mesh& display, const UvQuality& quality,
    std::stop_token stop = {});
// Updates an owned worker copy, reusing geometric metrics and compatible overlap results.
void updateUvQualitySettings(UvQuality& quality, const ComparisonSettings& settings, std::stop_token stop = {});
[[nodiscard]] const char* uvFindingLabel(const UvTriangleQuality& triangle, const ComparisonSettings& settings = {});
[[nodiscard]] std::optional<std::array<std::array<float, 3>, 3>> uvFindingGeometry(const Mesh& display, size_t finding);
[[nodiscard]] const char* uvQualityMetricKey(UvQualityMetric metric);
[[nodiscard]] UvQualityMetric parseUvQualityMetric(const std::string& key);
[[nodiscard]] const char* uvQualityMetricName(UvQualityMetric metric);
[[nodiscard]] const char* uvQualityLegend(UvQualityMetric metric);
[[nodiscard]] double uvQualityValue(const UvTriangleQuality& triangle, UvQualityMetric metric);
[[nodiscard]] bool validUvTriangle(const UvTriangleQuality& triangle);
[[nodiscard]] bool uvQualityExceedsThreshold(const UvTriangleQuality& triangle, const ComparisonSettings& settings);
[[nodiscard]] bool uvQualityHighlighted(const UvTriangleQuality& triangle, const ComparisonSettings& settings);
[[nodiscard]] UvQuality analyzeUvQuality(const Mesh& mesh, const ComparisonSettings& settings, std::stop_token stop = {});
// Run on the analysis worker; cancellation never publishes a partial result.
// Pair order: current box in (patch if scoped, minimum U, triangle index) order,
// then active boxes in (minimum V, sweep rank) order. first is the earlier sweep box.
void inspectUvOverlaps(UvQuality& quality, std::stop_token stop = {}, UvOverlapLimits limits = {});
void updateUvQualityStatistics(UvQuality& quality, std::stop_token stop = {});
[[nodiscard]] std::array<float, 4> uvQualityColor(double encodedValue, bool area, bool highlighted = false);

} // namespace woby
