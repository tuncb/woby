#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>

namespace woby {
enum class ModelLoadStage { reading, triangulating, sourcePositions, buildingMesh, normals, bounds, groups, ready };
struct ModelLoadProgress {
    ModelLoadStage stage = ModelLoadStage::reading;
    size_t completed = 0;
    size_t total = 0; // Zero means the stage does not expose a measurable fraction.
};
using ModelLoadProgressCallback = std::function<void(const ModelLoadProgress&)>;
inline void reportModelLoadProgress(const ModelLoadProgressCallback& callback, ModelLoadStage stage,
    size_t completed = 0, size_t total = 0)
{
    if (callback) { callback({stage, completed, total}); }
}
inline const char* modelLoadStageName(ModelLoadStage stage)
{
    switch (stage) {
    case ModelLoadStage::reading: return "Reading and parsing geometry";
    case ModelLoadStage::triangulating: return "Triangulating faces";
    case ModelLoadStage::sourcePositions: return "Validating source positions";
    case ModelLoadStage::buildingMesh: return "Building model geometry";
    case ModelLoadStage::normals: return "Checking and generating normals";
    case ModelLoadStage::bounds: return "Calculating model bounds";
    case ModelLoadStage::groups: return "Preparing model groups";
    case ModelLoadStage::ready: return "Geometry ready";
    }
    return "Loading geometry";
}
// Stage completion, not an estimate of remaining time.
inline float modelLoadFraction(const ModelLoadProgress& progress)
{
    const float fraction = progress.total ? std::min(1.0f, static_cast<float>(progress.completed) / static_cast<float>(progress.total)) : 0.0f;
    return std::min(1.0f, (static_cast<float>(progress.stage) + fraction) / static_cast<float>(ModelLoadStage::ready));
}
} // namespace woby
