#pragma once

#include "ui_state.h"

#include <functional>
#include <optional>

namespace woby {

struct GpuNodeRange {
    uint32_t triangleIndexOffset = 0;
    uint32_t triangleIndexCount = 0;
    uint32_t lineIndexOffset = 0;
    uint32_t lineIndexCount = 0;
    uint32_t pointIndexOffset = 0;
    uint32_t pointIndexCount = 0;
};

enum GpuMeshFeature : uint8_t { gpuMeshEdges = 1, gpuMeshPoints = 2 };
[[nodiscard]] uint8_t requestedGpuMeshFeatures(const UiFileState& file);

// Owned worker output: no graphics handles or borrowed source pointers.
struct SceneMeshPreparation {
    std::vector<GpuNodeRange> nodeRanges;
    std::vector<uint32_t> pointVertexIndices;
    std::vector<uint32_t> edgeIndices;
    size_t uploadBytes = 0;
    uint8_t features = 0;
};

// Nullopt means cancellation. Invalid geometry/capacity throws before allocation.
[[nodiscard]] std::optional<SceneMeshPreparation> prepareSceneMesh(
    const Mesh& mesh, uint8_t features = 0,
    const std::function<bool()>& shouldCancel = {});

} // namespace woby
