#include "obj_mesh.h"
#include "utf8_path.h"

#include <rapidobj/rapidobj.hpp>

#include <algorithm>
#include <bit>
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

// Dense keys and 32-bit bucket references avoid an allocation and pointer chase
// for every vertex. Entries never move logically and preserve first-use IDs.
struct VertexIndexTable {
    std::vector<IndexKey> keys;
    std::vector<uint32_t> buckets;
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

void reserveIndexTable(VertexIndexTable& table, size_t capacity)
{
    table.keys.reserve(capacity);
    table.buckets.assign(std::bit_ceil(std::max(size_t{16}, capacity + capacity / 2)), emptyBucket);
    const size_t mask = table.buckets.size() - 1;
    for (size_t i = 0; i < table.keys.size(); ++i) {
        size_t bucket = indexHash(table.keys[i]) & mask;
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & mask; }
        table.buckets[bucket] = static_cast<uint32_t>(i);
    }
}

uint32_t vertexIndex(VertexIndexTable& table, const IndexKey& key)
{
    size_t mask = table.buckets.size() - 1;
    size_t bucket = indexHash(key) & mask;
    while (table.buckets[bucket] != emptyBucket) {
        const auto index = table.buckets[bucket];
        if (table.keys[index] == key) { return index; }
        bucket = (bucket + 1) & mask;
    }
    if (table.keys.size() >= table.buckets.size() * 3 / 4) {
        reserveIndexTable(table, table.buckets.size());
        mask = table.buckets.size() - 1;
        bucket = indexHash(key) & mask;
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & mask; }
    }
    const auto index = static_cast<uint32_t>(table.keys.size());
    table.keys.push_back(key);
    table.buckets[bucket] = index;
    return index;
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

Mesh loadObjMesh(const std::filesystem::path& path)
{
    // Missing material libraries must not prevent importing the geometry.
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
    if (!rapidobj::Triangulate(result)) {
        throwLoadError("Failed to triangulate OBJ");
    }

    const auto& attrib = result.attributes;
    const auto& shapes = result.shapes;

    Mesh mesh;
    auto source = std::make_shared<SourceMeshData>();
    source->provenance = SourceProvenance::objPositions;
    source->points.reserve(attrib.positions.size() / 3);
    for (size_t i = 0; i < attrib.positions.size(); i += 3) {
        const std::array<float, 3> point = {attrib.positions[i], attrib.positions[i+1], attrib.positions[i+2]};
        if (!finitePosition(point)) { throw std::runtime_error("OBJ contains a non-finite source coordinate."); }
        source->points.push_back({point[0], point[1], point[2]});
    }
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
    mesh.nodes.reserve(shapes.size());
    source->indices.reserve(indexCount);
    reserveIndexTable(vertexMap, vertexCapacity);

    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const uint32_t nodeIndexOffset = static_cast<uint32_t>(mesh.indices.size());

        for (const auto& index : shape.mesh.indices) {
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
            const auto vertexIndex = static_cast<size_t>(index.position_index) * 3u;
            vertex.position = {
                attrib.positions[vertexIndex + 0u],
                attrib.positions[vertexIndex + 1u],
                attrib.positions[vertexIndex + 2u],
            };

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
    finalizeMesh(mesh, true);
    return mesh;
}

} // namespace woby
