#pragma once

#include "model_mesh.h"

#include <algorithm>
#include <limits>

namespace woby {

struct UvExtent {
    std::array<double, 2> min{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
    std::array<double, 2> max{-std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
};

inline void includeUv(UvExtent& extent, const std::array<float, 2>& uv)
{
    for (size_t k = 0; k < 2; ++k) {
        extent.min[k] = std::min(extent.min[k], double{uv[k]});
        extent.max[k] = std::max(extent.max[k], double{uv[k]});
    }
}

struct UvLayoutFrame {
    std::array<float, 3> center{};
    std::array<double, 2> uvCenter{};
    double scale = 1;
};

// One uniform display scale preserves island proportions and UV tile offsets.
// The original UVs are retained for grid sampling; this is not an unwrap.
inline UvLayoutFrame uvLayoutFrame(const Bounds& source, const UvExtent& extent)
{
    UvLayoutFrame result;
    result.center = source.center;
    double size = 0, uvSize = 0;
    for (size_t k = 0; k < 3; ++k) { size = std::max(size, double{source.max[k]} - source.min[k]); }
    for (size_t k = 0; k < 2; ++k) {
        result.uvCenter[k] = (extent.min[k] + extent.max[k]) * .5;
        uvSize = std::max(uvSize, extent.max[k] - extent.min[k]);
    }
    if (uvSize > 0) { result.scale = std::max(size, 1e-6) / uvSize; }
    return result;
}

inline std::array<float, 3> uvLayoutPosition(const UvLayoutFrame& frame, const std::array<float, 2>& uv)
{
    // Importers use texture-space V (down); the layout displays V upward.
    return {static_cast<float>(frame.center[0] + (uv[0] - frame.uvCenter[0]) * frame.scale),
        static_cast<float>(frame.center[1] - (uv[1] - frame.uvCenter[1]) * frame.scale), frame.center[2]};
}

[[nodiscard]] Mesh uvLayoutMesh(const Mesh& source);

} // namespace woby
