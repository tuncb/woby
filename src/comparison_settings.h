#pragma once

#include <cstddef>

namespace woby
{

enum class ComparisonMode
{
    distance,
    original,
    repaired,
    overlay
};

struct ComparisonSettings
{
    bool enabled = false;
    int originalFile = -1;
    int repairedFile = -1;
    ComparisonMode mode = ComparisonMode::distance;
    bool distanceOnOriginal = false;
    float tolerance = 0.05f;
    float colorRange = 0.5f;
    bool showEdges = false;
    bool showBoundaries = true;
    bool showNonManifold = true;
    friend bool operator==(const ComparisonSettings &, const ComparisonSettings &) = default;
};

[[nodiscard]] ComparisonSettings normalizedComparisonSettings(ComparisonSettings settings, size_t fileCount);

} // namespace woby
