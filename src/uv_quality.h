#pragma once

#include "comparison_settings.h"
#include "model_mesh.h"
#include <stop_token>

namespace woby {

struct UvTriangleQuality {
    uint64_t partId = 0;
    size_t triangle = 0; // One based, within the imported patch.
    double angleDegrees = 0, areaLog2 = 0, surfaceArea = 0, uvArea = 0;
    int orientation = 0;
    bool missing = false, collapsed = false, degenerateSurface = false, mixedOrientation = false;
};

struct UvQuality {
    std::vector<UvTriangleQuality> triangles;
    UvAreaNormalization normalization = UvAreaNormalization::perPatch;
    UvQualityMetric metric = UvQualityMetric::angle;
    size_t missing = 0, collapsed = 0, degenerateSurface = 0, mixedOrientationPatches = 0;
};

[[nodiscard]] UvQuality analyzeUvQuality(const Mesh& mesh, UvAreaNormalization normalization, UvQualityMetric metric,
    std::stop_token stop = {});
[[nodiscard]] std::vector<Vertex> uvQualityVertices(const Mesh& display, std::stop_token stop = {});

} // namespace woby
