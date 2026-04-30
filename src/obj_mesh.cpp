#include "obj_mesh.h"

#include <tiny_obj_loader.h>

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

} // namespace

Mesh loadObjMesh(const std::filesystem::path& path)
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

    Mesh mesh;
    std::unordered_map<IndexKey, uint32_t, IndexKeyHash> vertexMap;

    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const uint32_t nodeIndexOffset = static_cast<uint32_t>(mesh.indices.size());

        for (const auto& index : shape.mesh.indices) {
            const IndexKey key{index.vertex_index, index.normal_index, index.texcoord_index};
            const auto found = vertexMap.find(key);
            if (found != vertexMap.end()) {
                mesh.indices.push_back(found->second);
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

            const uint32_t newIndex = static_cast<uint32_t>(mesh.vertices.size());
            vertexMap.emplace(key, newIndex);
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
        throw std::runtime_error("OBJ did not contain renderable triangles: " + path.string());
    }

    finalizeMesh(mesh, true);
    return mesh;
}

} // namespace woby
