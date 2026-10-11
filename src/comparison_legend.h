#pragma once

#include "comparison_settings.h"
#include <imgui.h>

namespace woby {
enum class ComparisonRangeHandle { none, lower, upper, tolerance };
struct ComparisonRangeDrag {
    ComparisonRangeHandle handle = ComparisonRangeHandle::none;
    float axisMaximum = 0;
    double lastSampleMaximum = -1;
    float mouseStart = 0;
    double valueStart = 0;
};
// Edit local settings. The caller commits changes through ui_operations.
bool drawComparisonRangeControl(ComparisonSettings& settings, ComparisonRangeDrag& drag, double sampleMaximum);
// Draw at the supplied position, returning the height used including numeric labels.
float drawComparisonLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize,
    const ComparisonSettings& settings);
float drawSurfaceQualityLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize,
    SurfaceQualityMetric metric, const QualityDistribution& distribution);
float drawUvQualityLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize, UvQualityMetric metric);
}
