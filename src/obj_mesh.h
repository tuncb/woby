#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
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

class ObjMesh {
public:
    static ObjMesh load(const std::filesystem::path& path);

    [[nodiscard]] const std::vector<Vertex>& vertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<uint32_t>& indices() const noexcept { return indices_; }
    [[nodiscard]] const Bounds& bounds() const noexcept { return bounds_; }
    [[nodiscard]] bool empty() const noexcept { return vertices_.empty() || indices_.empty(); }

private:
    std::vector<Vertex> vertices_;
    std::vector<uint32_t> indices_;
    Bounds bounds_;
};

} // namespace woby
