#pragma once

#include <algorithm>
#include <array>
#include <cmath>

namespace woby {

enum class UvColorMode { grid, u, v };

inline constexpr float minUvGridDensity = 0.1f;
inline constexpr float maxUvGridDensity = 1000.0f;

// Cells per unit of the supplied UV coordinates. No implicit domain normalization.
struct UvGridSettings {
    bool enabled = false;
    float densityU = 10.0f;
    float densityV = 10.0f;
    UvColorMode mode = UvColorMode::grid;
    float minimum = 0.0f;
    float maximum = 1.0f;
    friend bool operator==(const UvGridSettings&, const UvGridSettings&) = default;
};

[[nodiscard]] inline UvGridSettings normalizedUvGrid(UvGridSettings value)
{
    const auto density = [](float input) {
        return std::isfinite(input) ? std::clamp(input, minUvGridDensity, maxUvGridDensity) : 10.0f;
    };
    value.densityU = density(value.densityU);
    value.densityV = density(value.densityV);
    if (value.mode != UvColorMode::u && value.mode != UvColorMode::v) { value.mode = UvColorMode::grid; }
    if (!std::isfinite(value.minimum)) { value.minimum = 0; }
    if (!std::isfinite(value.maximum) || value.maximum <= value.minimum) {
        value.minimum = 0; value.maximum = 1;
    }
    return value;
}

[[nodiscard]] inline const char* uvColorModeKey(UvColorMode mode)
{
    return mode == UvColorMode::u ? "u" : mode == UvColorMode::v ? "v" : "grid";
}

// Both source and analysis shaders use the same range and mode encoding.
[[nodiscard]] inline std::array<float, 4> uvColorParameters(const UvGridSettings& settings, bool available, bool analysis)
{
    if (!settings.enabled || !available) { return {}; }
    if (settings.mode == UvColorMode::grid) { return {settings.densityU, settings.densityV, analysis ? 3.0f : 1.0f, 0}; }
    return {settings.minimum, settings.maximum, settings.mode == UvColorMode::u ? 4.0f : 5.0f, 0};
}

} // namespace woby
