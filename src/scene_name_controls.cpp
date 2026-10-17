#include "scene_name_controls.h"
#include "ui_icon_controls.h"
#include "ui_operations.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>

namespace woby {
void beginSceneNameEdit(SceneNameEdit& edit, const SceneObjectInfo& object, uint64_t generation, bool focus)
{
    edit = {};
    if (!object.originalName) { return; }
    edit.objectId = object.id;
    edit.generation = generation;
    edit.text = edit.initialName = object.name;
    edit.originalName = *object.originalName;
    edit.focus = focus;
}

void synchronizeSceneNameEdit(UiState& state, SceneNameEdit& edit, bool finish)
{
    if (!edit.objectId) { return; }
    if (edit.generation != state.sceneGeneration) { edit = {}; return; }
    const bool selected = state.selectedSceneObjects.size() == 1 && state.selectedSceneObjects.front() == edit.objectId;
    if (!finish && selected && !edit.active && !edit.focus && !edit.invalid && edit.text == edit.initialName) { return; }
    const auto object = findSceneObject(state, edit.objectId);
    if (!object || object->name != edit.initialName) { edit = {}; return; }
    if (finish || !selected) {
        if (edit.text != edit.initialName) { renameSceneObject(state, edit.objectId, edit.text); }
        edit = {};
    }
}

SceneNameInputResult drawSceneNameInput(const char* label, SceneNameEdit& edit, float width)
{
    edit.submittedFrame = ImGui::GetFrameCount();
    const bool focusing = edit.focus;
    if (focusing) { ImGui::SetKeyboardFocusHere(); edit.focus = false; }
    ImGui::SetNextItemWidth(std::max(1.0f, width));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x,
        std::max(0.0f, (renderModeButtonSize() - ImGui::GetTextLineHeight()) * .5f)));
    const bool entered = ImGui::InputText(label, &edit.text,
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    const bool active = ImGui::IsItemActive();
    const bool deactivated = ImGui::IsItemDeactivated();
    const bool canceled = (active || edit.active || deactivated) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    edit.active = active;
    if (ImGui::IsItemEdited()) { edit.invalid = false; }
    if (canceled) { edit.text = edit.initialName; edit.invalid = false; return SceneNameInputResult::cancel; }
    if (entered || (!focusing && deactivated)) {
        if (edit.text == edit.originalName || !normalizedSceneName(edit.text).empty()) {
            edit.invalid = false;
            return SceneNameInputResult::apply;
        }
        if (entered) { edit.invalid = true; edit.focus = true; }
        else { edit.text = edit.initialName; edit.invalid = false; return SceneNameInputResult::cancel; }
    }
    return SceneNameInputResult::editing;
}

void drawSceneNameError(const SceneNameEdit& edit)
{
    if (!edit.invalid) { return; }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, .65f, .55f, 1.0f));
    ImGui::TextWrapped("Enter a name on one line, or restore the original name.");
    ImGui::PopStyleColor();
}
} // namespace woby
