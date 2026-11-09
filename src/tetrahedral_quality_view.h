#pragma once
#include "comparison_runtime.h"
namespace woby {
void drawTetrahedralQualityControls(TetrahedralQualitySettings& settings);
void drawTetrahedralQualityResults(UiState& state, const ComparisonRuntime& runtime, SceneObjectId id);
}
