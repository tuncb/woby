#pragma once

#include "comparison_settings.h"

namespace woby {

// Presentation categories are independent of the rendered A/B view.
[[nodiscard]] AnalysisTask analysisTask(const ComparisonSettings& settings, bool bothInputs);
[[nodiscard]] const char* analysisTaskLabel(AnalysisTask task);
[[nodiscard]] const char* analysisTaskKey(AnalysisTask task);
[[nodiscard]] ComparisonSettings settingsForAnalysisTask(ComparisonSettings settings, AnalysisTask task,
    bool hasA, bool hasB);

} // namespace woby
