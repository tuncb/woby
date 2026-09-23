#pragma once

#include "mesh_topology.h"

namespace woby {

// Zero explicitly opts out of that budget. Defaults keep interactive checks bounded.
struct IntersectionLimits {
    size_t pairs = 10000, candidateTests = 1000000;
    friend bool operator==(const IntersectionLimits&, const IntersectionLimits&) = default;
};
struct IntersectionSettings {
    bool autoUpdate = false, show = true;
    IntersectionLimits limits;
    friend bool operator==(const IntersectionSettings&, const IntersectionSettings&) = default;
};
struct IntersectionFinding {
    std::array<TopologyFaceReference, 2> faces;
    std::string source;
    SourceProvenance provenance = SourceProvenance::importerVertices;
    TopologyMode mode = TopologyMode::originalIndex;
    std::array<std::array<float, 3>, 6> geometry{};
};
enum class IntersectionPhase { notChecked, queued, running, complete, outdated, canceled, failed };
struct MeshIntersections {
    IntersectionSettings settings;
    IntersectionPhase phase = IntersectionPhase::notChecked;
    bool hasResult = false;
    std::string error;
    TopologyMode mode = TopologyMode::automatic;
    size_t availableSources = 0, unavailableSources = 0, excludedCollapsedFaces = 0;
    size_t candidateTests = 0, affectedFaces = 0;
    bool truncated = false;
    IntersectionLimits limits;
    std::string truncationReason;
    std::vector<IntersectionFinding> findings;
};

[[nodiscard]] const char* intersectionStatus(const MeshIntersections& result);
// Exact for the supplied finite doubles. No proximity epsilon is used.
[[nodiscard]] bool exactTriangleCollapsed(const std::array<std::array<double, 3>, 3>& points);
// Valid shared vertices/edges are excluded; duplicate faces and overlap beyond
// shared features are included. Vertex IDs belong to one source topology.
[[nodiscard]] bool trianglesSelfIntersect(const std::array<std::array<double, 3>, 3>& a,
    const std::array<std::array<double, 3>, 3>& b,
    const std::array<size_t, 3>& aIds, const std::array<size_t, 3>& bIds);
[[nodiscard]] MeshIntersections inspectIntersections(const MeshTopology& topology,
    IntersectionSettings settings = {true, true}, std::stop_token stop = {}, IntersectionLimits limits = {});

} // namespace woby
