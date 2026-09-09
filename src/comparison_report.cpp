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

std::string comparisonUnits(const ComparisonSettings& settings)
{
    return settings.unitLabel.empty() ? "model units" : settings.unitLabel;
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
    if (settings.mode != ComparisonMode::distance) {
        lines.push_back(settings.mode == ComparisonMode::overlay ? "Overlay: A blue wireframe; B gray surface" :
            settings.mode == ComparisonMode::original ? "Group A surface" : "Group B surface");
        if (settings.showBoundaries) { lines.push_back("Green edges: boundary"); }
        if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold; red edges: winding"); }
        return lines;
    }
    const auto& surface = settings.distanceOnOriginal ? result.original : result.repaired;
    const auto units = comparisonUnits(settings);
    lines.push_back("Approximate unsigned surface distance (" + units + ")");
    lines.push_back("Four samples per triangle; not an exact maximum.");
    if (options.direction) {
        lines.push_back(settings.distanceOnOriginal ? "A -> B: measured A; reference B" : "B -> A: measured B; reference A");
    }
    if (options.tolerance || options.legend) {
        lines.push_back("Tolerance: " + measurementNumber(settings.tolerance) + " " + units + " (gray at or below)");
    }
    if (options.legend) {
        lines.push_back("Color maximum: " + measurementNumber(settings.colorRange) + " " + units +
            (settings.colorRange == settings.tolerance ? " (above tolerance saturates)" : " (>= saturates)"));
        if (surface.maximum > settings.colorRange) {
            lines.push_back("SATURATED: sampled values exceed the color maximum.");
        }
        lines.push_back("Sample max: " + measurementNumber(surface.maximum) + " " + units);
        lines.push_back("Area-weighted mean: " + measurementNumber(surface.mean) + " " + units);
        lines.push_back("Area-weighted P95: " + measurementNumber(surface.percentile95) + " " + units);
        lines.push_back("Area above tolerance: " + measurementNumber(surfacePercentAboveTolerance(surface, settings.tolerance)) + "%");
        lines.push_back("Surface shading affects brightness; legend shows unlit colors.");
    }
    if (settings.showBoundaries) { lines.push_back("Green edges: boundary"); }
    if (settings.showNonManifold) { lines.push_back("Pink edges: non-manifold; red edges: winding"); }
    return lines;
}

} // namespace woby
