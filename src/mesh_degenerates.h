#pragma once

#include "mesh_duplicates.h"

namespace woby {

struct DegenerateSettings {
    bool enabled = true, show = true;
    float needleThresholdRatio = 1000;
    float capMinAngleDegrees = 177.5f;
    friend bool operator==(const DegenerateSettings&, const DegenerateSettings&) = default;
};

struct TriangleDegeneracy {
    bool collapsed = false, needle = false, cap = false;
    // Zero-length edges have no ratio or angle (NaN). An overflowing ratio is infinity.
    double edgeRatio = 0, maximumAngleDegrees = 0;
};

struct DegenerateFinding {
    uint64_t fileId = 0, partId = 0;
    size_t triangleId = 0; // Generated source triangle, zero based.
    std::string source;
    SourceProvenance provenance = SourceProvenance::importerVertices;
    TriangleDegeneracy reasons;
    std::array<std::array<float, 3>, 3> geometry{};
};

struct MeshDegenerates {
    DegenerateSettings settings;
    size_t availableSources = 0, unavailableSources = 0;
    size_t collapsedCount = 0, needleCount = 0, capCount = 0;
    std::vector<DegenerateFinding> findings; // Union: each source face/part occurs once.
};

[[nodiscard]] DegenerateSettings normalizedDegenerateSettings(DegenerateSettings settings);
[[nodiscard]] bool sameDegenerateThresholds(const DegenerateSettings& a, const DegenerateSettings& b);
[[nodiscard]] const char* triangleProvenanceName(SourceProvenance provenance);
[[nodiscard]] const char* degenerateStatus(const MeshDegenerates& result);
[[nodiscard]] TriangleDegeneracy classifyDegenerateTriangle(
    const std::array<std::array<double, 3>, 3>& points, const DegenerateSettings& settings);
// Uses retained source records, independent of duplicate settings and display offsets.
[[nodiscard]] MeshDegenerates inspectDegenerates(const std::vector<DuplicateSource>& sources,
    DegenerateSettings settings, std::stop_token stop = {});

} // namespace woby
