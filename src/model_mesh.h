#pragma once

#include "mesh_duplicates.h"
#include "load_progress.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace woby {

struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> texcoord{};
};

struct Bounds {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
    std::array<float, 3> center{};
    float radius = 1.0f;
};

struct UvQuality;
struct FreeformGeometry;

struct MeshNode {
    std::string name;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    // Source defaults only; editable appearance lives in UiState.
    std::optional<std::array<float, 4>> defaultColor = {};
    bool defaultVisible = true;
    // True only when every corner in this part has supplied, finite UVs.
    bool hasTexcoords = false;
    std::string displayName = {}; // Optional importer label; name stays the saved identity.
    uint32_t lineIndexOffset = 0;
    uint32_t lineIndexCount = 0; // Line groups have indexCount == 0.
    uint64_t sourceObjectId = 0; // Analysis copy: linked source patch, never an importer identity.
    size_t uvQualityOffset = 0; // Triangle offset before optional UV layout filtering.
    uint32_t pointIndexOffset = 0;
    uint32_t pointIndexCount = 0; // Standalone points; each node has only one primitive type.
};

[[nodiscard]] inline const std::string& meshNodeDisplayName(const MeshNode& node)
{
    return node.displayName.empty() ? node.name : node.displayName;
}

struct MeshHierarchyNode {
    std::string name;
    uint32_t parentIndex = UINT32_MAX;
    uint32_t groupIndex = UINT32_MAX; // A container has no geometry of its own.
};

struct MeshAnnotationBlock {
    size_t begin = 0, end = 0;
    std::array<float, 3> minimum{}, maximum{};
};

struct MeshAnnotationNode {
    std::array<float, 3> minimum{}, maximum{};
    size_t begin = 0, end = 0, left = 0, right = 0;
    size_t indexBegin = 0, indexEnd = 0;
};
struct MeshAnnotationIndex {
    std::vector<uint32_t> triangles; // Original index-buffer offsets, in spatial order.
    std::vector<MeshAnnotationNode> tree;
};

struct Mesh;

struct MeshAnnotationCache {
    size_t vertexCount = 0, indexCount = 0;
    const Vertex* vertexData = nullptr;
    const uint32_t* indexData = nullptr;
    std::vector<MeshAnnotationBlock> blocks;
    std::vector<std::string> fingerprints;
    std::shared_ptr<const MeshAnnotationIndex> spatial;
    // Built during preparation, never copied when a gesture/command starts.
    // Only positions, triangle indices, nodes and the derived index are retained.
    std::shared_ptr<const Mesh> snapshot;
};

using Coordinate = std::array<double, 3>;
using CoordinateMatrix = std::array<double, 16>;
void coordinateIdentity(double* result);
void coordinateMultiply(double* result, const double* a, const double* b);
[[nodiscard]] Coordinate transformCoordinate(const double* matrix, const Coordinate& point);

// Process-unique geometry version. Copies preserve it; writers renew it after changing content.
[[nodiscard]] uint64_t nextMeshContentRevision();
struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshNode> nodes;
    Bounds bounds;
    std::optional<std::array<Coordinate, 2>> originalBounds;
    Coordinate origin{}; // Original file coordinates of the local zero.
    std::vector<Coordinate> precisePositions; // CPU positions, in the same local frame as vertices.
    std::shared_ptr<const SourceMeshData> sourceData;
    std::shared_ptr<const DuplicateInput> duplicateInput;
    // Immutable derived geometry index, published by the runtime after preparation.
    std::shared_ptr<const MeshAnnotationCache> annotationCache;
    std::vector<MeshHierarchyNode> hierarchy; // Validated source defaults; editable tree lives in UiState.
    std::vector<uint32_t> lineIndices; // Independent pairs; never fed to triangle analysis.
    std::shared_ptr<const UvQuality> uvQuality;
    std::vector<uint32_t> pointIndices; // Explicit point geometry, separate from mesh vertex overlays.
    std::shared_ptr<const FreeformGeometry> freeform;
    uint64_t contentRevision = nextMeshContentRevision();
    // Prepared analysis geometry only: source batch per vertex. Empty means one batch.
    std::vector<uint32_t> analysisVertexBatches;
};

// After editing published geometry, renew this identity and notify the owning UiState.
void renewMeshContentRevision(Mesh& mesh);

[[nodiscard]] std::span<const uint32_t> meshNodeIndices(const Mesh& mesh, const MeshNode& node);
[[nodiscard]] bool finiteCoordinate(const Coordinate& point) noexcept;
[[nodiscard]] Coordinate meshPosition(const Mesh& mesh, size_t index);
[[nodiscard]] std::array<Coordinate, 2> originalMeshBounds(const Mesh& mesh, const MeshNode* node = nullptr);
[[nodiscard]] std::array<float, 3> renderPosition(const Coordinate& point);
[[nodiscard]] Coordinate coordinateOrigin(const std::vector<Coordinate>& points);
[[nodiscard]] Coordinate relativePosition(const Coordinate& point, const Coordinate& origin);
[[nodiscard]] Coordinate originalPosition(const Coordinate& point, const Coordinate& origin);
// Call once after reading double positions, before normals or bounds are built.
void localizeMesh(Mesh& mesh);
void rebaseMesh(Mesh& mesh, const Coordinate& origin);
[[nodiscard]] bool empty(const Mesh& mesh) noexcept;
[[nodiscard]] bool finitePosition(const std::array<float, 3>& position) noexcept;
[[nodiscard]] bool validNormal(const std::array<float, 3>& normal) noexcept;
[[nodiscard]] std::array<float, 3> calculateFaceNormal(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c);
void generateSmoothNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    const ModelLoadProgressCallback& progress = {});
[[nodiscard]] Bounds calculateBounds(const std::vector<Vertex>& vertices, const ModelLoadProgressCallback& progress = {});
void captureSourceMesh(Mesh& mesh, SourceProvenance provenance, std::span<const uint64_t> originalPointIds = {});
void finalizeMesh(Mesh& mesh, bool generateMissingSmoothNormals, const ModelLoadProgressCallback& progress = {});

} // namespace woby
