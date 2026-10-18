#pragma once
#include "scene_pick.h"

namespace woby {
// Text coordinates are measured in line heights, with Y pointing down.
[[nodiscard]] PickMatrix textAnnotationTransform(const TextAnnotationSettings& text, const ScenePickView& view);
[[nodiscard]] std::optional<PickPoint> projectTextAnnotationPoint(const PickMatrix& transform,
    const ScenePickView& view, PickPoint point);
[[nodiscard]] std::optional<std::array<float, 3>> textAnnotationPlanePoint(
    const ScenePickView& view, PickPoint point, const std::array<float, 3>& anchor);
[[nodiscard]] std::optional<std::array<float, 3>> textAnnotationPlacement(
    std::span<const ScenePickPart> parts, const ScenePickView& view, PickPoint point,
    const std::array<float, 3>& fallback);
} // namespace woby
