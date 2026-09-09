#include "comparison_legend.h"
#include "comparison_report.h"

#include <algorithm>

namespace woby {
float drawSurfaceQualityLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize,
    SurfaceQualityMetric metric, const QualityDistribution& distribution)
{
    for (int i = 0; i < 64; ++i) {
        const auto c = surfaceQualityColor(static_cast<double>(i) / 63, metric);
        draw.AddRectFilled({position.x + width * static_cast<float>(i) / 64, position.y},
            {position.x + width * static_cast<float>(i + 1) / 64, position.y + fontSize},
            ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 1}));
    }
    const auto left = measurementNumber(distribution.minimum);
    const auto right = measurementNumber(distribution.maximum);
    draw.AddText(ImGui::GetFont(), fontSize, {position.x, position.y + fontSize + 4}, IM_COL32_WHITE, left.c_str());
    const float rightWidth = ImGui::GetFont()->CalcTextSizeA(fontSize, 10000, 0, right.c_str()).x;
    draw.AddText(ImGui::GetFont(), fontSize, {position.x + std::max(0.0f, width - rightWidth), position.y + fontSize + 4},
        IM_COL32_WHITE, right.c_str());
    return 2 * fontSize + 10;
}
float drawComparisonLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize,
    const ComparisonSettings& settings)
{
    const auto textColor = IM_COL32(235, 239, 245, 255);
    const float barHeight = fontSize;
    const bool collapsedRange = settings.colorRange == settings.tolerance;
    const float grayWidth = collapsedRange ? width - 20 : width * settings.tolerance / settings.colorRange;
    const auto gray = comparisonHeatmapColor(0, settings);
    draw.AddRectFilled(position, {position.x + grayWidth, position.y + barHeight},
        ImGui::ColorConvertFloat4ToU32({gray[0], gray[1], gray[2], 1}));
    const float start = position.x + grayWidth + (collapsedRange ? 3.0f : 0.0f);
    if (collapsedRange) {
        // With no ramp interval, show a separate swatch for values above tolerance.
        draw.AddRectFilled({start, position.y}, {position.x + width, position.y + barHeight}, IM_COL32(240, 56, 14, 255));
    } else {
        draw.AddRectFilledMultiColor({start, position.y}, {position.x + width, position.y + barHeight},
            IM_COL32(255, 196, 79, 255), IM_COL32(240, 56, 14, 255),
            IM_COL32(240, 56, 14, 255), IM_COL32(255, 196, 79, 255));
    }
    const auto label = [&](float x, float y, const std::string& text) {
        const auto size = ImGui::GetFont()->CalcTextSizeA(fontSize, 10000, 0, text.c_str());
        draw.AddText(ImGui::GetFont(), fontSize,
            {std::clamp(x - size.x * .5f, position.x, std::max(position.x, position.x + width - size.x)), y}, textColor, text.c_str());
    };
    label(position.x, position.y + barHeight + 4, "0");
    // Stagger endpoint labels to avoid collisions in narrow inspectors.
    label(start, position.y + barHeight + fontSize + 7, "Tolerance " + measurementNumber(settings.tolerance));
    const float middle = position.x + (collapsedRange ? grayWidth : width) * .5f;
    if (width >= 350) {
        label(middle, position.y + barHeight + 4, measurementNumber(settings.colorRange * .5));
    }
    label(position.x + width, position.y + barHeight + 4,
        std::string(settings.colorRange == settings.tolerance ? "> " : ">= ") + measurementNumber(settings.colorRange));
    for (const float x : {position.x, start, middle, position.x + width}) {
        draw.AddLine({x, position.y + barHeight}, {x, position.y + barHeight + 3}, textColor);
    }
    return barHeight + 2 * fontSize + 12;
}
} // namespace woby
