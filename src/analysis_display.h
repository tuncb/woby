#pragma once
#include "model_mesh.h"
#include <cstddef>

namespace woby {
struct SurfaceQualityFace {
    std::array<float, 4> values{};
    std::array<float, 3> normal{};
    uint32_t reserved = 0;
};
struct UvQualityFace {
    std::array<float, 2> value{};
    std::array<std::array<float, 3>, 3> normals{};
    uint32_t reserved = 0;
};
struct DistanceFace {
    std::array<float, 4> distances{};
    std::array<std::array<float, 3>, 3> midpoints{};
    std::array<std::array<float, 3>, 4> normals{};
    std::array<uint32_t, 3> reserved{};
};
static_assert(sizeof(SurfaceQualityFace) == 32 && offsetof(SurfaceQualityFace, normal) == 16);
static_assert(sizeof(UvQualityFace) == 48 && offsetof(UvQualityFace, normals) == 8);
static_assert(sizeof(DistanceFace) == 112 && offsetof(DistanceFace, midpoints) == 16 && offsetof(DistanceFace, normals) == 52);
// The same fixed subdivision is used by the CPU distance calculation and shader.
inline constexpr std::array<uint32_t, 12> distanceCorners{0,3,5, 3,1,4, 5,4,2, 3,4,5};
[[nodiscard]] inline Vertex distanceDisplayVertex(const Mesh& source, const std::vector<DistanceFace>& faces, size_t corner)
{
    const auto face = corner / 12, local = corner % 12;
    const auto& record = faces.at(face);
    const auto point = distanceCorners[local];
    const auto position = point < 3 ? meshGeometry(source).vertices.at(meshGeometry(source).indices.at(face*3+point)).position
        : record.midpoints[point-3];
    return {position, record.normals[local/3], {record.distances[local/3], 0}};
}
} // namespace woby
