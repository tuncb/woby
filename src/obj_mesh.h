#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
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

struct ObjNode {
    std::string name;
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
};

struct ObjMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ObjNode> nodes;
    Bounds bounds;
};

[[nodiscard]] ObjMesh loadObjMesh(const std::filesystem::path& path);
[[nodiscard]] bool empty(const ObjMesh& mesh) noexcept;

} // namespace woby
