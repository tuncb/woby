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

void drawViews(UiState& state, ViewNameEdit& edit);

} // namespace woby
