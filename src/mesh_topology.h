#pragma once

#include "mesh_duplicates.h"

namespace woby {

enum class TopologyMode { automatic, originalIndex, exactPosition };
[[nodiscard]] TopologyMode normalizedTopologyMode(TopologyMode mode);
[[nodiscard]] const char* topologyModeName(TopologyMode mode);
[[nodiscard]] TopologyMode parseTopologyMode(const std::string& name);

struct TopologyInspectionSettings {
    bool nonManifoldVertices = true, holes = true;
    bool showNonManifoldVertices = true, showHoles = true;
    float holeSizeRatioTolerance = .05f;
    friend bool operator==(const TopologyInspectionSettings&, const TopologyInspectionSettings&) = default;
};
[[nodiscard]] bool sameTopologyInspectionFilters(const TopologyInspectionSettings& a, const TopologyInspectionSettings& b);
[[nodiscard]] TopologyInspectionSettings normalizedTopologyInspectionSettings(TopologyInspectionSettings settings);

struct TopologyFaceReference {
    uint64_t fileId = 0, partId = 0;
    size_t triangleId = 0;
    friend bool operator==(const TopologyFaceReference&, const TopologyFaceReference&) = default;
};
struct TopologyPointReference {
    uint64_t partId = 0;
    size_t pointId = 0;
};
struct TopologyVertex {
    std::array<double, 3> position{};
    std::vector<TopologyPointReference> references;
    std::vector<size_t> faces, edges, boundaryEdges;
    // One link edge per incident triangle, including parallel links from duplicate faces.
    std::vector<std::array<size_t, 2>> link;
};
struct TopologyFace {
    TopologyFaceReference reference;
    std::array<size_t, 3> vertices{}, edges{};
    size_t component = 0;
};
struct TopologyEdgeUse { size_t face = 0; bool forward = false; };
struct TopologyEdge {
    std::array<size_t, 2> vertices{};
    std::vector<TopologyEdgeUse> incidentFaces;
    bool windingConflict = false, orientationContradiction = false;
};
struct SourceTopology {
    uint64_t fileId = 0;
    std::string source;
    SourceProvenance provenance = SourceProvenance::importerVertices;
    TopologyMode mode = TopologyMode::originalIndex;
    bool available = true;
    size_t excludedCollapsedFaces = 0;
    std::vector<TopologyVertex> vertices;
    std::vector<TopologyFace> faces;
    std::vector<TopologyEdge> edges;
    std::vector<std::vector<size_t>> components;
};
struct TopologyEdgeFinding { size_t source = 0, edge = 0; };
struct TopologyVertexFinding { size_t source = 0, vertex = 0, linkComponents = 0; };
enum class BoundaryKind { loop, open, branched };
[[nodiscard]] const char* boundaryKindName(BoundaryKind kind);
struct TopologyBoundary {
    size_t source = 0, component = 0;
    BoundaryKind kind = BoundaryKind::loop;
    // Simple loops are ordered, with the first vertex not repeated at the end.
    std::vector<size_t> vertices, edges;
    std::array<double, 3> minimum{}, maximum{};
    double diagonal = 0, componentDiagonal = 0, sizeRatio = 0;
    bool ratioAvailable = false;
};
struct MeshTopology {
    TopologyMode mode = TopologyMode::automatic;
    size_t availableSources = 0, unavailableSources = 0, excludedCollapsedFaces = 0;
    std::vector<SourceTopology> sources;
    std::vector<TopologyEdgeFinding> boundaries, nonManifoldEdges, windingEdges;
    std::vector<TopologyFaceReference> windingFaces;
    size_t orientationContradictions = 0;
    std::vector<TopologyVertexFinding> nonManifoldVertices;
    size_t excludedNonManifoldEdgeVertices = 0;
    std::vector<TopologyBoundary> boundaryRegions;
    TopologyInspectionSettings inspection;
    std::vector<size_t> holes; // Indices into boundaryRegions; filtered without rebuilding topology.
};

// Returns true when the hole geometry filter changed. Run/Show retain cached findings.
bool filterTopologyFindings(MeshTopology& topology, TopologyInspectionSettings settings, std::stop_token stop = {});

[[nodiscard]] const char* topologyStatus(const MeshTopology& topology);
// Pure, source-scoped topology. Source files are never welded together.
// Original IDs are partitioned by world transform; exact-position keys use doubles.
[[nodiscard]] MeshTopology buildMeshTopology(const std::vector<DuplicateSource>& sources,
    TopologyMode mode = TopologyMode::automatic, std::stop_token stop = {});

} // namespace woby
