#pragma once

#include "mesh_topology.h"
#include <limits>

namespace woby {

struct ThicknessSettings {
    bool autoUpdate = false, show = true, showMap = false;
    bool absoluteEnabled = false, relativeEnabled = true;
    float maximumThickness = 1, maximumRelativeThickness = .2f, minimumRegionArea = 0;
    int elementsThroughThickness = 3;
    friend bool operator==(const ThicknessSettings&, const ThicknessSettings&) = default;
};
inline constexpr size_t thicknessMapBinCount = 13; // 12 relative-thickness bins, then unknown.
inline constexpr size_t noThicknessFace = std::numeric_limits<size_t>::max();
struct ThicknessFace {
    // NaN means unknown. Minimum is a supported sampled chord, not a global minimum.
    double minimum = std::numeric_limits<double>::quiet_NaN();
    double typical = std::numeric_limits<double>::quiet_NaN();
    double area = 0, confidence = 0;
    std::array<double, 3> from{}, to{};
    size_t opposite = noThicknessFace;
    std::array<size_t, 3> neighbors{noThicknessFace, noThicknessFace, noThicknessFace};
};
struct ThicknessSource {
    std::shared_ptr<const SourceAnalysisGeometry> geometry;
    std::vector<ThicknessFace> faces;
    double reference = 0; // Area-weighted 95th percentile within this analysis batch.
    std::string unavailableReason;
};
struct ThicknessMeasurements {
    std::vector<ThicknessSource> sources;
    size_t availableSources = 0, unavailableSources = 0, measuredFaces = 0, unknownFaces = 0;
};
struct ThinRegion {
    size_t source = 0;
    std::vector<size_t> faces;
    double minimum = 0, typical = 0, relativeThickness = 0, area = 0, confidence = 0, suggestedSize = 0;
    std::array<double, 3> minimumBound{}, maximumBound{}, from{}, to{};
};
struct MeshThickness {
    ThicknessSettings settings;
    std::array<double,3> coordinateOrigin{};
    std::shared_ptr<const ThicknessMeasurements> measurements;
    std::vector<ThinRegion> regions;
};

[[nodiscard]] ThicknessSettings normalizedThicknessSettings(ThicknessSettings settings);
[[nodiscard]] bool sameThicknessFilters(ThicknessSettings a, ThicknessSettings b);
// Exact coordinate welding is independent of the topology display setting.
// Each batch must be closed, manifold, consistently wound and free of detected intersections.
[[nodiscard]] std::shared_ptr<const ThicknessMeasurements> measureMeshThickness(
    const std::vector<DuplicateSource>& sources, std::stop_token stop = {});
[[nodiscard]] MeshThickness filterThinRegions(std::shared_ptr<const ThicknessMeasurements> measurements,
    ThicknessSettings settings, std::stop_token stop = {});
[[nodiscard]] const char* thicknessStatus(const MeshThickness& result);

} // namespace woby
