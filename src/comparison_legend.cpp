#include "comparison_legend.h"
#include "comparison_report.h"
#include "uv_quality.h"

#include <algorithm>
#include <cmath>

namespace woby {
float drawUvQualityLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize, UvQualityMetric metric)
{
    for (int i = 0; i < 64; ++i) {
        const double value = metric == UvQualityMetric::orientation ? (i < 32 ? 0.0 : 1.0)
            : metric == UvQualityMetric::overlap ? (i < 21 ? 0.0 : i < 43 ? .5 : 1.0) : static_cast<double>(i) / 63;
        const auto c = uvQualityColor(value, metric == UvQualityMetric::area);
        draw.AddRectFilled({position.x + width * static_cast<float>(i) / 64, position.y},
            {position.x + width * static_cast<float>(i + 1) / 64, position.y + fontSize},
            ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 1}));
    }
    const char* left = metric == UvQualityMetric::angle ? "0 deg" : metric == UvQualityMetric::area ? "<= 1/8"
        : metric == UvQualityMetric::anisotropy ? "1" : metric == UvQualityMetric::minStretch ? ">= 1"
        : metric == UvQualityMetric::overlap ? "None" : "Positive";
    const char* right = metric == UvQualityMetric::angle ? ">= 90 deg" : metric == UvQualityMetric::area ? ">= 8"
        : metric == UvQualityMetric::anisotropy ? ">= 64" : metric == UvQualityMetric::minStretch ? "<= 0.01"
        : metric == UvQualityMetric::overlap ? "Within patch" : "Negative";
    draw.AddText(ImGui::GetFont(), fontSize, {position.x, position.y + fontSize + 4}, IM_COL32_WHITE, left);
    const float rightWidth = ImGui::GetFont()->CalcTextSizeA(fontSize, 10000, 0, right).x;
    draw.AddText(ImGui::GetFont(), fontSize, {position.x + std::max(0.0f, width - rightWidth), position.y + fontSize + 4},
        IM_COL32_WHITE, right);
    return 2 * fontSize + 10;
}

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
    const float grayWidth = collapsedRange ? std::max(0.0f, width - 20)
        : width * std::clamp((settings.tolerance - settings.colorMinimum) / (settings.colorRange - settings.colorMinimum), 0.0f, 1.0f);
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
    label(position.x, position.y + barHeight + 4, measurementNumber(settings.colorMinimum));
    // Stagger endpoint labels to avoid collisions in narrow inspectors.
    label(start, position.y + barHeight + fontSize + 7, "Tolerance " + measurementNumber(settings.tolerance));
    const float middle = position.x + (collapsedRange ? grayWidth : width) * .5f;
    if (width >= 350) {
        label(middle, position.y + barHeight + 4, measurementNumber((settings.colorRange + settings.colorMinimum) * .5));
    }
    label(position.x + width, position.y + barHeight + 4,
        std::string(settings.colorRange == settings.tolerance ? "> " : ">= ") + measurementNumber(settings.colorRange));
    for (const float x : {position.x, start, middle, position.x + width}) {
        draw.AddLine({x, position.y + barHeight}, {x, position.y + barHeight + 3}, textColor);
    }
    return barHeight + 2 * fontSize + 12;
}

bool drawComparisonRangeControl(ComparisonSettings& settings, ComparisonRangeDrag& drag, double sampleMaximum)
{
    const auto before = settings;
    const float font = ImGui::GetFontSize();
    const float padding = font * .5f;
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float trackWidth = std::max(1.0f, width - 2 * padding);
    const auto origin = ImGui::GetCursorScreenPos();
    const float left = origin.x + padding;
    const float top = origin.y + font * .8f;
    const float diamondRadius = font * .28f;
    ImGui::InvisibleButton("distance_range", {width, font * 6.2f});
    const bool active = ImGui::IsItemActive();
    const bool activated = ImGui::IsItemActivated();
    const double measuredMaximum = std::isfinite(sampleMaximum) ? std::max(0.0, sampleMaximum) : 0.0;
    if (drag.axisMaximum <= 0 || ((!active || activated) && measuredMaximum != drag.lastSampleMaximum)) {
        // The axis covers the measured data, not the selected color interval.
        // Keep five percent headroom and fit a different dataset after a gesture ends.
        drag.axisMaximum = std::max(1e-6f, fittedComparisonColorRange(std::max(measuredMaximum,
            static_cast<double>(settings.colorRange)) * 1.05, settings.tolerance));
        drag.lastSampleMaximum = measuredMaximum;
    } else if (!active && settings.colorRange > drag.axisMaximum) {
        // Also keep deliberately extended custom limits visible.
        drag.axisMaximum = fittedComparisonColorRange(static_cast<double>(settings.colorRange) * 1.05, settings.tolerance);
    }
    const auto x = [&](double value) { return left + trackWidth * static_cast<float>(value / drag.axisMaximum); };
    auto& io = ImGui::GetIO();
    const bool onTolerance = std::abs(io.MousePos.x - x(settings.tolerance))
        + std::abs(io.MousePos.y - (top + font * .5f)) <= diamondRadius + 2;
    if (activated) {
        drag.handle = ComparisonRangeHandle::none;
        const bool onTrack = io.MousePos.x >= left - padding && io.MousePos.x <= left + trackWidth + padding
            && io.MousePos.y >= origin.y && io.MousePos.y <= top + font + 3;
        if (onTolerance) { drag.handle = ComparisonRangeHandle::tolerance; }
        else if (onTrack) {
            drag.handle = std::abs(io.MousePos.x - x(settings.colorMinimum)) <= std::abs(io.MousePos.x - x(settings.colorRange))
                ? ComparisonRangeHandle::lower : ComparisonRangeHandle::upper;
        }
        drag.mouseStart = io.MousePos.x;
        drag.valueStart = drag.handle == ComparisonRangeHandle::tolerance ? settings.tolerance
            : drag.handle == ComparisonRangeHandle::lower ? settings.colorMinimum : settings.colorRange;
        if (drag.handle != ComparisonRangeHandle::tolerance && std::abs(io.MousePos.x - x(drag.valueStart)) > padding) {
            drag.valueStart = static_cast<double>(io.MousePos.x - left) / trackWidth * drag.axisMaximum;
        }
    }
    if (active && drag.handle != ComparisonRangeHandle::none) {
        const double delta = static_cast<double>(io.MousePos.x - drag.mouseStart) / trackWidth * drag.axisMaximum;
        const double value = drag.valueStart + delta * (io.KeyShift ? .1 : 1.0);
        if (drag.handle == ComparisonRangeHandle::tolerance) {
            settings.tolerance = static_cast<float>(std::clamp(value, 0.0, std::min(1e12, static_cast<double>(drag.axisMaximum))));
        } else { settings = editedComparisonColorLimit(settings, drag.handle == ComparisonRangeHandle::lower, value); }
    }
    auto& draw = *ImGui::GetWindowDrawList();
    for (int i = 0; i < 128; ++i) {
        const double value = static_cast<double>(drag.axisMaximum) * (i + .5) / 128;
        const auto c = comparisonHeatmapColor(value, settings);
        draw.AddRectFilled({left + trackWidth * static_cast<float>(i) / 128, top},
            {left + trackWidth * static_cast<float>(i + 1) / 128, top + font},
            ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 1}));
    }
    for (const bool lower : {true, false}) {
        const float handle = std::clamp(x(lower ? settings.colorMinimum : settings.colorRange), left, left + trackWidth);
        const bool selected = drag.handle == (lower ? ComparisonRangeHandle::lower : ComparisonRangeHandle::upper);
        const auto color = ImGui::GetColorU32(active && selected ? ImGuiCol_SliderGrabActive : ImGuiCol_Text);
        draw.AddTriangleFilled({handle - padding, top - font * .65f}, {handle + padding, top - font * .65f}, {handle, top}, color);
        draw.AddLine({handle, top}, {handle, top + font + 3}, color, 2);
    }
    // A diamond and dashed line distinguish the acceptance threshold from the range handles.
    const auto toleranceColor = IM_COL32(71, 198, 224, 255);
    const float toleranceX = std::clamp(x(settings.tolerance), left, left + trackWidth);
    for (float y = top; y < top + font; y += 5) {
        const float end = std::min(y + 3, top + font);
        draw.AddLine({toleranceX, y}, {toleranceX, end}, IM_COL32(20, 25, 30, 255), 4);
        draw.AddLine({toleranceX, y}, {toleranceX, end}, toleranceColor, 2);
    }
    const auto diamond = [&](ImVec2 center) {
        const float radius = diamondRadius;
        draw.AddQuadFilled({center.x, center.y - radius}, {center.x + radius, center.y},
            {center.x, center.y + radius}, {center.x - radius, center.y}, toleranceColor);
    };
    diamond({toleranceX, top + font * .5f});
    const auto low = measurementNumber(settings.colorMinimum);
    const auto high = measurementNumber(settings.colorRange);
    const auto axisMaximum = measurementNumber(drag.axisMaximum);
    draw.AddText({left, top + font + 5}, ImGui::GetColorU32(ImGuiCol_Text), "0");
    draw.AddText({left + trackWidth - ImGui::CalcTextSize(axisMaximum.c_str()).x, top + font + 5},
        ImGui::GetColorU32(ImGuiCol_Text), axisMaximum.c_str());
    const auto tolerance = measurementNumber(settings.tolerance);
    const auto limits = "Color limits: " + low + " - " + high;
    draw.AddText({left, top + 2 * font + 9}, ImGui::GetColorU32(ImGuiCol_Text), limits.c_str());
    const float captionY = top + 3 * font + 13;
    diamond({left + font * .3f, captionY + font * .5f});
    draw.AddText({left + font, captionY}, ImGui::GetColorU32(ImGuiCol_Text), tolerance.c_str());
    if (ImGui::IsItemHovered() || active) {
        ImGui::SetTooltip("Axis: 0 - %s\nLargest sampled distance: %s\nLower color limit: %s\nUpper color limit: %s\nAcceptance tolerance: %s (diamond)\nDrag the diamond to change acceptance tolerance.\nOther bar clicks and the triangle handles change only the color limits.\nThe upper limit cannot go below the lower limit.\nHold Shift for fine adjustment. Drag beyond the bar to extend the range.",
            axisMaximum.c_str(), measurementNumber(measuredMaximum).c_str(), low.c_str(), high.c_str(), tolerance.c_str());
    }
    return settings != before;
}
} // namespace woby
