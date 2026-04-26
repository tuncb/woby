#include "obj_mesh.h"

#include <meshoptimizer.h>
#include <tiny_obj_loader.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

struct IndexKeyHash {
    size_t operator()(const IndexKey& key) const noexcept
    {
        const auto a = static_cast<uint32_t>(key.vertex + 1);
        const auto b = static_cast<uint32_t>(key.normal + 1);
        const auto c = static_cast<uint32_t>(key.texcoord + 1);
        size_t seed = a;
        seed ^= size_t(b) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        seed ^= size_t(c) + 0x9e3779b9u + (seed << 6u) + (seed >> 2u);
        return seed;
    }
};

std::array<float, 3> subtract(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
}

std::array<float, 3> cross(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    return {
        lhs[1] * rhs[2] - lhs[2] * rhs[1],
        lhs[2] * rhs[0] - lhs[0] * rhs[2],
        lhs[0] * rhs[1] - lhs[1] * rhs[0],
    };
}

void add(std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    lhs[0] += rhs[0];
    lhs[1] += rhs[1];
    lhs[2] += rhs[2];
}

void normalize(std::array<float, 3>& value)
{
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length <= 0.000001f) {
        value = {0.0f, 1.0f, 0.0f};
        return;
    }

    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

bool hasCompleteNormals(const std::vector<Vertex>& vertices)
{
    return std::all_of(vertices.begin(), vertices.end(), [](const Vertex& vertex) {
        return std::abs(vertex.normal[0]) > 0.000001f
            || std::abs(vertex.normal[1]) > 0.000001f
            || std::abs(vertex.normal[2]) > 0.000001f;
    });
}

void generateSmoothNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    for (auto& vertex : vertices) {
        vertex.normal = {0.0f, 0.0f, 0.0f};
    }

    for (size_t index = 0; index + 2 < indices.size(); index += 3) {
        Vertex& a = vertices[indices[index + 0]];
        Vertex& b = vertices[indices[index + 1]];
        Vertex& c = vertices[indices[index + 2]];

        auto ab = subtract(b.position, a.position);
        auto ac = subtract(c.position, a.position);
        auto faceNormal = cross(ab, ac);
        normalize(faceNormal);

        add(a.normal, faceNormal);
        add(b.normal, faceNormal);
        add(c.normal, faceNormal);
    }

    for (auto& vertex : vertices) {
        normalize(vertex.normal);
    }
}

Bounds calculateBounds(const std::vector<Vertex>& vertices)
{
    Bounds bounds{};
    bounds.min = vertices.front().position;
    bounds.max = vertices.front().position;

    for (const auto& vertex : vertices) {
        for (size_t axis = 0; axis < 3; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
    }

    for (size_t axis = 0; axis < 3; ++axis) {
        bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }

    float radiusSquared = 0.0f;
    for (const auto& vertex : vertices) {
        const auto offset = subtract(vertex.position, bounds.center);
        radiusSquared = std::max(radiusSquared, offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
    }

    bounds.radius = std::max(std::sqrt(radiusSquared), 0.001f);
    return bounds;
}

void optimizeMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    if (vertices.empty() || indices.empty()) {
        return;
    }

    static_assert(sizeof(uint32_t) == sizeof(unsigned int));

    std::vector<uint32_t> remap(indices.size());
    const size_t vertexCount = meshopt_generateVertexRemap(
        remap.data(),
        reinterpret_cast<const unsigned int*>(indices.data()),
        indices.size(),
        vertices.data(),
        vertices.size(),
        sizeof(Vertex));

    std::vector<uint32_t> remappedIndices(indices.size());
    std::vector<Vertex> remappedVertices(vertexCount);

    meshopt_remapIndexBuffer(
        reinterpret_cast<unsigned int*>(remappedIndices.data()),
        reinterpret_cast<const unsigned int*>(indices.data()),
        indices.size(),
        remap.data());

    meshopt_remapVertexBuffer(
        remappedVertices.data(),
        vertices.data(),
        vertices.size(),
        sizeof(Vertex),
        remap.data());

    indices = std::move(remappedIndices);
    vertices = std::move(remappedVertices);

    meshopt_optimizeVertexCache(
        reinterpret_cast<unsigned int*>(indices.data()),
        reinterpret_cast<const unsigned int*>(indices.data()),
        indices.size(),
        vertices.size());

    meshopt_optimizeOverdraw(
        reinterpret_cast<unsigned int*>(indices.data()),
        reinterpret_cast<const unsigned int*>(indices.data()),
        indices.size(),
        &vertices.front().position[0],
        vertices.size(),
        sizeof(Vertex),
        1.05f);

    std::vector<Vertex> fetchedVertices(vertices.size());
    meshopt_optimizeVertexFetch(
        fetchedVertices.data(),
        reinterpret_cast<unsigned int*>(indices.data()),
        indices.size(),
        vertices.data(),
        vertices.size(),
        sizeof(Vertex));

    vertices = std::move(fetchedVertices);
}

} // namespace

ObjMesh ObjMesh::load(const std::filesystem::path& path)
{
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;

    if (path.has_parent_path()) {
        config.mtl_search_path = path.parent_path().string();
    }

    tinyobj::ObjReader reader;
    if (!reader.ParseFromFile(path.string(), config)) {
        std::string message = "Failed to load OBJ: " + path.string();
        if (!reader.Error().empty()) {
            message += "\n" + reader.Error();
        }
        throw std::runtime_error(message);
    }

    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();

    ObjMesh mesh;
    std::unordered_map<IndexKey, uint32_t, IndexKeyHash> vertexMap;

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            const IndexKey key{index.vertex_index, index.normal_index, index.texcoord_index};
            const auto found = vertexMap.find(key);
            if (found != vertexMap.end()) {
                mesh.indices_.push_back(found->second);
                continue;
            }

            if (index.vertex_index < 0) {
                throw std::runtime_error("OBJ contains a face vertex without a position index.");
            }

            Vertex vertex{};
            const auto vertexIndex = static_cast<size_t>(index.vertex_index) * 3u;
            vertex.position = {
                attrib.vertices[vertexIndex + 0u],
                attrib.vertices[vertexIndex + 1u],
                attrib.vertices[vertexIndex + 2u],
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

            const uint32_t newIndex = static_cast<uint32_t>(mesh.vertices_.size());
            vertexMap.emplace(key, newIndex);
            mesh.vertices_.push_back(vertex);
            mesh.indices_.push_back(newIndex);
        }
    }

    if (mesh.empty()) {
        throw std::runtime_error("OBJ did not contain renderable triangles: " + path.string());
    }

    if (!hasCompleteNormals(mesh.vertices_)) {
        generateSmoothNormals(mesh.vertices_, mesh.indices_);
    }

    mesh.bounds_ = calculateBounds(mesh.vertices_);
    optimizeMesh(mesh.vertices_, mesh.indices_);
    return mesh;
}

} // namespace woby
