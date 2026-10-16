#include "comparison_report.h"
#include "count_format.h"
#include "uv_quality.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace woby {

ScreenshotSettings normalizedScreenshotSettings(ScreenshotSettings settings)
{
    settings.width = std::clamp(settings.width, 960, 7680);
    settings.height = std::clamp(settings.height, 720, 4320);
    return settings;
}

std::string measurementNumber(double value)
{
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::setprecision(5) << value;
    return text.str();
}

float fittedComparisonColorRange(double maximum, float tolerance)
{
    float range = static_cast<float>(maximum);
    if (static_cast<double>(range) < maximum) {
        range = std::nextafter(range, std::numeric_limits<float>::infinity());
    }
    return std::max(range, tolerance);
}

ComparisonSettings fittedComparisonSettings(ComparisonSettings settings, const SurfaceComparison& surface)
{
    double maximum = settings.colorRange;
    switch (settings.colorRangeMode) {
    case ComparisonColorRange::custom: return normalizedComparisonSettings(settings);
    case ComparisonColorRange::max90: maximum = surface.maximum * .9; break;
    case ComparisonColorRange::max95: maximum = surface.maximum * .95; break;
    case ComparisonColorRange::max975: maximum = surface.maximum * .975; break;
    case ComparisonColorRange::max99: maximum = surface.maximum * .99; break;
    case ComparisonColorRange::allSamples: maximum = surface.maximum; break;
    }
    settings.colorMinimum = 0;
    settings.colorRange = fittedComparisonColorRange(maximum, 0);
    return normalizedComparisonSettings(settings);
}

ComparisonSettings editedComparisonColorLimit(ComparisonSettings settings, bool lower, double value)
{
    if (!std::isfinite(value)) { return settings; }
    settings.colorRangeMode = ComparisonColorRange::custom;
    const float limit = static_cast<float>(std::clamp(value, 0.0, 1e12));
    if (lower) { settings.colorMinimum = limit; }
    else { settings.colorRange = std::max(limit, std::nextafter(settings.colorMinimum, std::numeric_limits<float>::infinity())); }
    return normalizedComparisonSettings(settings);
}

std::array<float, 4> comparisonHeatmapColor(double distance, const ComparisonSettings& settings)
{
    if (distance <= settings.tolerance) { return {.56f, .61f, .67f, 1}; }
    const double start = std::max(settings.colorMinimum, settings.tolerance);
    const auto amount = settings.colorRange <= start ? 1.0f :
        static_cast<float>(std::clamp((distance - start) / (settings.colorRange - start), 0.0, 1.0));
    return {1.0f - .06f * amount, .77f - .55f * amount, .31f - .255f * amount, 1};
}

std::vector<std::string> comparisonReportLines(
    const std::string& name, const std::string& a, const std::string& b,
    const ComparisonSettings& settings, const MeshComparison& result, const ScreenshotSettings& options)
{
    std::vector<std::string> lines;
    if (options.comparisonName) { lines.push_back(name); }
    if (options.sources) {
        if (!a.empty()) { lines.push_back((settings.task == AnalysisTask::meshQuality ? "Sources: " : "A: ") + a); }
        if (!b.empty()) { lines.push_back("B: " + b); }
    }
    if (isUvAnalysis(settings.type)) {
        lines.push_back(settings.uvView == UvView::layout ? "2D UV layout" : "3D UV surface");
        if (options.legend && settings.type == AnalysisType::uvQuality) {
            lines.push_back(uvQualityLegend(settings.uvMetric));
            lines.push_back(settings.uvNormalization == UvAreaNormalization::perPatch ? "Area / stretch normalized per patch" : "Absolute UV / surface area; stretch in UV units / surface unit");
            lines.push_back("Magenta: collapsed UV; gray: missing UV / degenerate surface");
            if (result.original.source.uvQuality) {
                const auto& q = *result.original.source.uvQuality;
                const auto& s = q.statistics;
                if (s.count) {
                    lines.push_back("Min / median / P95 / max: "+measurementNumber(s.minimum)+" / "+measurementNumber(s.median)+" / "+measurementNumber(s.percentile95)+" / "+measurementNumber(s.maximum));
                    lines.push_back("Threshold "+measurementNumber(q.settings.uvThreshold)+": "+formatCount(s.thresholdCount)+" faces; "+measurementNumber(s.thresholdAreaPercent)+"% surface area");
                }
                lines.push_back(formatCount(s.nearCollapseCount)+" near-collapse triangles (stretch < "+measurementNumber(q.settings.uvNearCollapse)+")");
                if (bool(q.overlap)) {
                    const auto& overlap = uvOverlapResult(q);
                    auto line = formatCount(overlap.pairs.size())+" UV overlap pairs; "+formatCount(overlap.crossPatchPairs)+" cross-patch (may be intentional)";
                    if (overlap.truncated) {
                        line += std::string("; PARTIAL: ")+uvOverlapTruncationKey(overlap.truncation)+" limit; highlights cover retained pairs only";
                    }
                    lines.push_back(std::move(line));
                }
                if (settings.uvThresholdEnabled || settings.uvRangeEnabled) { lines.push_back("Cyan: highlighted threshold / histogram range"); }
                lines.push_back(formatCount(uvMeasurements(q).collapsed)+" collapsed UV triangles; "+formatCount(uvMeasurements(q).mixedOrientationPatches)+" patches with mixed orientation");
            }
        } else if (options.legend && settings.uvGrid.enabled && settings.uvGrid.mode != UvColorMode::grid) {
            lines.push_back(std::string(uvColorModeKey(settings.uvGrid.mode))+" gradient: blue "+measurementNumber(settings.uvGrid.minimum)+"; yellow "+measurementNumber(settings.uvGrid.maximum));
        } else if (options.legend && settings.uvGrid.enabled) {
            lines.push_back("Cyan: constant U; orange: constant V");
            lines.push_back("Cells per UV unit: U " + measurementNumber(settings.uvGrid.densityU)
                + "; V " + measurementNumber(settings.uvGrid.densityV));
        }
        if (options.legend && settings.uvView == UvView::layout) {
            lines.push_back(settings.uvSeparated ? "Separated patches; common display scale; original UVs retained." : "Uniform display scale; overlapping islands retained; parts without UVs omitted.");
        }
        return lines;
    }
    const auto ready = [&](DiagnosticCategory category) {
        return comparisonDetectorStatus(result, category).phase == IntersectionPhase::complete;
    };
    if (options.legend) {
        size_t side = 0;
        for (const auto* surface : {&result.original, &result.repaired}) {
            const std::string label = side++ == 0 ? "A" : "B";
            if (meshGeometry(surface->source).indices.empty()) { continue; }
            const auto append = [&](const char* name, const DuplicateResult& duplicates) {
                if (!duplicates.enabled) { return; }
                lines.push_back(label + " " + name + ": " + formatCount(duplicates.duplicateCount) + " (" + duplicateStatus(duplicates) + ")");
            };
            const char* detectorNames[] = {"boundary edges", "non-manifold edges", "inconsistent triangles", "duplicate points",
                "source-ID duplicate triangles", "degenerate triangles", "non-manifold vertices", "holes", "fin candidates"};
            for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                const auto& status = result.detectors[i];
                if (status.phase == IntersectionPhase::complete) { continue; }
                lines.push_back(label + " " + detectorNames[i] + ": " + detectorPhaseName(status.phase)
                    + (status.hasResult ? "; previous result: " + formatCount(status.knownCounts[side - 1]) + " known findings" : ""));
            }
            const auto& topology = surface->topology;
            lines.push_back(label + " topology: " + topologyModeName(topology.mode) + "; per batch (" + topologyStatus(topology) + ")");
            if (ready(DiagnosticCategory::boundary) && ready(DiagnosticCategory::nonManifold) && ready(DiagnosticCategory::winding)) {
                lines.push_back(label + " known boundary / non-manifold edges / inconsistent triangles: "
                    + formatCount(topology.boundaries.size()) + " / " + formatCount(topology.nonManifoldEdges.size())
                    + " / " + formatCount(topology.windingFaces.size()));
            } else {
                if (ready(DiagnosticCategory::boundary)) { lines.push_back(label + " boundary edges: " + formatCount(topology.boundaries.size())); }
                if (ready(DiagnosticCategory::nonManifold)) { lines.push_back(label + " non-manifold edges: " + formatCount(topology.nonManifoldEdges.size())); }
                if (ready(DiagnosticCategory::winding)) { lines.push_back(label + " inconsistent triangles: " + formatCount(topology.windingFaces.size())); }
            }
            if (ready(DiagnosticCategory::nonManifoldVertices)) {
                lines.push_back(label + " non-manifold vertices: " + formatCount(topology.nonManifoldVertices.size()) + " known (" + topologyStatus(topology) + ")");
            }
            if (ready(DiagnosticCategory::fins)) {
                lines.push_back(label + " Woby fin candidates: " + formatCount(topology.fins.size()) + " known (" + finStatus(topology) + "); heuristic");
            }
            if (ready(DiagnosticCategory::holes)) {
                size_t branched = 0, open = 0;
                for (const auto& boundary : topology.boundaryRegions) { branched += boundary.kind == BoundaryKind::branched; open += boundary.kind == BoundaryKind::open; }
                lines.push_back(label + " holes: " + formatCount(topology.holes.size()) + " known (" + topologyStatus(topology) + "); "
                    + formatCount(branched) + " branched / " + formatCount(open) + " open boundary regions");
            }
            if (topology.excludedCollapsedFaces) { lines.push_back(label + " topology excluded collapsed faces: " + formatCount(topology.excludedCollapsedFaces)); }
            if (ready(DiagnosticCategory::duplicatePoints)) { append("duplicate points", surface->duplicates.points); }
            if (ready(DiagnosticCategory::duplicateTriangles)) { append("source-ID duplicate triangles", surface->duplicates.triangles); }
            {
                const auto& d = surface->intersections;
                if (d.phase != IntersectionPhase::complete) {
                    lines.push_back(label + " self-intersections: " + intersectionStatus(d)
                        + (d.hasResult ? "; previous result: " + formatCount(d.findings.size()) + " known pairs" : ""));
                } else {
                    lines.push_back(label + " self-intersections: " + ((d.unavailableSources || d.truncated) ? std::string("N/A; known ") : std::string{})
                        + formatCount(d.findings.size()) + " pairs; " + formatCount(d.affectedFaces) + " known affected faces (" + intersectionStatus(d) + ")");
                }
            }
            if (ready(DiagnosticCategory::degenerateTriangles)) {
                const auto& d = surface->degenerates;
                lines.push_back(label + " degenerate triangles: " + (d.unavailableSources ? std::string("N/A; known ") : std::string{})
                    + formatCount(d.findings.size()) + " (" + degenerateStatus(d) + ")");
                lines.push_back(label + " collapsed / needle / cap: " + formatCount(d.collapsedCount) + " / "
                    + formatCount(d.needleCount) + " / " + formatCount(d.capCount) + " (reasons overlap)");
            }
        }
    }
    if (ready(DiagnosticCategory::fins) && (options.legend || options.tolerance)) {
        lines.push_back("Fin candidates: physical boundary is not one simple loop after splitting at non-manifold edges; patch/largest boundary-bearing patch area ratio <= " + measurementNumber(settings.topologyInspection.finMaxAreaRatio) + ".");
        if (options.legend && settings.topologyInspection.showFins) { lines.push_back("Green faces/edges: Woby fin candidates (heuristic)."); }
    }
    if (ready(DiagnosticCategory::holes) && (options.legend || options.tolerance)) {
        lines.push_back("Holes: loop/component bounding-box diagonal ratio <= " + measurementNumber(settings.topologyInspection.holeSizeRatioTolerance) + "; larger openings remain boundaries.");
        if (options.legend && settings.topologyInspection.showHoles) { lines.push_back("Blue loops: holes."); }
    }
    if (options.legend && ready(DiagnosticCategory::nonManifoldVertices) && settings.topologyInspection.showNonManifoldVertices) {
        lines.push_back("Amber crosses: non-manifold vertices; endpoints of non-manifold edges excluded.");
    }
    if (options.legend) {
        lines.push_back("Self-intersections: exact world-coordinate checks per source; valid shared features excluded; coplanar overlap included.");
        if (settings.intersections.show && (result.original.intersections.phase == IntersectionPhase::complete || result.repaired.intersections.phase == IntersectionPhase::complete)) { lines.push_back("Red faces/edges: intersecting triangle pairs."); }
    }
    if (ready(DiagnosticCategory::degenerateTriangles) && (options.legend || options.tolerance)) {
        lines.push_back("Degenerates: edge ratio > " + measurementNumber(settings.degenerates.needleThresholdRatio)
            + "; maximum angle > " + measurementNumber(settings.degenerates.capMinAngleDegrees) + " degrees; collapsed faces always included.");
        if (options.legend && settings.degenerates.show) { lines.push_back("Purple faces/edges: degenerate triangles; crosses mark collapsed faces."); }
    }
    if (settings.mode == ComparisonMode::surfaceQuality) {
        const bool sources = settings.task == AnalysisTask::meshQuality;
        const auto metric = settings.quality.metric;
        const auto index = static_cast<size_t>(metric);
        const auto& distribution = result.qualityDistributions[index];
        lines.push_back("Surface mesh quality");
        lines.push_back(surfaceQualityMetricName(metric));
        if (options.direction) { lines.push_back(sources ? "Heatmap: Sources" : settings.quality.onOriginal ? "Heatmap: A" : "Heatmap: B"); }
        if (options.legend) {
            lines.push_back(std::string(sources ? "Range: " : "Shared A/B range: ") + measurementNumber(distribution.minimum) + " to " + measurementNumber(distribution.maximum));
            lines.push_back(metric == SurfaceQualityMetric::shape ? "Blue: equilateral; red: poor shape." :
                metric == SurfaceQualityMetric::sizeJump ? "Blue: equal neighbor size; red: largest jump." : "Blue: small; red: large. Colors describe size.");
            lines.push_back("Magenta: degenerate; gray: unavailable. Surface shading affects brightness.");
            size_t side = 0;
            for (const auto* quality : {&result.original.quality, &result.repaired.quality}) {
                const std::string label = sources ? "Sources" : side++ == 0 ? "A" : "B";
                if (quality->triangles.empty()) { continue; }
                lines.push_back(label + ": " + formatCount(quality->triangles.size()) + " triangles; " +
                    formatCount(quality->degenerateTriangles) + " degenerate");
                const auto& stats = quality->statistics[index];
                lines.push_back(label + " min / P5 / median / P95 / max: " + (stats.count ?
                    measurementNumber(stats.minimum) + " / " + measurementNumber(stats.percentile5) + " / " +
                    measurementNumber(stats.median) + " / " + measurementNumber(stats.percentile95) + " / " +
                    measurementNumber(stats.maximum) : "N/A"));
                lines.push_back(label + " worst shape: " + (quality->statistics[2].count ? measurementNumber(quality->statistics[2].minimum) : "N/A") +
                    "; max size jump: " + (quality->statistics[3].count ? measurementNumber(quality->statistics[3].maximum) : "N/A"));

            }
            lines.push_back("Statistics exclude degenerate faces; unavailable neighbor ratios are omitted.");
        }
        if ((options.tolerance || options.legend) && (settings.quality.minimumEnabled || settings.quality.maximumEnabled)) {
            lines.push_back("Longest-edge limits (inclusive): " +
                (settings.quality.minimumEnabled ? measurementNumber(settings.quality.minimumSize) : "no minimum") + " to " +
                (settings.quality.maximumEnabled ? measurementNumber(settings.quality.maximumSize) : "no maximum"));
            if (options.legend) {
                size_t side = 0;
                for (const auto* quality : {&result.original.quality, &result.repaired.quality}) {
                    const std::string label = sources ? "Sources" : side++ == 0 ? "A" : "B";
                    if (quality->triangles.empty()) { continue; }
                    const auto limits = surfaceQualitySizeLimits(*quality, settings.quality);
                    lines.push_back(label + " below / above limits: " + formatCount(limits.below) + " / " + formatCount(limits.above));
                    lines.push_back(label + " outside limits: " + (limits.validTriangles ? measurementNumber(limits.trianglePercent) +
                        "% of faces; " + measurementNumber(limits.areaPercent) + "% of area" : "N/A"));
                }
            }
        }
        if (settings.showBoundaries) { lines.push_back("Green edges: boundary"); }
        if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold"); }
        if (settings.showWinding) { lines.push_back("Red edges: winding conflicts"); }
        return lines;
    }
    if (settings.mode != ComparisonMode::distance) {
        lines.push_back(settings.mode == ComparisonMode::overlay ? "Overlay: A blue wireframe; B gray surface" :
            settings.mode == ComparisonMode::original ? "Group A surface" : "Group B surface");
        if (settings.showBoundaries) { lines.push_back("Green edges: boundary"); }
        if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold"); }
        if (settings.showWinding) { lines.push_back("Red edges: winding conflicts"); }
        return lines;
    }
    const auto& surface = settings.distanceOnOriginal ? result.original : result.repaired;
    lines.push_back("Approximate unsigned surface distance");
    lines.push_back("Four samples per triangle; not an exact maximum.");
    if (options.direction) {
        lines.push_back(settings.distanceOnOriginal ? "A -> B: measured A; reference B" : "B -> A: measured B; reference A");
    }
    if (options.tolerance || options.legend) {
        lines.push_back("Tolerance: " + measurementNumber(settings.tolerance) + " (gray at or below)");
    }
    if (options.legend) {
        lines.push_back("Color minimum: " + measurementNumber(settings.colorMinimum));
        lines.push_back("Color maximum: " + measurementNumber(settings.colorRange) +
            (settings.colorRange == settings.tolerance ? " (above tolerance saturates)" : " (>= saturates)"));
        if (surface.maximum > settings.colorRange || settings.colorMinimum > settings.tolerance) {
            lines.push_back("Distances outside the color range use the end colors. Measurements are unchanged.");
        }
        lines.push_back("Sample max: " + measurementNumber(surface.maximum));
        lines.push_back("Area-weighted mean: " + measurementNumber(surface.mean));
        lines.push_back("Area-weighted P95: " + measurementNumber(surface.percentile95));
        lines.push_back("Area above tolerance: " + measurementNumber(surfacePercentAboveTolerance(surface, settings.tolerance)) + "%");
        lines.push_back("Surface shading affects brightness; legend shows unlit colors.");
    }
    if (settings.showBoundaries) { lines.push_back("Green edges: boundary"); }
    if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold"); }
    if (settings.showWinding) { lines.push_back("Red edges: winding conflicts"); }
    return lines;
}

} // namespace woby
