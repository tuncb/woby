#include "hover_pick.h"

#include "hash_utils.h"

#include <bx/math.h>

#include <algorithm>
#include <cmath>

namespace woby {
namespace {

constexpr float vertexHoverMinRadius = 3.0f;
constexpr float vertexHoverEpsilon = 0.000001f;

struct ProjectedVertex {
    bool visible = false;
    float x = 0.0f;
    float y = 0.0f;
    float depth = 0.0f;
};

std::array<float, 4> transformPoint4(const float* matrix, const std::array<float, 4>& point)
{
    return {
        matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12] * point[3],
        matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13] * point[3],
        matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14] * point[3],
        matrix[3] * point[0] + matrix[7] * point[1] + matrix[11] * point[2] + matrix[15] * point[3],
    };
}

std::array<float, 3> transformPosition(const float* matrix, const std::array<float, 3>& position)
{
    const auto transformed = transformPoint4(matrix, {position[0], position[1], position[2], 1.0f});
    if (std::abs(transformed[3]) <= vertexHoverEpsilon) {
        return {transformed[0], transformed[1], transformed[2]};
    }

    return {
        transformed[0] / transformed[3],
        transformed[1] / transformed[3],
        transformed[2] / transformed[3],
    };
}

ProjectedVertex projectPosition(
    const std::array<float, 3>& position,
    const float* model,
    const float* view,
    const float* projection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth)
{
    const auto world = transformPoint4(model, {position[0], position[1], position[2], 1.0f});
    const auto eye = transformPoint4(view, world);
    const auto clip = transformPoint4(projection, eye);
    if (clip[3] <= vertexHoverEpsilon) {
        return {};
    }

    const float ndcX = clip[0] / clip[3];
    const float ndcY = clip[1] / clip[3];
    const float ndcZ = clip[2] / clip[3];
    const float minDepth = homogeneousDepth ? -1.0f : 0.0f;
    if (ndcX < -1.0f
        || ndcX > 1.0f
        || ndcY < -1.0f
        || ndcY > 1.0f
        || ndcZ < minDepth
        || ndcZ > 1.0f) {
        return {};
    }

    ProjectedVertex projected;
    projected.visible = true;
    projected.x = (ndcX * 0.5f + 0.5f) * static_cast<float>(viewportWidth);
    projected.y = (0.5f - ndcY * 0.5f) * static_cast<float>(viewportHeight);
    projected.depth = ndcZ;
    return projected;
}

void updateHoveredVertexCandidate(
    const std::array<float, 3>& localPosition,
    const std::array<float, 3>& transformedPosition,
    const ProjectedVertex& projected,
    const MousePosition& mouse,
    float hoverRadius,
    std::optional<HoveredVertex>& hoveredVertex)
{
    const float deltaX = projected.x - mouse.x;
    const float deltaY = projected.y - mouse.y;
    const float distanceSquared = deltaX * deltaX + deltaY * deltaY;
    if (distanceSquared > hoverRadius * hoverRadius) {
        return;
    }

    if (hoveredVertex.has_value()
        && (projected.depth > hoveredVertex->depth
            || (projected.depth == hoveredVertex->depth
                && distanceSquared >= hoveredVertex->distanceSquared))) {
        return;
    }

    hoveredVertex = HoveredVertex{
        localPosition,
        transformedPosition,
        projected.depth,
        distanceSquared,
    };
}

} // namespace

uint64_t hoverPickSignature(
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    const MousePosition& mouse,
    bool mouseInsideViewport,
    float masterVertexPointSize,
    const SceneCamera& camera,
    SceneUpAxis upAxis,
    const Bounds& sceneBounds,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth)
{
    uint64_t seed = 0xcbf29ce484222325ull;
    hashFloat(seed, mouse.x);
    hashFloat(seed, mouse.y);
    hashBool(seed, mouseInsideViewport);
    hashFloat(seed, masterVertexPointSize);
    hashCamera(seed, camera);
    hashCombine(seed, static_cast<uint64_t>(upAxis));
    hashBounds(seed, sceneBounds);
    hashCombine(seed, viewportWidth);
    hashCombine(seed, viewportHeight);
    hashBool(seed, homogeneousDepth);
    hashCombine(seed, files.size());
    hashCombine(seed, runtimes.size());

    const size_t fileCount = std::min(files.size(), runtimes.size());
    for (size_t fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
        const auto& file = files[fileIndex];
        const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
        hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.vertices.data()));
        hashCombine(seed, file.mesh.vertices.size());
        hashCombine(seed, reinterpret_cast<uintptr_t>(gpuMesh.pointVertexIndices.data()));
        hashCombine(seed, gpuMesh.pointVertexIndices.size());
        hashCombine(seed, gpuMesh.nodeRanges.size());
        hashFileSettings(seed, file.fileSettings);
        hashFloat(seed, file.vertexSizeScale);

        const size_t groupCount = std::min(file.groupSettings.size(), gpuMesh.nodeRanges.size());
        hashCombine(seed, groupCount);
        for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
            const auto& range = gpuMesh.nodeRanges[groupIndex];
            hashCombine(seed, range.pointIndexOffset);
            hashCombine(seed, range.pointIndexCount);
            hashGroupSettings(seed, file.groupSettings[groupIndex]);
        }
    }

    return seed;
}

std::optional<HoveredVertex> findHoveredVertex(
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    const MousePosition& mouse,
    float masterVertexPointSize,
    const float* view,
    const float* projection,
    uint32_t viewportWidth,
    uint32_t viewportHeight,
    bool homogeneousDepth)
{
    std::optional<HoveredVertex> hoveredVertex;
    const size_t fileCount = std::min(files.size(), runtimes.size());
    for (size_t fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
        const auto& file = files[fileIndex];
        const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
        if (!file.fileSettings.visible || file.fileSettings.opacity <= vertexHoverEpsilon) {
            continue;
        }

        float fileModel[16];
        fileTransformMatrix(file.fileSettings, fileModel);
        const size_t groupCount = std::min(file.groupSettings.size(), gpuMesh.nodeRanges.size());
        for (size_t nodeIndex = 0; nodeIndex < groupCount; ++nodeIndex) {
            const auto& settings = file.groupSettings[nodeIndex];
            if (!settings.visible
                || !settings.showVertices
                || settings.opacity <= vertexHoverEpsilon) {
                continue;
            }

            float groupModel[16];
            float model[16];
            groupTransformMatrix(settings, groupModel);
            bx::mtxMul(model, fileModel, groupModel);

            const uint32_t pointSize = vertexPointSize(
                masterVertexPointSize,
                file.vertexSizeScale * settings.vertexSizeScale);
            const float hoverRadius = std::max(
                static_cast<float>(pointSize) * 0.5f,
                vertexHoverMinRadius);
            const auto& range = gpuMesh.nodeRanges[nodeIndex];
            const uint32_t endIndex = std::min(
                range.pointIndexOffset + range.pointIndexCount,
                static_cast<uint32_t>(gpuMesh.pointVertexIndices.size()));
            for (uint32_t index = range.pointIndexOffset; index < endIndex; ++index) {
                const uint32_t vertexIndex = gpuMesh.pointVertexIndices[index];
                if (vertexIndex >= file.mesh.vertices.size()) {
                    continue;
                }

                const auto& localPosition = file.mesh.vertices[vertexIndex].position;
                const auto projected = projectPosition(
                    localPosition,
                    model,
                    view,
                    projection,
                    viewportWidth,
                    viewportHeight,
                    homogeneousDepth);
                if (!projected.visible) {
                    continue;
                }

                updateHoveredVertexCandidate(
                    localPosition,
                    transformPosition(model, localPosition),
                    projected,
                    mouse,
                    hoverRadius,
                    hoveredVertex);
            }
        }
    }

    return hoveredVertex;
}

} // namespace woby
