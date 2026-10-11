#pragma once

#include "update_ui.h"
#include "file_associations.h"
#include <imgui.h>

namespace woby {

struct UiState;

struct SettingsDialogResult {
    bool open = false;
    bool scaleChanged = false;
    UpdateCommand updateCommand = UpdateCommand::none;
    FileAssociationRequest fileAssociationRequest;
};

bool drawSettingsButton(bool disabled);
SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen, ImVec2 anchor = ImVec2(-1, -1),
    const FileAssociationsState& associations = {});
SettingsDialogResult drawUpdatesDialog(UiState& state, bool requestOpen, const UpdateUiState& update,
    ImVec2 anchor = ImVec2(-1, -1));

} // namespace woby
