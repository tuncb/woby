#pragma once

#include "ui_icon_controls.h"

#include <cstdint>

#include <imgui.h>
#include <imgui_internal.h>

namespace woby {

enum class SceneHistoryCommand { none, undo, redo };

inline SceneHistoryCommand drawSceneHistoryToolbar(bool canUndo, bool canRedo, bool blocked)
{
    auto command = SceneHistoryCommand::none;
    if (drawRenderModeIconButton("undo_scene", "\xef\x83\xa2", "Undo scene edit (Ctrl+Z)",
            RenderModeState::off, blocked || !canUndo)) {
        command = SceneHistoryCommand::undo;
    }
    ImGui::SameLine();
    if (drawRenderModeIconButton("redo_scene", "\xef\x80\x9e", "Redo scene edit (Ctrl+Y or Ctrl+Shift+Z)",
            RenderModeState::off, blocked || !canRedo)) {
        command = SceneHistoryCommand::redo;
    }
    return command;
}

// Nonmodal property popups allow scene undo after a drag; active text fields
// retain ImGui's own text undo. Native dialogs and processing are gated by caller.
inline SceneHistoryCommand sceneHistoryShortcut(bool blocked)
{
    if (blocked || ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive()) {
        return SceneHistoryCommand::none;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)
        || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal)) {
        return SceneHistoryCommand::redo;
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)) {
        return SceneHistoryCommand::undo;
    }
    return SceneHistoryCommand::none;
}

inline uint64_t sceneHistoryInteraction() { return ImGui::GetActiveID(); }

} // namespace woby
