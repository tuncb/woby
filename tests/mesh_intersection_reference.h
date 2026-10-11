#pragma once
#include <array>
#include <cstddef>

namespace intersection_reference {
bool intersects(const std::array<std::array<double, 3>, 3>& a,
    const std::array<std::array<double, 3>, 3>& b,
    const std::array<size_t, 3>& aIds, const std::array<size_t, 3>& bIds);
}
