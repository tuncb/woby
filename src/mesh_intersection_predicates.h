#pragma once

#include <array>
#include <cstddef>
#include <optional>

namespace woby {

// Internal narrow-phase classifier. A missing result requires the rational
// fallback. An engaged false is an exact non-intersection decision.
[[nodiscard]] std::optional<bool> adaptiveTrianglesSelfIntersect(
    const std::array<std::array<double, 3>, 3>& a,
    const std::array<std::array<double, 3>, 3>& b,
    const std::array<size_t, 3>& aIds, const std::array<size_t, 3>& bIds);

} // namespace woby
