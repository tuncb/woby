#include "obj_mesh.h"
#include "obj_freeform.h"
#include <sstream>
#include "utf8_path.h"

#include <rapidobj/rapidobj.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
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
    std::vector<FreeformPatch> freeformPatches;
    if (result.error && objFreeformStatement(result.error.line)) {
        auto input = readObjFreeform(path, progress);
        std::istringstream stream(std::move(input.polygonText));
        result = rapidobj::ParseStream(stream, rapidobj::MaterialLibrary::SearchPath(
            std::filesystem::absolute(path).parent_path(), rapidobj::Load::Optional));
        freeformPatches = std::move(input.patches);
    }
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
    size_t indexCount = 0, lineIndexCount = 0, pointIndexCount = 0;
    for (const auto& shape : shapes) {
        indexCount += shape.mesh.indices.size();
        pointIndexCount += shape.points.indices.size();
        size_t lineVertices = 0;
        for (const auto count : shape.lines.num_line_vertices) {
            if (count < 2) { throw std::runtime_error("OBJ polyline needs at least two vertices."); }
            lineVertices += static_cast<size_t>(count);
            lineIndexCount += (static_cast<size_t>(count) - 1) * 2;
        }
        if (lineVertices != shape.lines.indices.size()) { throw std::runtime_error("Invalid OBJ polyline range."); }
    }
    // Vertex-only OBJ files are commonly used as point clouds. In files with
    // primitives, only explicit p records become standalone point geometry.
    const bool vertexCloud = indexCount == 0 && lineIndexCount == 0 && pointIndexCount == 0 && freeformPatches.empty();
    if (vertexCloud) { pointIndexCount = source->points.size(); }
    const size_t totalIndices = indexCount + lineIndexCount + pointIndexCount;
    if (totalIndices > std::numeric_limits<uint32_t>::max()
        || source->points.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        throw std::runtime_error("OBJ exceeds the supported vertex or index range.");
    }
    // Position count is only an estimate: normal/UV seams can split vertices.
    const size_t vertexCapacity = std::min(attrib.positions.size() / 3u, totalIndices);
    mesh.indices.reserve(indexCount);
    mesh.lineIndices.reserve(lineIndexCount);
    mesh.pointIndices.reserve(pointIndexCount);
    mesh.vertices.reserve(vertexCapacity);
    mesh.precisePositions.reserve(vertexCapacity);
    mesh.nodes.reserve(shapes.size());
    source->indices.reserve(indexCount);
    reserveIndexTable(vertexMap, source->points.size(), totalIndices,
        attrib.normals.empty() && attrib.texcoords.empty());
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, 0, totalIndices);

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
            if (mesh.indices.size() % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, mesh.indices.size(), totalIndices); }
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

    // Append new primitive groups after all face groups, preserving existing
    // face group indexes and saved appearance in older mixed-geometry scenes.
    std::unordered_set<std::string> names;
    for (const auto& node : mesh.nodes) { names.insert(node.name); }
    const auto groupName = [&](const std::string& base) {
        std::string name = base;
        for (size_t suffix = 2; !names.insert(name).second; ++suffix) { name = base + " (" + std::to_string(suffix) + ")"; }
        return name;
    };
    size_t primitiveVisits = 0;
    const auto primitiveVertex = [&](int positionIndex) {
        if (positionIndex < 0 || static_cast<size_t>(positionIndex) >= source->points.size()) {
            throw std::runtime_error("OBJ contains an invalid source position index.");
        }
        const auto mapped = vertexIndex(vertexMap, {positionIndex, -1, -1});
        if (mapped == mesh.vertices.size()) {
            const auto& point = source->points[static_cast<size_t>(positionIndex)];
            mesh.precisePositions.push_back(point);
            // Unlit primitives do not need normals. A valid placeholder avoids
            // triggering regeneration of authored face normals in mixed files.
            mesh.vertices.push_back({renderPosition(point), {0, 0, 1}, {}});
        }
        const size_t completed = mesh.indices.size() + mesh.lineIndices.size() + mesh.pointIndices.size();
        if (primitiveVisits++ % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, completed, totalIndices); }
        return mapped;
    };
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const auto base = shape.name.empty() ? "shape " + std::to_string(shapeIndex + 1) : shape.name;
        if (!shape.lines.indices.empty()) {
            MeshNode node;
            node.name = groupName(base + (shape.mesh.indices.empty() ? "" : " (lines)"));
            node.lineIndexOffset = static_cast<uint32_t>(mesh.lineIndices.size());
            size_t begin = 0;
            for (const auto count : shape.lines.num_line_vertices) {
                auto previous = primitiveVertex(shape.lines.indices[begin].position_index);
                for (size_t i = 1; i < static_cast<size_t>(count); ++i) {
                    const auto next = primitiveVertex(shape.lines.indices[begin + i].position_index);
                    mesh.lineIndices.push_back(previous);
                    mesh.lineIndices.push_back(next);
                    previous = next;
                }
                begin += static_cast<size_t>(count);
            }
            node.lineIndexCount = static_cast<uint32_t>(mesh.lineIndices.size()) - node.lineIndexOffset;
            mesh.nodes.push_back(std::move(node));
        }
        if (!shape.points.indices.empty()) {
            MeshNode node;
            node.name = groupName(base + (shape.mesh.indices.empty() && shape.lines.indices.empty() ? "" : " (points)"));
            node.pointIndexOffset = static_cast<uint32_t>(mesh.pointIndices.size());
            for (const auto& index : shape.points.indices) { mesh.pointIndices.push_back(primitiveVertex(index.position_index)); }
            node.pointIndexCount = static_cast<uint32_t>(mesh.pointIndices.size()) - node.pointIndexOffset;
            mesh.nodes.push_back(std::move(node));
        }
    }
    if (vertexCloud && !source->points.empty()) {
        MeshNode node;
        node.name = "Points";
        for (size_t i = 0; i < source->points.size(); ++i) { mesh.pointIndices.push_back(primitiveVertex(static_cast<int>(i))); }
        node.pointIndexCount = static_cast<uint32_t>(mesh.pointIndices.size());
        mesh.nodes.push_back(std::move(node));
    }
    if (empty(mesh) && freeformPatches.empty()) { throw std::runtime_error("OBJ did not contain renderable geometry: " + pathToUtf8(path)); }

    mesh.sourceData = std::move(source);
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices);
    if (!empty(mesh)) { finalizeMesh(mesh, true, progress); }
    if (!freeformPatches.empty()) {
        appendFreeformGeometry(mesh, std::move(freeformPatches), progress);
        finalizeMesh(mesh, false, progress);
    }
    return mesh;
}

} // namespace woby
