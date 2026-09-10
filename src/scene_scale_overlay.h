#pragma once

#include "scene_dimensions.h"
#include <imgui.h>

namespace woby {

// Adapter only: origin and pixelScale map drawable viewport pixels to the
// destination draw list. The same overlay is used for the window and PNGs.
void drawSceneScaleOverlay(ImDrawList& draw, const UiState& state,
    const std::optional<SceneDimensions>& dimensions, const ScenePickView& view,
    ImVec2 origin, float pixelScale, float fontSize);

} // namespace woby
