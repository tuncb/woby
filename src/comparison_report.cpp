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
        if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold; red edges: winding"); }
        return lines;
    }
    if (settings.mode != ComparisonMode::distance) {
        lines.push_back(settings.mode == ComparisonMode::overlay ? "Overlay: A blue wireframe; B gray surface" :
            settings.mode == ComparisonMode::original ? "Group A surface" : "Group B surface");
        if (settings.showBoundaries) { lines.push_back("Green edges: boundary"); }
        if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold; red edges: winding"); }
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
    if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold; red edges: winding"); }
    return lines;
}

} // namespace woby
