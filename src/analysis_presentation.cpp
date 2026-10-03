#include "analysis_presentation.h"
#include <algorithm>

namespace woby {

AnalysisTask analysisTask(const ComparisonSettings& settings, bool bothInputs)
{
    if (isUvAnalysis(settings.type)) { return AnalysisTask::uvInspection; }
    if (settings.type == AnalysisType::surfaceComparison) { return AnalysisTask::surfaceComparison; }
    if (settings.task != AnalysisTask::automatic) { return settings.task; }
    if (settings.mode == ComparisonMode::surfaceQuality) { return AnalysisTask::meshQuality; }
    if (bothInputs) {
        return AnalysisTask::surfaceComparison;
    }
    return AnalysisTask::meshChecks;
}

const char* analysisTaskLabel(AnalysisTask task)
{
    switch (task) {
    case AnalysisTask::meshChecks: return "Mesh checks";
    case AnalysisTask::meshQuality: return "Mesh quality";
    case AnalysisTask::surfaceComparison: return "Surface comparison";
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

ComparisonSettings settingsForAnalysisTask(ComparisonSettings settings, AnalysisTask task)
{
    if (task < AnalysisTask::meshChecks || task > AnalysisTask::uvInspection) { return settings; }
    settings.task = task;
    if (task == AnalysisTask::uvInspection) {
        if (!isUvAnalysis(settings.type)) { settings.type = AnalysisType::uv; }
    } else {
        settings.type = AnalysisType::mesh;
        switch (task) {
        case AnalysisTask::meshChecks:
            settings.mode = ComparisonMode::original;
            settings.diagnosticSide = ComparisonSide::a;
            break;
        case AnalysisTask::meshQuality:
            settings.mode = ComparisonMode::surfaceQuality;
            settings.quality.onOriginal = true;
            break;
        case AnalysisTask::surfaceComparison:
            settings.type = AnalysisType::surfaceComparison;
            settings.mode = ComparisonMode::distance;
            break;
        case AnalysisTask::automatic: case AnalysisTask::uvInspection: break;
        }
    }
    return normalizedComparisonSettings(settings);
}


const char* analysisResultStateLabel(AnalysisResultState state)
{
    switch (state) {
    case AnalysisResultState::ready: return "Ready";
    case AnalysisResultState::notRun: return "Not run";
    case AnalysisResultState::queued: return "Queued";
    case AnalysisResultState::running: return "Running";
    case AnalysisResultState::outdated: return "Outdated";
    case AnalysisResultState::canceled: return "Canceled";
    case AnalysisResultState::failed: return "Failed";
    case AnalysisResultState::partial: return "Partial";
    case AnalysisResultState::unavailable: return "Unavailable";
    }
    return "Unavailable";
}

DiagnosticSummary diagnosticSummary(const SurfaceComparison& surface, DiagnosticCategory category,
    IntersectionPhase phase, bool current)
{
    DiagnosticSummary summary;
    switch (phase) {
    case IntersectionPhase::notChecked: return summary;
    case IntersectionPhase::queued: summary.state = AnalysisResultState::queued; return summary;
    case IntersectionPhase::running: summary.state = AnalysisResultState::running; return summary;
    case IntersectionPhase::outdated: summary.state = AnalysisResultState::outdated; return summary;
    case IntersectionPhase::canceled: summary.state = AnalysisResultState::canceled; return summary;
    case IntersectionPhase::failed: summary.state = AnalysisResultState::failed; return summary;
    case IntersectionPhase::complete: break;
    }
    if (!current) { summary.state = AnalysisResultState::outdated; return summary; }
    size_t available = surface.topology.availableSources, unavailable = surface.topology.unavailableSources;
    bool limited = false;
    switch (category) {
    case DiagnosticCategory::boundary: summary.count = surface.topology.boundaries.size(); break;
    case DiagnosticCategory::nonManifold: summary.count = surface.topology.nonManifoldEdges.size(); break;
    case DiagnosticCategory::winding: summary.count = surface.topology.windingFaces.size(); break;
    case DiagnosticCategory::nonManifoldVertices: summary.count = surface.topology.nonManifoldVertices.size(); break;
    case DiagnosticCategory::holes: summary.count = surface.topology.holes.size(); break;
    case DiagnosticCategory::fins:
        summary.count = surface.topology.fins.size();
        unavailable += surface.topology.unavailableFinAreaSources;
        available -= std::min(available, surface.topology.unavailableFinAreaSources);
        break;
    case DiagnosticCategory::duplicatePoints: case DiagnosticCategory::duplicateTriangles: {
        const auto& result = category == DiagnosticCategory::duplicatePoints ? surface.duplicates.points : surface.duplicates.triangles;
        summary.count = result.duplicateCount; available = result.availableSources; unavailable = result.unavailableSources;
        break;
    }
    case DiagnosticCategory::degenerateTriangles:
        summary.count = surface.degenerates.findings.size();
        available = surface.degenerates.availableSources; unavailable = surface.degenerates.unavailableSources;
        break;
    case DiagnosticCategory::selfIntersections:
        summary.count = surface.intersections.findings.size();
        available = surface.intersections.availableSources; unavailable = surface.intersections.unavailableSources;
        limited = surface.intersections.truncated;
        break;
    }
    summary.state = unavailable && !available ? AnalysisResultState::unavailable
        : unavailable || limited ? AnalysisResultState::partial : AnalysisResultState::ready;
    return summary;
}

std::string diagnosticSummaryText(const DiagnosticSummary& summary)
{
    if (summary.state == AnalysisResultState::ready) { return std::to_string(summary.count); }
    if (summary.state == AnalysisResultState::partial) { return std::to_string(summary.count) + "+ Partial"; }
    return analysisResultStateLabel(summary.state);
}

} // namespace woby
