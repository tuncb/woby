#include "model_mesh.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace woby {
namespace {

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
    if (length <= 0.000001f || !std::isfinite(length)) {
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
        return validNormal(vertex.normal);
    });
}

} // namespace

bool empty(const Mesh& mesh) noexcept
{
    return mesh.vertices.empty() || mesh.indices.empty();
}

bool finitePosition(const std::array<float, 3>& position) noexcept
{
    return std::isfinite(position[0])
        && std::isfinite(position[1])
        && std::isfinite(position[2]);
}

bool validNormal(const std::array<float, 3>& normal) noexcept
{
    if (!std::isfinite(normal[0]) || !std::isfinite(normal[1]) || !std::isfinite(normal[2])) {
        return false;
    }

    return std::abs(normal[0]) > 0.000001f
        || std::abs(normal[1]) > 0.000001f
        || std::abs(normal[2]) > 0.000001f;
}

std::array<float, 3> calculateFaceNormal(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c)
{
    auto normal = cross(subtract(b, a), subtract(c, a));
    normalize(normal);
    return normal;
}

void generateSmoothNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    for (auto& vertex : vertices) {
        vertex.normal = {0.0f, 0.0f, 0.0f};
    }

    for (size_t index = 0; index + 2 < indices.size(); index += 3u) {
        Vertex& a = vertices[indices[index + 0u]];
        Vertex& b = vertices[indices[index + 1u]];
        Vertex& c = vertices[indices[index + 2u]];

        const auto faceNormal = calculateFaceNormal(a.position, b.position, c.position);
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
    if (vertices.empty()) {
        throw std::runtime_error("Cannot calculate bounds for an empty mesh.");
    }

    Bounds bounds{};
    bounds.min = vertices.front().position;
    bounds.max = vertices.front().position;

    for (const auto& vertex : vertices) {
        for (size_t axis = 0; axis < 3u; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
    }

    for (size_t axis = 0; axis < 3u; ++axis) {
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

void compactMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
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

void finalizeMesh(Mesh& mesh, bool generateMissingSmoothNormals)
{
    if (empty(mesh)) {
        throw std::runtime_error("Mesh did not contain renderable triangles.");
    }

    if (generateMissingSmoothNormals && !hasCompleteNormals(mesh.vertices)) {
        generateSmoothNormals(mesh.vertices, mesh.indices);
    }

    mesh.bounds = calculateBounds(mesh.vertices);
    compactMesh(mesh.vertices, mesh.indices);
}

} // namespace woby
