#pragma once
#include "comparison_runtime.h"
namespace woby {
void appendCross(std::vector<std::array<float, 3>>& lines, const std::array<float, 3>& point, float radius);
void destroySurface(ComparisonGpuSurface& gpu);
void destroyStage(ComparisonGpuSurface& gpu, uint32_t stages);
void uploadSurface(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages,
    const PreparedComparisonSource* prepared = nullptr);
void uploadQuality(ComparisonGpuSurface& gpu, const SurfaceComparison& surface,
    SurfaceQualityMetric metric, const QualityDistribution& distribution);
} // namespace woby
