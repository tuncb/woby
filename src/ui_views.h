#pragma once

#include "ui_state.h"

namespace woby {

// ImGui editing state remains outside the logical scene.
struct ViewNameEdit {
    ViewId id = 0;
    uint64_t generation = 0;
    std::string text;
    bool focus = false;
};

// The divider position is window layout state, separate from the saved scene.
struct ViewListLayout {
    float preferredHeight = 0.0f; // Zero follows the list's automatic height.
};

[[nodiscard]] float viewListHeight(float preferredHeight, float automaticHeight,
    float minimumHeight, float availableHeight);
void drawViews(UiState& state, ViewNameEdit& edit, ViewListLayout& layout, float reservedHeight);

} // namespace woby
