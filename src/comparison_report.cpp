#include "comparison_report.h"

#include <algorithm>
#include <iomanip>
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

std::array<float, 4> comparisonHeatmapColor(double distance, const ComparisonSettings& settings)
{
    if (distance <= settings.tolerance) { return {.56f, .61f, .67f, 1}; }
    const auto amount = settings.colorRange <= settings.tolerance ? 1.0f :
        static_cast<float>(std::clamp((distance - settings.tolerance) /
            (static_cast<double>(settings.colorRange) - settings.tolerance), 0.0, 1.0));
    return {1.0f - .06f * amount, .77f - .55f * amount, .31f - .255f * amount, 1};
}

std::vector<std::string> comparisonReportLines(
    const std::string& name, const std::string& a, const std::string& b,
    const ComparisonSettings& settings, const MeshComparison& result, const ScreenshotSettings& options)
{
    std::vector<std::string> lines;
    if (options.comparisonName) { lines.push_back(name); }
    if (options.sources) {
        if (!a.empty()) { lines.push_back("A: " + a); }
        if (!b.empty()) { lines.push_back("B: " + b); }
    }
    const auto ready = [&](DiagnosticCategory category) {
        return comparisonDetectorStatus(result, category).phase == IntersectionPhase::complete;
    };
    if (options.legend) {
        size_t side = 0;
        for (const auto* surface : {&result.original, &result.repaired}) {
            const std::string label = side++ == 0 ? "A" : "B";
            if (surface->source.indices.empty()) { continue; }
            const auto append = [&](const char* name, const DuplicateResult& duplicates) {
                if (!duplicates.enabled) { return; }
                lines.push_back(label + " " + name + ": " + std::to_string(duplicates.duplicateCount) + " (" + duplicateStatus(duplicates) + ")"
                    + (duplicates.informationalCount ? "; " + std::to_string(duplicates.informationalCount) + " informational STL corners" : ""));
            };
            const char* detectorNames[] = {"boundary edges", "non-manifold edges", "inconsistent triangles", "duplicate points",
                "source-ID duplicate triangles", "degenerate triangles", "non-manifold vertices", "holes"};
            for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                const auto& status = result.detectors[i];
                if (status.phase == IntersectionPhase::complete) { continue; }
                lines.push_back(label + " " + detectorNames[i] + ": " + detectorPhaseName(status.phase)
                    + (status.hasResult ? "; previous result: " + std::to_string(status.knownCounts[side - 1]) + " known findings" : ""));
            }
            const auto& topology = surface->topology;
            lines.push_back(label + " topology: " + topologyModeName(topology.mode) + "; per source (" + topologyStatus(topology) + ")");
            if (ready(DiagnosticCategory::boundary) && ready(DiagnosticCategory::nonManifold) && ready(DiagnosticCategory::winding)) {
                lines.push_back(label + " known boundary / non-manifold edges / inconsistent triangles: "
                    + std::to_string(topology.boundaries.size()) + " / " + std::to_string(topology.nonManifoldEdges.size())
                    + " / " + std::to_string(topology.windingFaces.size()));
            } else {
                if (ready(DiagnosticCategory::boundary)) { lines.push_back(label + " boundary edges: " + std::to_string(topology.boundaries.size())); }
                if (ready(DiagnosticCategory::nonManifold)) { lines.push_back(label + " non-manifold edges: " + std::to_string(topology.nonManifoldEdges.size())); }
                if (ready(DiagnosticCategory::winding)) { lines.push_back(label + " inconsistent triangles: " + std::to_string(topology.windingFaces.size())); }
            }
            if (ready(DiagnosticCategory::nonManifoldVertices)) {
                lines.push_back(label + " non-manifold vertices: " + std::to_string(topology.nonManifoldVertices.size()) + " known (" + topologyStatus(topology) + ")");
            }
            if (ready(DiagnosticCategory::holes)) {
                size_t branched = 0, open = 0;
                for (const auto& boundary : topology.boundaryRegions) { branched += boundary.kind == BoundaryKind::branched; open += boundary.kind == BoundaryKind::open; }
                lines.push_back(label + " holes: " + std::to_string(topology.holes.size()) + " known (" + topologyStatus(topology) + "); "
                    + std::to_string(branched) + " branched / " + std::to_string(open) + " open boundary regions");
            }
            if (topology.excludedCollapsedFaces) { lines.push_back(label + " topology excluded collapsed faces: " + std::to_string(topology.excludedCollapsedFaces)); }
            if (ready(DiagnosticCategory::duplicatePoints)) { append("duplicate points", surface->duplicates.points); }
            if (ready(DiagnosticCategory::duplicateTriangles)) { append("source-ID duplicate triangles", surface->duplicates.triangles); }
            {
                const auto& d = surface->intersections;
                if (d.phase != IntersectionPhase::complete) {
                    lines.push_back(label + " self-intersections: " + intersectionStatus(d)
                        + (d.hasResult ? "; previous result: " + std::to_string(d.findings.size()) + " known pairs" : ""));
                } else {
                    lines.push_back(label + " self-intersections: " + ((d.unavailableSources || d.truncated) ? std::string("N/A; known ") : std::string{})
                        + std::to_string(d.findings.size()) + " pairs; " + std::to_string(d.affectedFaces) + " known affected faces (" + intersectionStatus(d) + ")");
                }
            }
            if (ready(DiagnosticCategory::degenerateTriangles)) {
                const auto& d = surface->degenerates;
                lines.push_back(label + " degenerate triangles: " + (d.unavailableSources ? std::string("N/A; known ") : std::string{})
                    + std::to_string(d.findings.size()) + " (" + degenerateStatus(d) + ")");
                lines.push_back(label + " collapsed / needle / cap: " + std::to_string(d.collapsedCount) + " / "
                    + std::to_string(d.needleCount) + " / " + std::to_string(d.capCount) + " (reasons overlap)");
            }
        }
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
        const auto metric = settings.quality.metric;
        const auto index = static_cast<size_t>(metric);
        const auto& distribution = result.qualityDistributions[index];
        lines.push_back("Surface mesh quality");
        lines.push_back(surfaceQualityMetricName(metric));
        if (options.direction) { lines.push_back(settings.quality.onOriginal ? "Heatmap: A" : "Heatmap: B"); }
        if (options.legend) {
            lines.push_back("Shared A/B range: " + measurementNumber(distribution.minimum) + " to " + measurementNumber(distribution.maximum));
            lines.push_back(metric == SurfaceQualityMetric::shape ? "Blue: equilateral; red: poor shape." :
                metric == SurfaceQualityMetric::sizeJump ? "Blue: equal neighbor size; red: largest jump." : "Blue: small; red: large. Colors describe size.");
            lines.push_back("Magenta: degenerate; gray: unavailable. Surface shading affects brightness.");
            size_t side = 0;
            for (const auto* quality : {&result.original.quality, &result.repaired.quality}) {
                const std::string label = side++ == 0 ? "A" : "B";
                if (quality->triangles.empty()) { continue; }
                lines.push_back(label + ": " + std::to_string(quality->triangles.size()) + " triangles; " +
                    std::to_string(quality->degenerateTriangles) + " degenerate");
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
                    const std::string label = side++ == 0 ? "A" : "B";
                    if (quality->triangles.empty()) { continue; }
                    const auto limits = surfaceQualitySizeLimits(*quality, settings.quality);
                    lines.push_back(label + " below / above limits: " + std::to_string(limits.below) + " / " + std::to_string(limits.above));
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
        lines.push_back("Color maximum: " + measurementNumber(settings.colorRange) +
            (settings.colorRange == settings.tolerance ? " (above tolerance saturates)" : " (>= saturates)"));
        if (surface.maximum > settings.colorRange) {
            lines.push_back("SATURATED: sampled values exceed the color maximum.");
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
