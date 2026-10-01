#include "obj_mesh.h"
#include "utf8_path.h"

#include <rapidobj/rapidobj.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace woby {
namespace {

struct IndexKey {
    int vertex = -1;
    int normal = -1;
    int texcoord = -1;

    bool operator==(const IndexKey& other) const noexcept
    {
        return vertex == other.vertex && normal == other.normal && texcoord == other.texcoord;
    }
};

// First-use IDs are global across shapes. Most corners hit the primary entry
// for their source position; only alternate normal/UV tuples need hashing.
struct VertexIndexTable {
    std::vector<IndexKey> keys;
    std::vector<uint32_t> primary, buckets;
    size_t secondaryCount = 0;
    bool direct = false;
    uint32_t next = 0;
};
constexpr uint32_t emptyBucket = std::numeric_limits<uint32_t>::max();

size_t indexHash(const IndexKey& key)
{
    uint64_t hash = uint64_t(static_cast<uint32_t>(key.vertex)) * 0x9e3779b185ebca87ull;
    hash ^= uint64_t(static_cast<uint32_t>(key.normal)) * 0xc2b2ae3d27d4eb4full;
    hash ^= uint64_t(static_cast<uint32_t>(key.texcoord)) * 0x165667b19e3779f9ull;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    return static_cast<size_t>(hash ^ (hash >> 33));
}

void growSecondary(VertexIndexTable& table)
{
    std::vector<uint32_t> buckets(std::max(size_t{16}, table.buckets.size() * 2), emptyBucket);
    for (const auto id : table.buckets) {
        if (id == emptyBucket) { continue; }
        size_t bucket = indexHash(table.keys[id]) & (buckets.size() - 1);
        while (buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (buckets.size() - 1); }
        buckets[bucket] = id;
    }
    table.buckets = std::move(buckets);
}

uint32_t vertexIndex(VertexIndexTable& table, const IndexKey& key)
{
    auto& primary = table.primary[static_cast<size_t>(key.vertex)];
    if (table.direct) {
        if (primary == emptyBucket) { primary = table.next++; }
        return primary;
    }
    if (primary == emptyBucket) {
        primary = static_cast<uint32_t>(table.keys.size());
        table.keys.push_back(key);
        return primary;
    }
    if (table.keys[primary] == key) { return primary; }
    if (table.buckets.empty()) { growSecondary(table); }
    size_t bucket = indexHash(key) & (table.buckets.size() - 1);
    while (table.buckets[bucket] != emptyBucket) {
        const auto id = table.buckets[bucket];
        if (table.keys[id] == key) { return id; }
        bucket = (bucket + 1) & (table.buckets.size() - 1);
    }
    if (table.secondaryCount >= table.buckets.size() * 3 / 4) {
        growSecondary(table);
        bucket = indexHash(key) & (table.buckets.size() - 1);
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (table.buckets.size() - 1); }
    }
    const auto id = static_cast<uint32_t>(table.keys.size());
    table.keys.push_back(key);
    table.buckets[bucket] = id;
    ++table.secondaryCount;
    return id;
}

void reserveIndexTable(VertexIndexTable& table, size_t positions, size_t corners, bool positionOnly)
{
    table.primary.assign(positions, emptyBucket);
    table.direct = positionOnly;
    if (!table.direct) { table.keys.reserve(std::min(positions, corners)); }
}

rapidobj::Result parseObj(const std::filesystem::path& path)
{
#if defined(_WIN32)
    // RapidOBJ's Windows file reader bypasses the OS page cache. That is useful
    // for large parallel reads but makes thousands of tiny files I/O-bound.
    // Match its single-thread cutoff, retaining ParseFile for larger meshes.
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (!error && bytes <= 1024 * 1024) {
        std::ifstream stream(path, std::ios::binary);
        if (stream) {
            return rapidobj::ParseStream(stream, rapidobj::MaterialLibrary::SearchPath(
                std::filesystem::absolute(path).parent_path(), rapidobj::Load::Optional));
        }
    }
#endif
    return rapidobj::ParseFile(path, rapidobj::MaterialLibrary::Default(rapidobj::Load::Optional));
}

} // namespace

Mesh loadObjMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress)
{
    // Missing material libraries must not prevent importing the geometry.
    reportModelLoadProgress(progress, ModelLoadStage::reading);
    auto result = parseObj(path);
    const auto throwLoadError = [&](const char* operation) {
        std::string message = std::string(operation) + ": " + pathToUtf8(path);
        if (result.error) {
            message += "\n" + result.error.code.message();
            if (result.error.line_num != 0) {
                message += " (line " + std::to_string(result.error.line_num) + ")";
            }
        }
        throw std::runtime_error(message);
    };
    if (result.error) {
        throwLoadError("Failed to load OBJ");
    }
    std::vector<Coordinate> sourcePoints;
    sourcePoints.reserve(result.attributes.positions.size() / 3);
    for (size_t i = 0; i < result.attributes.positions.size(); i += 3) {
        sourcePoints.push_back({result.attributes.positions[i], result.attributes.positions[i+1], result.attributes.positions[i+2]});
    }
    const auto origin = coordinateOrigin(sourcePoints);
    for (size_t i = 0; i < sourcePoints.size(); ++i) {
        sourcePoints[i] = relativePosition(sourcePoints[i], origin);
        for (size_t k = 0; k < 3; ++k) { result.attributes.positions[i*3+k] = sourcePoints[i][k]; }
    }
    reportModelLoadProgress(progress, ModelLoadStage::triangulating);
    if (!rapidobj::Triangulate(result)) {
        throwLoadError("Failed to triangulate OBJ");
    }

    const auto& attrib = result.attributes;
    const auto& shapes = result.shapes;

    Mesh mesh;
    auto source = std::make_shared<SourceMeshData>();
    source->provenance = SourceProvenance::objPositions;
    mesh.origin = origin;
    source->points = std::move(sourcePoints);
    reportModelLoadProgress(progress, ModelLoadStage::sourcePositions, source->points.size(), source->points.size());
    VertexIndexTable vertexMap;
    size_t indexCount = 0;
    for (const auto& shape : shapes) {
        indexCount += shape.mesh.indices.size();
    }
    if (indexCount > std::numeric_limits<uint32_t>::max()
        || source->points.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("OBJ exceeds the supported vertex or index range.");
    }
    // Position count is only an estimate: normal/UV seams can split vertices.
    const size_t vertexCapacity = std::min(attrib.positions.size() / 3u, indexCount);
    mesh.indices.reserve(indexCount);
    mesh.vertices.reserve(vertexCapacity);
    mesh.precisePositions.reserve(vertexCapacity);
    mesh.nodes.reserve(shapes.size());
    source->indices.reserve(indexCount);
    reserveIndexTable(vertexMap, source->points.size(), indexCount,
        attrib.normals.empty() && attrib.texcoords.empty());
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, 0, indexCount);

    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const uint32_t nodeIndexOffset = static_cast<uint32_t>(mesh.indices.size());
        bool hasTexcoords = true;

        for (const auto& index : shape.mesh.indices) {
            hasTexcoords = hasTexcoords && index.texcoord_index >= 0;
            if (index.texcoord_index >= 0) {
                const auto uv = static_cast<size_t>(index.texcoord_index) * 2u;
                hasTexcoords = hasTexcoords && std::isfinite(attrib.texcoords[uv]) && std::isfinite(attrib.texcoords[uv + 1u]);
            }
            if (mesh.indices.size() % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, mesh.indices.size(), indexCount); }
            if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= source->points.size()) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            source->indices.push_back(static_cast<uint32_t>(index.position_index));
            const IndexKey key{index.position_index, index.normal_index, index.texcoord_index};
            const auto mappedIndex = vertexIndex(vertexMap, key);
            if (mappedIndex < mesh.vertices.size()) {
                mesh.indices.push_back(mappedIndex);
                continue;
            }

            if (index.position_index < 0) {
                throw std::runtime_error("OBJ contains a face vertex without a position index.");
            }

            Vertex vertex{};
            const auto& point = source->points[static_cast<size_t>(index.position_index)];
            mesh.precisePositions.push_back(point);
            vertex.position = renderPosition(point);

            if (index.normal_index >= 0) {
                const auto normalIndex = static_cast<size_t>(index.normal_index) * 3u;
                vertex.normal = {
                    attrib.normals[normalIndex + 0u],
                    attrib.normals[normalIndex + 1u],
                    attrib.normals[normalIndex + 2u],
                };
            }

            if (index.texcoord_index >= 0) {
                const auto texcoordIndex = static_cast<size_t>(index.texcoord_index) * 2u;
                vertex.texcoord = {
                    attrib.texcoords[texcoordIndex + 0u],
                    1.0f - attrib.texcoords[texcoordIndex + 1u],
                };
            }

            const uint32_t newIndex = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(vertex);
            mesh.indices.push_back(newIndex);
        }

        const uint32_t nodeIndexCount = static_cast<uint32_t>(mesh.indices.size()) - nodeIndexOffset;
        if (nodeIndexCount > 0) {
            MeshNode node;
            node.hasTexcoords = hasTexcoords;
            node.name = shape.name.empty() ? "shape " + std::to_string(shapeIndex + 1u) : shape.name;
            node.indexOffset = nodeIndexOffset;
            node.indexCount = nodeIndexCount;
            mesh.nodes.push_back(std::move(node));
        }
    }

    if (empty(mesh)) {
        throw std::runtime_error("OBJ did not contain renderable triangles: " + pathToUtf8(path));
    }

    mesh.sourceData = std::move(source);
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, indexCount, indexCount);
    finalizeMesh(mesh, true, progress);
    return mesh;
}

} // namespace woby
