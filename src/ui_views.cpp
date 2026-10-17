#include "ui_views.h"
#include "ui_bulk_controls.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace woby {

float viewListHeight(float preferredHeight, float automaticHeight,
    float minimumHeight, float availableHeight)
{
    const float maximum = std::max(1.0f, availableHeight);
    return std::clamp(preferredHeight > 0.0f ? preferredHeight : automaticHeight,
        std::min(minimumHeight, maximum), maximum);
}

void drawViews(UiState& state, ViewNameEdit& edit, ViewListLayout& layout, float reservedHeight, bool flowing)
{
    if (edit.generation != state.sceneGeneration || (edit.id && !findView(state, edit.id))) {
        edit = {};
        edit.generation = state.sceneGeneration;
    }
    const float button = renderModeButtonSize();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    ImGui::PushID("views");
    bool open = false;
    if (ImGui::BeginTable("header", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("add", ImGuiTableColumnFlags_WidthFixed, button);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        open = ImGui::CollapsingHeader("Views", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::TableNextColumn();
        if (drawRenderModeIconButton("add", "+", "Create a view from the current scene", RenderModeState::off, false)) {
            createView(state);
        }
        ImGui::EndTable();
    }
    if (open) {
        const float rowCount = static_cast<float>(std::clamp(state.views.size(), size_t{1}, size_t{5}));
        const float automaticHeight = rowCount * (button + ImGui::GetStyle().ItemSpacing.y)
            + ImGui::GetStyle().WindowPadding.y;
        constexpr float dividerHeight = 8.0f;
        const float minimumHeight = button + ImGui::GetStyle().ItemSpacing.y
            + ImGui::GetStyle().WindowPadding.y;
        const float availableHeight = ImGui::GetContentRegionAvail().y - reservedHeight
            - dividerHeight - ImGui::GetStyle().ItemSpacing.y;
        const float height = flowing ? automaticHeight : viewListHeight(layout.preferredHeight, automaticHeight,
            minimumHeight, availableHeight);
        if (ImGui::BeginChild("view_rows", ImVec2(0, height))) {
            static UiSelectionRows rows;
            beginSelectionRows(rows, state);
            bool duplicate = false, remove = false, selectAll = false;
            if (state.views.empty()) { ImGui::TextDisabled("Use + to save the current view."); }
            const auto beginRename = [&](ViewId id) {
                if (const auto* view = findView(state, id)) {
                    edit.id = id;
                    edit.text = view->name;
                    edit.focus = true;
                }
            };
            if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive()
                && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)
                && state.viewSelection.ids.size() == 1
                && ImGui::IsKeyPressed(ImGuiKey_F2, false)) { beginRename(state.viewSelection.ids.front()); }
            for (const auto& view : state.views) {
                const auto id = view.id;
                const auto order = registerSelectionRow(rows, id);
                ImGui::PushID(std::to_string(id).c_str());
                const float nameWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - button - spacing);
                if (edit.id == id) {
                    const bool focusing = edit.focus;
                    if (focusing) { ImGui::SetKeyboardFocusHere(); edit.focus = false; }
                    ImGui::SetNextItemWidth(nameWidth);
                    const bool entered = ImGui::InputText("##name", &edit.text,
                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { edit.id = 0; }
                    else if (entered || (!focusing && ImGui::IsItemDeactivated())) {
                        renameView(state, id, edit.text);
                        edit.id = 0;
                    }
                } else {
                    if (drawSceneItemButton((view.name + "###name").c_str(), nameWidth, selectionContains(state.viewSelection.ids, id))) {
                        selectViewRow(state, id, order, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
                        if (!ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) { applyView(state, id); }
                    }
                    if (state.activeViewId == id) { drawSceneItemOutline(); }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Click to restore. Ctrl-click to select; Shift-click to select a range. Double-click to rename.");
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) { beginRename(id); }
                    }
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectViewRow(state, id, order, false, false, true); }
                    if (ImGui::BeginPopupContextItem("view_menu")) {
                        const auto count = state.viewSelection.ids.size();
                        if (count > 1) { drawBulkMenuHeader(count, "views"); }
                        if (ImGui::MenuItem("Restore view", nullptr, false, count == 1)) { applyView(state, id); }
                        if (ImGui::MenuItem("Save current state to view", nullptr, false, count == 1)) { updateView(state, id); }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Rename", "F2", false, count == 1)) { beginRename(id); }
                        if (ImGui::MenuItem(bulkMenuLabel("Duplicate", "Duplicate", count, "views").c_str())) { duplicate = true; }
                        if (ImGui::MenuItem(bulkMenuLabel("Delete view", "Delete", count, "views").c_str(), "Del")) { remove = true; }
                        selectAll = drawSelectAllMenu();
                        ImGui::EndPopup();
                    }
                }
                if (!duplicate && !remove) {
                    ImGui::SameLine();
                    if (drawRenderModeIconButton("save", "\xef\x83\x87",
                        "Save current state to this view. Save the .woby file to keep it on disk.",
                        RenderModeState::off, false)) { updateView(state, id); }
                }
                ImGui::PopID();
            }
            const auto shortcuts = endSelectionRows(rows);
            selectAll = shortcuts.selectAll || selectAll;
            remove = shortcuts.remove || remove;
            if (selectAll) { selectViewRows(state, rows.current); }
            if (duplicate) { duplicateSelectedViews(state); }
            else if (remove) { deleteSelectedViews(state); }
        }
        ImGui::EndChild();
        if (!flowing) {
            ImGui::InvisibleButton("views_objects_divider", ImVec2(-1.0f, dividerHeight));
            const bool hovered = ImGui::IsItemHovered();
            const bool active = ImGui::IsItemActive();
            const ImVec2 dividerMin = ImGui::GetItemRectMin();
            const ImVec2 dividerMax = ImGui::GetItemRectMax();
            const float dividerY = (dividerMin.y + dividerMax.y) * 0.5f;
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(dividerMin.x, dividerY), ImVec2(dividerMax.x, dividerY),
                ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                    : hovered ? ImGuiCol_SeparatorHovered : ImGuiCol_Separator));
            if (hovered || active) { ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS); }
            if (hovered) {
                ImGui::SetTooltip("Drag to resize Views and Objects. Double-click to reset.");
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    layout.preferredHeight = 0.0f;
                }
            }
            if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                layout.preferredHeight = viewListHeight(height + ImGui::GetIO().MouseDelta.y,
                    automaticHeight, minimumHeight, availableHeight);
            }
        }
    }
    ImGui::PopID();
}

} // namespace woby
