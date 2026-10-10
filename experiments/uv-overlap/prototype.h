#pragma once
#include "uv_quality.h"
#include <limits>

namespace woby {
void inspectUvOverlapsReference(UvQuality& quality, std::stop_token stop = {}, UvOverlapLimits limits = {});
struct UvBroadPhaseProbe {
    size_t nodeTests = 0, leafTests = 0, scratchBytes = 0;
    double buildMs = 0, queryMs = 0;
    bool workTruncated = false;
    size_t maintenanceTests = 0, peakActive = 0;
};
void inspectUvOverlapsTree(UvQuality& quality, UvBroadPhaseProbe& probe,
    UvOverlapLimits limits = {}, std::stop_token stop = {},
    size_t workLimit = std::numeric_limits<size_t>::max(), bool lazyPatches = false);
void inspectUvOverlapsSweep(UvQuality& quality, UvBroadPhaseProbe& probe,
    UvOverlapLimits limits = {}, std::stop_token stop = {},
    size_t workLimit = std::numeric_limits<size_t>::max(), bool verifyIndex = false);
}
