#pragma once

#include <algorithm>
#include <cmath>

namespace woby {

inline constexpr float minUvGridDensity = 0.1f;
inline constexpr float maxUvGridDensity = 1000.0f;

// Cells per unit of the supplied UV coordinates. No implicit domain normalization.
struct UvGridSettings {
    bool enabled = false;
    float densityU = 10.0f;
    float densityV = 10.0f;
    friend bool operator==(const UvGridSettings&, const UvGridSettings&) = default;
};

[[nodiscard]] inline UvGridSettings normalizedUvGrid(UvGridSettings value)
{
    const auto density = [](float input) {
        return std::isfinite(input) ? std::clamp(input, minUvGridDensity, maxUvGridDensity) : 10.0f;
    };
    value.densityU = density(value.densityU);
    value.densityV = density(value.densityV);
    return value;
}

} // namespace woby
