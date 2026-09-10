#include "ui_views.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"

#include <algorithm>
#include <imgui.h>
#include <imgui_stdlib.h>

namespace woby {

void drawViews(UiState& state, ViewNameEdit& edit)
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
        const float rows = static_cast<float>(std::clamp(state.views.size(), size_t{1}, size_t{5}));
        const float height = rows * (button + ImGui::GetStyle().ItemSpacing.y) + ImGui::GetStyle().WindowPadding.y;
        if (ImGui::BeginChild("view_rows", ImVec2(0, height))) {
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
                && ImGui::IsKeyPressed(ImGuiKey_F2, false)) { beginRename(state.activeViewId); }
            for (const auto& view : state.views) {
                const auto id = view.id;
                ImGui::PushID(std::to_string(id).c_str());
                const float saveWidth = ImGui::CalcTextSize("Save").x + ImGui::GetStyle().FramePadding.x * 2;
                const float nameWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - saveWidth - button - spacing * 2);
                bool removed = false;
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
                    if (drawSceneItemButton((view.name + "###name").c_str(), nameWidth, state.activeViewId == id)) {
                        applyView(state, id);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Restore this view. Double-click to rename.");
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { beginRename(id); }
                    }
                    if (ImGui::BeginPopupContextItem("view_menu")) {
                        if (ImGui::MenuItem("Restore view")) { applyView(state, id); }
                        if (ImGui::MenuItem("Rename", "F2")) { beginRename(id); }
                        if (ImGui::MenuItem("Save current state to view")) { updateView(state, id); }
                        if (ImGui::MenuItem("Delete view")) { removeView(state, id); removed = true; }
                        ImGui::EndPopup();
                    }
                }
                if (!removed) {
                    ImGui::SameLine();
                    if (ImGui::Button("Save", ImVec2(saveWidth, button))) { updateView(state, id); }
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Replace this checkpoint with the current state. Save the .woby file to keep it on disk."); }
                    ImGui::SameLine();
                    if (drawRemoveButton("delete", "Delete view")) { removeView(state, id); removed = true; }
                }
                ImGui::PopID();
                if (removed) { break; }
            }
        }
        ImGui::EndChild();
    }
    ImGui::PopID();
}

} // namespace woby
