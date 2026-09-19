#pragma once

#include "surface_mesh_quality.h"
#include "mesh_duplicates.h"
#include "mesh_degenerates.h"
#include "mesh_topology.h"
#include "mesh_intersections.h"

#include <cstddef>
#include <string>

namespace woby
{

enum class ComparisonSide { a, b };
enum class DiagnosticCategory { boundary, nonManifold, winding, duplicatePoints, duplicateTriangles, degenerateTriangles, nonManifoldVertices, holes, selfIntersections };
inline constexpr size_t backgroundDetectorCount = static_cast<size_t>(DiagnosticCategory::selfIntersections);
inline constexpr size_t diagnosticCategoryCount = backgroundDetectorCount + 1;
inline constexpr std::array<const char*, diagnosticCategoryCount> diagnosticCategoryKeys = {
    "boundary_edges", "non_manifold_edges", "inconsistently_oriented_tris", "duplicate_points",
    "duplicate_tris", "degenerate_tris", "non_manifold_vertices", "holes", "self_intersections"
};

struct DetectorRequest {
    uint64_t revision = 0;
    bool cancel = false;
};

struct ComparisonMembership
{
    bool a = false;
    bool b = false;
    friend bool operator==(const ComparisonMembership &, const ComparisonMembership &) = default;
};

[[nodiscard]] inline bool comparisonMember(ComparisonMembership membership, ComparisonSide side)
{
    return side == ComparisonSide::a ? membership.a : membership.b;
}

enum class ComparisonMode
{
    distance,
    original,
    repaired,
    overlay,
    surfaceQuality
};

struct ComparisonSettings
{
    bool enabled = false;
    ComparisonMode mode = ComparisonMode::distance;
    bool distanceOnOriginal = false;
    float tolerance = 0.05f;
    float colorRange = 0.5f;
    bool showEdges = false;
    bool showBoundaries = true;
    bool showNonManifold = true;
    bool showWinding = true;
    bool autoUpdateBoundaries = true, autoUpdateNonManifold = true, autoUpdateWinding = true;
    TopologyInspectionSettings topologyInspection;
    TopologyMode topologyMode = TopologyMode::automatic;
    ComparisonSide diagnosticSide = ComparisonSide::a;
    DiagnosticCategory diagnosticCategory = DiagnosticCategory::boundary;
    SurfaceQualitySettings quality;
    DuplicateSettings duplicates;
    DegenerateSettings degenerates;
    IntersectionSettings intersections;
    friend bool operator==(const ComparisonSettings &, const ComparisonSettings &) = default;
};

[[nodiscard]] ComparisonSettings normalizedComparisonSettings(ComparisonSettings settings);
// The legacy detector enabled fields now specify automatic scheduling, not result visibility.
[[nodiscard]] bool diagnosticAutoUpdate(const ComparisonSettings& settings, DiagnosticCategory category);
void setDiagnosticAutoUpdate(ComparisonSettings& settings, DiagnosticCategory category, bool automatic);

} // namespace woby
