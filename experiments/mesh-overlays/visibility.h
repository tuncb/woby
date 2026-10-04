#pragma once
#include "renderer.h"
#include <span>

namespace woby::overlay {
struct MarkerRange { uint32_t offset = 0, count = 0; };
struct MarkerSelection {
    // Original global marker offsets, in original submission order.
    std::vector<uint32_t> markers;
    std::vector<MarkerRange> groups;
};
struct VisibilityAudit {
    uint64_t submitted = 0, depthClipped = 0, offscreen = 0, centerOutside = 0;
    uint64_t centerOccluded = 0, footprintOccluded = 0;
    double projectedQuadPixels = 0;
    MarkerSelection frustum, conservative, center;
};
// Offline diagnostic only: current-frame depth readback and CPU classification
// are excluded from draw timings. The selections are not a moving-camera cache.
VisibilityAudit inspectVisibility(const Mesh& mesh, const Scene& scene,
    const std::array<float,16>& viewProjection, uint32_t width, uint32_t height,
    float pointSize, std::span<const float> minimumSampleDepth);
MarkerSelection finalMarkerOracle(const Scene& scene, std::span<const uint32_t> sampleIds);
void installMarkerSelection(Renderer& renderer, const MarkerSelection& selection);
} // namespace woby::overlay
