#pragma once

#include <cstddef>

namespace woby
{

enum class ComparisonSide { a, b };

struct ComparisonMembership
{
    bool a = false;
    bool b = false;
    friend bool operator==(const ComparisonMembership &, const ComparisonMembership &) = default;
};

[[nodiscard]] inline bool comparisonMember(ComparisonMembership membership, ComparisonSide side)
{
    return side == ComparisonSide::a ? membership.a : membership.b;
}

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
    ComparisonMode mode = ComparisonMode::distance;
    bool distanceOnOriginal = false;
    float tolerance = 0.05f;
    float colorRange = 0.5f;
    bool showEdges = false;
    bool showBoundaries = true;
    bool showNonManifold = true;
    friend bool operator==(const ComparisonSettings &, const ComparisonSettings &) = default;
};

[[nodiscard]] ComparisonSettings normalizedComparisonSettings(ComparisonSettings settings);

} // namespace woby
