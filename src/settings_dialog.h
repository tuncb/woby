#pragma once

#include "update_ui.h"

namespace woby {

struct UiState;

struct SettingsDialogResult {
    bool open = false;
    bool scaleChanged = false;
    UpdateCommand updateCommand = UpdateCommand::none;
};

bool drawSettingsButton(bool disabled);
SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen, const UpdateUiState& update = {});

} // namespace woby
