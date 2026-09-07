#pragma once

namespace woby {

struct UiState;

struct SettingsDialogResult {
    bool open = false;
    bool scaleChanged = false;
};

bool drawSettingsButton(bool disabled);
SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen);

} // namespace woby
