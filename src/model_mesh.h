#pragma once

#include <array>
#include <cstdint>
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

struct MeshNode {
    std::string name;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<MeshNode> nodes;
    Bounds bounds;
};

[[nodiscard]] bool empty(const Mesh& mesh) noexcept;
[[nodiscard]] bool finitePosition(const std::array<float, 3>& position) noexcept;
[[nodiscard]] bool validNormal(const std::array<float, 3>& normal) noexcept;
[[nodiscard]] std::array<float, 3> calculateFaceNormal(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c);
void generateSmoothNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
[[nodiscard]] Bounds calculateBounds(const std::vector<Vertex>& vertices);
void compactMesh(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
void finalizeMesh(Mesh& mesh, bool generateMissingSmoothNormals);

} // namespace woby
