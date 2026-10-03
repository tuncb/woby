#pragma once
#include "ui_state.h"
#include "uv_quality.h"

namespace woby {
void drawUvQualityControls(UiState& state, ComparisonSettings& settings, const UvQuality* quality, SceneObjectId id);
}
