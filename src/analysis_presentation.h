#pragma once

#include "comparison_settings.h"
#include "mesh_comparison.h"

namespace woby {

// Presentation categories are independent of the rendered A/B view.
[[nodiscard]] AnalysisTask analysisTask(const ComparisonSettings& settings, bool bothInputs);
[[nodiscard]] const char* analysisTaskLabel(AnalysisTask task);
[[nodiscard]] const char* analysisTaskKey(AnalysisTask task);
[[nodiscard]] ComparisonSettings settingsForAnalysisTask(ComparisonSettings settings, AnalysisTask task);

enum class AnalysisResultState { ready, notRun, queued, running, outdated, canceled, failed, partial, unavailable };
struct DiagnosticSummary {
    AnalysisResultState state = AnalysisResultState::notRun;
    size_t count = 0;
};
[[nodiscard]] const char* analysisResultStateLabel(AnalysisResultState state);
[[nodiscard]] DiagnosticSummary diagnosticSummary(const SurfaceComparison& surface, DiagnosticCategory category,
    IntersectionPhase phase, bool current);
[[nodiscard]] std::string diagnosticSummaryText(const DiagnosticSummary& summary);

} // namespace woby
