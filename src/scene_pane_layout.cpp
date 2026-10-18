#include "scene_pane_layout.h"
#include "ui_icon_controls.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace woby {
namespace {
size_t nextOpenSection(const ScenePaneLayout& layout, size_t index)
{
    for (++index; index < scenePaneSectionCount; ++index) {
        if (layout.open[index]) { return index; }
    }
    return scenePaneSectionCount;
}

void updateSectionHeights(ScenePaneLayout& layout)
{
    const auto& style = ImGui::GetStyle();
    const float count = static_cast<float>(std::count(layout.open.begin(), layout.open.end(), true));
    const float headerHeight = std::max(renderModeButtonSize(), ImGui::GetFrameHeight())
        + 2.0f * style.CellPadding.y + style.ItemSpacing.y;
    const float bodyHeight = layout.availableHeight - static_cast<float>(scenePaneSectionCount) * headerHeight
        - count * style.ItemSpacing.y
        - std::max(0.0f, count - 1.0f) * (layout.dividerHeight + style.ItemSpacing.y)
        + style.ItemSpacing.y;
    layout.heights = scenePaneSectionHeights(layout.open, layout.weights, bodyHeight, layout.minimumHeight);
}
} // namespace

ScenePaneHeights scenePaneSectionHeights(const ScenePaneOpen& open,
    const ScenePaneHeights& weights, float availableHeight, float minimumHeight)
{
    ScenePaneHeights result{};
    const float minimum = std::max(1.0f, minimumHeight);
    float totalWeight = 0;
    float count = 0;
    for (size_t i = 0; i < scenePaneSectionCount; ++i) {
        if (!open[i]) { continue; }
        totalWeight += std::max(0.0f, weights[i]);
        ++count;
    }
    const float extra = std::max(0.0f, availableHeight - count * minimum);
    for (size_t i = 0; i < scenePaneSectionCount; ++i) {
        if (!open[i]) { continue; }
        const float share = totalWeight > 0 ? std::max(0.0f, weights[i]) / totalWeight : 1.0f / count;
        result[i] = minimum + extra * share;
    }
    return result;
}

void resizeScenePaneSections(ScenePaneLayout& layout, ScenePaneSection section, float delta)
{
    const auto index = static_cast<size_t>(section);
    if (index >= scenePaneSectionCount || !layout.open[index] || !std::isfinite(delta)) { return; }
    const auto next = nextOpenSection(layout, index);
    if (next == scenePaneSectionCount) { return; }
    const float before = std::max(0.0f, layout.heights[index] - layout.minimumHeight);
    const float after = std::max(0.0f, layout.heights[next] - layout.minimumHeight);
    const float extra = before + after;
    if (extra <= 0) { return; }
    float weight = layout.weights[index] + layout.weights[next];
    if (weight <= 0) {
        // All remaining sections can have zero weight after larger sections close.
        // Restore equal weights before adjusting their automatic equal shares.
        for (size_t i = 0; i < scenePaneSectionCount; ++i) {
            if (layout.open[i]) { layout.weights[i] = 1.0f; }
        }
        weight = 2.0f;
    }
    const float moved = std::clamp(delta, -before, after);
    layout.weights[index] = weight * (before + moved) / extra;
    layout.weights[next] = weight - layout.weights[index];
    layout.heights[index] += moved;
    layout.heights[next] -= moved;
}

void beginScenePaneLayout(ScenePaneLayout& layout)
{
    layout.availableHeight = ImGui::GetContentRegionAvail().y;
    layout.minimumHeight = renderModeButtonSize() + ImGui::GetStyle().WindowPadding.y;
    layout.dividerHeight = uiSize(6.0f);
    updateSectionHeights(layout);
}

bool beginScenePaneSection(ScenePaneLayout& layout, ScenePaneSection section, bool open)
{
    const auto index = static_cast<size_t>(section);
    if (layout.open[index] != open) {
        layout.open[index] = open;
        updateSectionHeights(layout);
    }
    if (!open) { return false; }
    ImGui::PushID(static_cast<int>(index));
    return ImGui::BeginChild("section_rows", {0, layout.heights[index]});
}

void endScenePaneSection(ScenePaneLayout& layout, ScenePaneSection section)
{
    const auto index = static_cast<size_t>(section);
    if (!layout.open[index]) { return; }
    ImGui::EndChild();
    if (nextOpenSection(layout, index) != scenePaneSectionCount) {
        ImGui::InvisibleButton("section_divider", {-1, layout.dividerHeight});
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const auto low = ImGui::GetItemRectMin();
        const auto high = ImGui::GetItemRectMax();
        const float y = (low.y + high.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine({low.x, y}, {high.x, y},
            ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                : hovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator));
        if (hovered || active) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); }
        if (hovered) {
            ImGui::SetTooltip("Drag to resize sections. Double-click to reset all section sizes.");
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { layout.weights.fill(1.0f); }
        }
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            auto resized = layout;
            resizeScenePaneSections(resized, section, ImGui::GetIO().MouseDelta.y);
            // Apply both new heights together on the next frame.
            layout.weights = resized.weights;
        }
    }
    ImGui::PopID();
}

} // namespace woby
