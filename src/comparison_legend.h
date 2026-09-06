#pragma once

#include "comparison_settings.h"
#include <imgui.h>

namespace woby {
// Draw at the supplied position, returning the height used including numeric labels.
float drawComparisonLegend(ImDrawList& draw, ImVec2 position, float width, float fontSize,
    const ComparisonSettings& settings);
}
