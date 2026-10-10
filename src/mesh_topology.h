#pragma once

#include "mesh_duplicates.h"
#include <span>

namespace woby {

enum class TopologyMode { originalIndex, exactPosition };
[[nodiscard]] TopologyMode normalizedTopologyMode(TopologyMode mode);
[[nodiscard]] const char* topologyModeName(TopologyMode mode);
[[nodiscard]] TopologyMode parseTopologyMode(const std::string& name);

struct TopologyInspectionSettings {
    bool nonManifoldVertices = true, holes = true;
    bool showNonManifoldVertices = true, showHoles = true;
    float holeSizeRatioTolerance = .05f;
    bool fins = true, showFins = true;
    float finMaxAreaRatio = 1.0f;
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
struct AnalysisVertex { std::array<double, 3> position{}; };
struct AnalysisFace {
    TopologyFaceReference reference;
    std::array<size_t, 3> vertices{};
};
// Geometry owns no incidence or findings. Point provenance permits a later
// topology upgrade without transforming or welding source coordinates again.
struct SourceAnalysisGeometry {
    uint64_t fileId = 0, batchId = 0;
    std::string source;
    SourceProvenance provenance = SourceProvenance::importerVertices;
    TopologyMode mode = TopologyMode::originalIndex;
    bool available = true;
    size_t excludedCollapsedFaces = 0;
    std::vector<AnalysisVertex> vertices;
    std::vector<AnalysisFace> faces;
    std::vector<TopologyPointReference> pointReferences;
    std::vector<size_t> referenceOffsets;
};
struct AnalysisGeometry {
    TopologyMode mode = TopologyMode::originalIndex;
    size_t availableSources = 0, unavailableSources = 0, excludedCollapsedFaces = 0;
    std::vector<std::shared_ptr<const SourceAnalysisGeometry>> sources;
};
struct TopologyVertex {
    std::span<const TopologyPointReference> references;
    std::span<const size_t> faces, edges, boundaryEdges;
};
struct TopologyFace {
    std::array<size_t, 3> edges{};
    size_t component = 0;
};
struct TopologyEdgeUse { size_t face = 0; bool forward = false; };
struct TopologyEdge {
    std::array<size_t, 2> vertices{};
    std::span<const TopologyEdgeUse> incidentFaces;
    bool windingConflict = false, orientationContradiction = false;
};
// Immutable incidence storage is shared when a result snapshot is copied. The
// spans in vertices/edges remain valid across copies, moves and cache publication.
struct TopologyIncidence {
    std::vector<size_t> vertexFaces, vertexEdges, boundaryEdges;
    std::vector<TopologyEdgeUse> edgeUses;
};
struct SourceTopology {
    std::shared_ptr<const SourceAnalysisGeometry> geometry;
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
    std::shared_ptr<const TopologyIncidence> incidence;
    uint64_t batchId = 0;
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
enum class FinBoundaryKind { none, simpleLoop, multipleLoops, open, branched };
[[nodiscard]] const char* finBoundaryKindName(FinBoundaryKind kind);
struct TopologyFinPatch {
    size_t source = 0, patch = 0, splitComponentCount = 0;
    std::vector<size_t> faces, physicalBoundaryEdges, cutBoundaryEdges;
    FinBoundaryKind boundary = FinBoundaryKind::none;
    size_t boundaryComponents = 0;
    double area = 0, denominatorArea = 0, areaRatio = 0;
    bool candidate = false;
};
enum TopologyFindingStage : uint32_t {
    topologyEdgeFindings = 1u << 0, topologyVertexLinks = 1u << 1,
    topologyBoundaryRegions = 1u << 2, topologyFinPatches = 1u << 3,
    topologyAllFindings = 15
};
struct MeshTopology {
    std::shared_ptr<const AnalysisGeometry> geometry;
    uint32_t completedFindings = 0;
    TopologyMode mode = TopologyMode::originalIndex;
    size_t availableSources = 0, unavailableSources = 0, excludedCollapsedFaces = 0;
    // The source graphs are immutable after construction. Result/export copies
    // share them; threshold-dependent selections below remain independently owned.
    std::shared_ptr<const std::vector<SourceTopology>> sourceStorage;
    std::span<const SourceTopology> sources;
    std::vector<TopologyEdgeFinding> boundaries, nonManifoldEdges, windingEdges;
    std::vector<TopologyFaceReference> windingFaces;
    size_t orientationContradictions = 0;
    std::vector<TopologyVertexFinding> nonManifoldVertices;
    size_t excludedNonManifoldEdgeVertices = 0;
    std::vector<TopologyBoundary> boundaryRegions;
    TopologyInspectionSettings inspection;
    std::vector<size_t> holes; // Indices into boundaryRegions; filtered without rebuilding topology.
    std::vector<TopologyFinPatch> finPatches;
    std::vector<size_t> fins; // Indices into finPatches after the inclusive area filter.
    size_t unavailableFinAreaSources = 0;
    std::array<double, 3> coordinateOrigin{}; // Report positions in original world coordinates.
};

// Returns true when a geometry filter changed. Run/Show retain cached findings.
bool filterTopologyFindings(MeshTopology& topology, TopologyInspectionSettings settings, std::stop_token stop = {});

[[nodiscard]] const char* topologyStatus(const MeshTopology& topology);
[[nodiscard]] const char* finStatus(const MeshTopology& topology);
[[nodiscard]] std::shared_ptr<const AnalysisGeometry> buildAnalysisGeometry(
    const std::vector<DuplicateSource>& sources, TopologyMode mode = TopologyMode::originalIndex, std::stop_token stop = {});
[[nodiscard]] MeshTopology buildTopologyIncidence(std::shared_ptr<const AnalysisGeometry> geometry, std::stop_token stop = {});
void inspectTopologyFindings(MeshTopology& topology, uint32_t stages = topologyAllFindings, std::stop_token stop = {});
// Pure, source-scoped topology. Source files are never welded together.
// Original IDs are partitioned by world transform; exact-position keys use doubles.
[[nodiscard]] MeshTopology buildMeshTopology(const std::vector<DuplicateSource>& sources,
    TopologyMode mode = TopologyMode::originalIndex, std::stop_token stop = {});

} // namespace woby
