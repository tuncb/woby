#pragma once

#include "comparison_settings.h"
#include "mesh_comparison.h"

#include <array>
#include <string>
#include <vector>

namespace woby {

struct ScreenshotSettings {
    int width = 1920;
    int height = 1800;
    bool resultsOnly = false;
    bool legend = true;
    bool comparisonName = true;
    bool sources = true;
    bool direction = true;
    bool tolerance = true;
    friend bool operator==(const ScreenshotSettings&, const ScreenshotSettings&) = default;
};

[[nodiscard]] ScreenshotSettings normalizedScreenshotSettings(ScreenshotSettings settings);
[[nodiscard]] std::string measurementNumber(double value);
// Unlit palette, shared by numeric legends. Matches comparison.frag.sc.
[[nodiscard]] std::array<float, 4> comparisonHeatmapColor(double distance, const ComparisonSettings& settings);
[[nodiscard]] std::vector<std::string> comparisonReportLines(
    const std::string& name, const std::string& a, const std::string& b,
    const ComparisonSettings& settings, const MeshComparison& result, const ScreenshotSettings& options);

} // namespace woby
