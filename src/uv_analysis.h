#pragma once

#include "model_mesh.h"
#include "scene_up_axis.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stop_token>

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
    SceneUpAxis upAxis = SceneUpAxis::z;
};

// One uniform display scale preserves island proportions and UV tile offsets.
// The original UVs are retained for grid sampling; this is not an unwrap.
inline UvLayoutFrame uvLayoutFrame(const Bounds& source, const UvExtent& extent, SceneUpAxis upAxis)
{
    UvLayoutFrame result;
    result.center = source.center;
    result.upAxis = upAxis;
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
    auto result = frame.center;
    const size_t vertical = frame.upAxis == SceneUpAxis::y ? 1 : 2;
    result[0] = static_cast<float>(frame.center[0] + (uv[0] - frame.uvCenter[0]) * frame.scale);
    result[vertical] = static_cast<float>(frame.center[vertical] - (uv[1] - frame.uvCenter[1]) * frame.scale);
    return result;
}

struct UvPatchLayout {
    UvLayoutFrame frame;
    std::array<float, 2> offset{};
};

inline UvPatchLayout uvPatchLayout(const UvLayoutFrame& frame, const UvExtent& extent,
    const UvExtent& local, size_t patch, size_t patchCount, bool separated)
{
    UvPatchLayout result{frame, {}};
    if (!separated) { return result; }
    const auto columns = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(patchCount))));
    const auto rows = (patchCount + columns - 1) / columns;
    const double width = (extent.max[0] - extent.min[0]) * frame.scale;
    const double height = (extent.max[1] - extent.min[1]) * frame.scale;
    const double gap = std::max({width, height, 1e-6}) * .15;
    result.frame.uvCenter = {(local.min[0]+local.max[0])*.5, (local.min[1]+local.max[1])*.5};
    result.offset = {
        static_cast<float>((static_cast<double>(patch%columns)-static_cast<double>(columns-1)*.5)*(width+gap)),
        static_cast<float>((static_cast<double>(patch/columns)-static_cast<double>(rows-1)*.5)*(height+gap))};
    return result;
}

inline std::array<float, 3> uvPatchPosition(const UvPatchLayout& patch, const std::array<float, 2>& uv)
{
    auto position = uvLayoutPosition(patch.frame, uv);
    position[0] += patch.offset[0];
    position[patch.frame.upAxis == SceneUpAxis::y ? 1 : 2] += patch.offset[1];
    return position;
}

[[nodiscard]] Mesh uvLayoutMesh(const Mesh& source, SceneUpAxis upAxis, bool separated = false, std::stop_token stop = {});

} // namespace woby
