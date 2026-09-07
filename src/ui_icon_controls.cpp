#include "ui_icon_controls.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace woby {

constexpr const char* visibleIcon = "\xef\x81\xae";
constexpr const char* hiddenIcon = "\xef\x81\xb0";
constexpr const char* mixedStateIcon = "\xef\x81\xa8";

float uiSize(float value) { return value * ImGui::GetStyle().FontScaleMain; }
float renderModeButtonSize() { return uiSize(28.0f); }

RenderModeState renderModeState(size_t enabledCount, size_t totalCount)
{
    if (totalCount > 0u && enabledCount == totalCount) {
        return RenderModeState::on;
    }
    if (enabledCount > 0u && enabledCount < totalCount) {
        return RenderModeState::mixed;
    }
    return RenderModeState::off;
}

void pushRenderModeButtonColors(RenderModeState state)
{
    const ImVec4 buttonColor = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    const ImVec4 activeColor = ImGui::GetStyleColorVec4(ImGuiCol_Header);
    const ImVec4 activeHoveredColor = ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered);
    const ImVec4 mixedColor(0.34f, 0.35f, 0.37f, 1.0f);
    const ImVec4 mixedHoveredColor(0.42f, 0.44f, 0.47f, 1.0f);
    const ImVec4 offTextColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec4 onTextColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    if (state == RenderModeState::on) {
        ImGui::PushStyleColor(ImGuiCol_Button, activeColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, activeHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, activeHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_Text, onTextColor);
        return;
    }
    if (state == RenderModeState::mixed) {
        ImGui::PushStyleColor(ImGuiCol_Button, mixedColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixedHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixedHoveredColor);
        ImGui::PushStyleColor(ImGuiCol_Text, onTextColor);
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, buttonColor);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    ImGui::PushStyleColor(ImGuiCol_Text, offTextColor);
}

void drawMixedRenderModeMark()
{
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    const float fontSize = ImGui::GetFontSize() * 0.72f;
    const ImVec2 textSize = ImGui::CalcTextSize(mixedStateIcon);
    const ImVec2 position(
        buttonMax.x - textSize.x - 4.0f,
        buttonMax.y - fontSize - 3.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImGui::GetFont(),
        fontSize,
        position,
        ImGui::GetColorU32(ImGuiCol_Text),
        mixedStateIcon);
}

void drawEnabledOutline()
{
    const auto low = ImGui::GetItemRectMin();
    const auto high = ImGui::GetItemRectMax();
    const auto color = ImGui::GetColorU32(ImGuiCol_Text);
    const float unit = uiSize(1.0f);
    auto* draw = ImGui::GetWindowDrawList();
    draw->AddRect(low, high, color, 3 * unit);
}

bool drawRenderModeIconButton(
    const char* id,
    const char* icon,
    const char* tooltip,
    RenderModeState state,
    bool disabled)
{
    const std::string label = std::string(icon) + "##" + id;

    if (disabled) {
        ImGui::BeginDisabled();
    }
    pushRenderModeButtonColors(state);
    const bool changed = ImGui::Button(
        label.c_str(),
        ImVec2(renderModeButtonSize(), renderModeButtonSize()));
    ImGui::PopStyleColor(4);
    if (state == RenderModeState::on) { drawEnabledOutline(); }
    if (state == RenderModeState::mixed) {
        drawMixedRenderModeMark();
    }
    if (disabled) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    return changed;
}

float informationIconSize() { return uiSize(20.0f); }

void drawInformationIcon(const char* id, const char* title, const char* text)
{
    ImGui::PushID(id);
    const float size = informationIconSize();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - size));
    const ImVec2 position = ImGui::GetCursorScreenPos();
    ImGui::PushFont(ImGui::GetFont(), ImGui::GetFontSize() * 0.85f);
    constexpr const char* icon = "\xef\x81\x9a";
    const ImVec2 glyphSize = ImGui::CalcTextSize(icon);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(position.x + (size - glyphSize.x) * 0.5f, position.y + (size - glyphSize.y) * 0.5f),
        ImGui::GetColorU32(ImGuiCol_Text), icon);
    ImGui::PopFont();
    // A passive item supplies hover bounds without button visuals or click/focus behavior.
    ImGui::Dummy(ImVec2(size, size));
    const auto& style = ImGui::GetStyle();
    const auto* viewport = ImGui::GetMainViewport();
    if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        // Let the tooltip fit the entire explanation; no fixed-height scrolling hint.
        ImGui::PushTextWrapPos(std::max(1.0f,
            std::min(ImGui::GetFontSize() * 32.0f, viewport->WorkSize.x - 24.0f - style.WindowPadding.x * 2.0f)));
        ImGui::TextUnformatted(title);
        ImGui::Separator();
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
}

bool drawInformationHeader(const char* label, const char* title, const char* text)
{
    bool open = false;
    // Separate columns keep the passive hint out of the header's toggle hit area.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
    if (ImGui::BeginTable(label, 2, ImGuiTableFlags_NoSavedSettings
            | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_NoPadInnerX)) {
        ImGui::TableSetupColumn("section", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("information", ImGuiTableColumnFlags_WidthFixed,
            informationIconSize() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);
        const float headerHeight = ImGui::GetItemRectSize().y;
        ImGui::TableNextColumn();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (headerHeight - informationIconSize()) * 0.5f));
        drawInformationIcon("info", title, text);
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    return open;
}

bool drawTriStateMasterIconButton(
    const char* id,
    const char* icon,
    const char* label,
    size_t enabledCount,
    size_t totalCount)
{
    const RenderModeState state = renderModeState(enabledCount, totalCount);
    const std::string tooltip = std::string(label)
        + " enabled for "
        + std::to_string(enabledCount)
        + " of "
        + std::to_string(totalCount)
        + " groups";

    return drawRenderModeIconButton(
        id,
        icon,
        tooltip.c_str(),
        state,
        totalCount == 0u);
}

bool drawVisibilityIconButton(
    const char* id,
    RenderModeState state,
    const char* tooltip,
    bool disabled)
{
    const char* icon = state == RenderModeState::off ? hiddenIcon : visibleIcon;
    const std::string label = std::string("##") + id;

    if (disabled) {
        ImGui::BeginDisabled();
    }
    pushRenderModeButtonColors(state);
    const bool changed = ImGui::Button(
        label.c_str(),
        ImVec2(renderModeButtonSize(), renderModeButtonSize()));
    const ImVec2 buttonMin = ImGui::GetItemRectMin();
    const ImVec2 buttonMax = ImGui::GetItemRectMax();
    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    const ImVec2 iconPosition(
        std::floor(buttonMin.x + (buttonMax.x - buttonMin.x - iconSize.x) * 0.5f - 1.0f),
        std::floor(buttonMin.y + (buttonMax.y - buttonMin.y - iconSize.y) * 0.5f));
    ImGui::GetWindowDrawList()->AddText(
        iconPosition,
        ImGui::GetColorU32(ImGuiCol_Text),
        icon);
    ImGui::PopStyleColor(4);
    if (state == RenderModeState::on) { drawEnabledOutline(); }
    if (state == RenderModeState::mixed) {
        drawMixedRenderModeMark();
    }
    if (disabled) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }

    return changed;
}

bool drawTriStateVisibilityButton(
    const char* id,
    const char* label,
    size_t visibleCount,
    size_t totalCount)
{
    const RenderModeState state = renderModeState(visibleCount, totalCount);
    const std::string tooltip = std::string(label)
        + " visibility: "
        + std::to_string(visibleCount)
        + " of "
        + std::to_string(totalCount)
        + " groups shown";

    return drawVisibilityIconButton(
        id,
        state,
        tooltip.c_str(),
        totalCount == 0u);
}

bool drawVisibilityButton(const char* id, bool visible, const char* itemName)
{
    const std::string tooltip = std::string(visible ? "Hide " : "Show ")
        + itemName;

    return drawVisibilityIconButton(
        id,
        visible ? RenderModeState::on : RenderModeState::off,
        tooltip.c_str(),
        false);
}

bool drawVisibilityField(const char* label, bool& visible, bool mixed)
{
    ImGui::PushID(label);
    ImGui::BeginGroup();
    const std::string tooltip = mixed
        ? std::string(label) + ": mixed visibility (click to show all)"
        : std::string(visible ? "Hide " : "Show ") + label;
    const bool changed = drawVisibilityIconButton("visible",
        mixed ? RenderModeState::mixed : visible ? RenderModeState::on : RenderModeState::off,
        tooltip.c_str(), false);
    if (changed) { visible = mixed || !visible; }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (mixed) { ImGui::SameLine(); ImGui::TextDisabled("Mixed (click to show all)"); }
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

bool drawRemoveButton(const char* id, const char* tooltip)
{
    return drawRenderModeIconButton(id, "\xef\x80\x8d", tooltip, RenderModeState::off, false);
}

bool drawResetIconButton(const char* id, const char* tooltip)
{
    const std::string label = std::string("\xef\x83\xa2##") + id;
    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button(label.c_str(), ImVec2(size, size));
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", tooltip); }
    return pressed;
}

bool drawSceneItemButton(const char* label, float width, bool selected)
{
    ImGui::PushStyleColor(ImGuiCol_Button,
        selected ? ImGui::GetStyleColorVec4(ImGuiCol_Header) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool pressed = ImGui::Button(label, ImVec2(std::max(1.0f, width), renderModeButtonSize()));
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    return pressed;
}

void drawSceneItemOutline()
{
    auto* draw = ImGui::GetWindowDrawList();
    auto low = ImGui::GetItemRectMin();
    auto high = ImGui::GetItemRectMax();
    // Tree rows span the window, but file names are clipped before the delete
    // column. Keep the outline (including its antialiasing) inside that clip.
    const auto clipLow = draw->GetClipRectMin();
    const auto clipHigh = draw->GetClipRectMax();
    low.x = std::max(low.x, clipLow.x + 1.0f);
    high.x = std::min(high.x, clipHigh.x - 1.0f);
    if (high.x > low.x) {
        draw->AddRect(low, high, ImGui::GetColorU32(ImGuiCol_Text), 2.0f);
    }
}

} // namespace woby
