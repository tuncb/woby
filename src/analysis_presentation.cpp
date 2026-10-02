#include "analysis_presentation.h"

namespace woby {

AnalysisTask analysisTask(const ComparisonSettings& settings, bool bothInputs)
{
    if (isUvAnalysis(settings.type)) { return AnalysisTask::uvInspection; }
    if (settings.task != AnalysisTask::automatic) { return settings.task; }
    if (settings.mode == ComparisonMode::surfaceQuality) { return AnalysisTask::meshQuality; }
    if (bothInputs && (settings.mode == ComparisonMode::distance || settings.mode == ComparisonMode::overlay)) {
        return AnalysisTask::surfaceComparison;
    }
    return AnalysisTask::meshChecks;
}

const char* analysisTaskLabel(AnalysisTask task)
{
    switch (task) {
    case AnalysisTask::meshChecks: return "Mesh checks";
    case AnalysisTask::meshQuality: return "Mesh quality";
    case AnalysisTask::surfaceComparison: return "Comparison";
    case AnalysisTask::uvInspection: return "UV inspection";
    case AnalysisTask::automatic: default: return "Analysis";
    }
}

const char* analysisTaskKey(AnalysisTask task)
{
    switch (task) {
    case AnalysisTask::meshChecks: return "mesh_checks";
    case AnalysisTask::meshQuality: return "mesh_quality";
    case AnalysisTask::surfaceComparison: return "surface_comparison";
    case AnalysisTask::uvInspection: return "uv_inspection";
    case AnalysisTask::automatic: default: return "automatic";
    }
}

ComparisonSettings settingsForAnalysisTask(ComparisonSettings settings, AnalysisTask task, bool hasA, bool hasB)
{
    if (task < AnalysisTask::meshChecks || task > AnalysisTask::uvInspection) { return settings; }
    settings.task = task;
    if (task == AnalysisTask::uvInspection) {
        if (!isUvAnalysis(settings.type)) { settings.type = AnalysisType::uv; }
    } else {
        settings.type = AnalysisType::mesh;
        switch (task) {
        case AnalysisTask::meshChecks:
            settings.mode = !hasA && hasB ? ComparisonMode::repaired : ComparisonMode::original;
            settings.diagnosticSide = !hasA && hasB ? ComparisonSide::b : ComparisonSide::a;
            break;
        case AnalysisTask::meshQuality:
            settings.mode = ComparisonMode::surfaceQuality;
            if (!hasA && hasB) { settings.quality.onOriginal = false; }
            else if (hasA && !hasB) { settings.quality.onOriginal = true; }
            break;
        case AnalysisTask::surfaceComparison: settings.mode = ComparisonMode::distance; break;
        case AnalysisTask::automatic: case AnalysisTask::uvInspection: break;
        }
    }
    return normalizedComparisonSettings(settings);
}

} // namespace woby
