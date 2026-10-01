#include "scene_renderer.h"
#include "marker_pick.h"
#include "scene_dimensions.h"

#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <stdexcept>

namespace woby {
namespace {

// point_sprite.vert.sc reads each mesh vertex as two vec4 records.
static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, position) == 0);

using HelperLineVertex = std::array<float, 3>;

uint32_t sceneBufferBytes(size_t count, size_t elementBytes)
{
    if (count > std::numeric_limits<uint32_t>::max() / elementBytes) {
        throw std::runtime_error("Scene exceeds the supported 32-bit GPU buffer size.");
    }
    return static_cast<uint32_t>(count * elementBytes);
}

template <typename T>
const woby::graphics::Memory* ownedBuffer(std::vector<T> values)
{
    const auto bytes = sceneBufferBytes(values.size(), sizeof(T));
    auto owner = std::make_unique<std::vector<T>>(std::move(values));
    // graphics releases this allocation on its render thread after consuming it.
    // The callback owns only immutable upload data, never UI state or a mesh.
    const auto* memory = woby::graphics::makeRef(owner->data(), bytes, [](void*, void* data) {
        delete static_cast<std::vector<T>*>(data);
    }, owner.get());
    owner.release();
    return memory;
}

std::array<float, 4> scaledRgbColor(const std::array<float, 4>& color, float scale)
{
    return {
        std::clamp(color[0] * scale, 0.0f, 1.0f),
        std::clamp(color[1] * scale, 0.0f, 1.0f),
        std::clamp(color[2] * scale, 0.0f, 1.0f),
        color[3],
    };
}

std::array<float, 4> groupColor(
    const UiGroupState& settings,
    float rgbScale,
    float opacityScale = 1.0f)
{
    auto color = scaledRgbColor(settings.color, rgbScale);
    color[3] = std::clamp(settings.opacity * opacityScale, minGroupOpacity, maxGroupOpacity);
    return color;
}

std::vector<uint32_t> buildLineIndices(const std::vector<uint32_t>& triangleIndices)
{
    std::vector<uint32_t> lineIndices;
    lineIndices.reserve((triangleIndices.size() / 3u) * 6u);

    for (size_t index = 0; index + 2u < triangleIndices.size(); index += 3u) {
        const uint32_t a = triangleIndices[index + 0u];
        const uint32_t b = triangleIndices[index + 1u];
        const uint32_t c = triangleIndices[index + 2u];
        lineIndices.push_back(a);
        lineIndices.push_back(b);
        lineIndices.push_back(b);
        lineIndices.push_back(c);
        lineIndices.push_back(c);
        lineIndices.push_back(a);
    }

    return lineIndices;
}

uint32_t appendPointIndicesForRange(
    const std::vector<uint32_t>& triangleIndices,
    uint32_t triangleIndexOffset,
    uint32_t triangleIndexCount,
    std::vector<size_t>& vertexGroups,
    size_t groupIndex,
    std::vector<uint32_t>& pointIndices)
{
    const uint32_t pointOffset = static_cast<uint32_t>(pointIndices.size());

    const uint32_t endIndex = triangleIndexOffset + triangleIndexCount;
    for (uint32_t index = triangleIndexOffset; index < endIndex; ++index) {
        const uint32_t vertexIndex = triangleIndices[index];
        if (vertexGroups[vertexIndex] != groupIndex) {
            vertexGroups[vertexIndex] = groupIndex;
            pointIndices.push_back(vertexIndex);
        }
    }

    return static_cast<uint32_t>(pointIndices.size()) - pointOffset;
}

uint64_t renderState(
    uint64_t depthTest,
    bool writeDepth,
    const std::array<float, 4>& color,
    uint64_t primitiveState)
{
    uint64_t state = WOBY_GPU_STATE_WRITE_RGB
        | WOBY_GPU_STATE_WRITE_A
        | depthTest
        | WOBY_GPU_STATE_MSAA
        | primitiveState;

    if (writeDepth && color[3] >= 0.999f) {
        state |= WOBY_GPU_STATE_WRITE_Z;
    }
    if (color[3] < 0.999f) {
        state |= WOBY_GPU_STATE_BLEND_FUNC(
            WOBY_GPU_STATE_BLEND_SRC_ALPHA,
            WOBY_GPU_STATE_BLEND_INV_SRC_ALPHA);
    }

    return state;
}

void submitTriangleRange(
    woby::graphics::ViewId viewId,
    const GpuMesh& mesh,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::UniformHandle colorUniform,
    const float* model,
    const std::array<float, 4>& color,
    uint32_t indexOffset,
    uint32_t indexCount,
    bool markerIds,
    const std::array<float, 4>& uvGrid)
{
    if (!woby::graphics::isValid(mesh.vertexBuffer) || !woby::graphics::isValid(mesh.triangleIndexBuffer) || indexCount == 0) {
        return;
    }

    woby::graphics::setUniform(uvGridUniform, uvGrid.data());
    woby::graphics::setTransform(model);
    woby::graphics::setUniform(colorUniform, color.data());
    woby::graphics::setVertexBuffer(0, mesh.vertexBuffer);
    woby::graphics::setIndexBuffer(mesh.triangleIndexBuffer, indexOffset, indexCount);
    setMarkerRenderState(renderState(WOBY_GPU_STATE_DEPTH_TEST_LESS, true, color, 0u), markerIds);
    woby::graphics::submit(viewId, program);
}

void submitColorRange(
    woby::graphics::ViewId viewId,
    const GpuMesh& mesh,
    woby::graphics::IndexBufferHandle indexBuffer,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform,
    const float* model,
    const std::array<float, 4>& color,
    uint64_t primitiveState,
    uint32_t indexOffset,
    uint32_t indexCount,
    bool markerIds)
{
    if (!woby::graphics::isValid(mesh.vertexBuffer) || !woby::graphics::isValid(indexBuffer) || indexCount == 0) {
        return;
    }

    woby::graphics::setTransform(model);
    woby::graphics::setUniform(colorUniform, color.data());
    woby::graphics::setVertexBuffer(0, mesh.vertexBuffer);
    woby::graphics::setIndexBuffer(indexBuffer, indexOffset, indexCount);
    setMarkerRenderState(renderState(WOBY_GPU_STATE_DEPTH_TEST_ALWAYS, false, color, primitiveState), markerIds);
    woby::graphics::submit(viewId, program);
}

void submitPointSpriteRange(
    woby::graphics::ViewId viewId,
    const GpuMesh& mesh,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    const float* model,
    const std::array<float, 4>& color,
    float pointSize,
    uint32_t viewWidth,
    uint32_t viewHeight,
    uint32_t indexOffset,
    uint32_t indexCount,
    bool markerIds)
{
    if (!woby::graphics::isValid(mesh.vertexBuffer)
        || !woby::graphics::isValid(mesh.pointIdBuffer)
        || indexCount == 0) {
        return;
    }

    const auto pointParams = pointSpriteParameters(pointSize, viewWidth, viewHeight, indexOffset);
    woby::graphics::setTransform(model);
    woby::graphics::setUniform(colorUniform, color.data());
    woby::graphics::setUniform(pointParamsUniform, pointParams.data(), 2);
    woby::graphics::setBuffer(0, mesh.vertexBuffer, woby::graphics::Access::Read);
    woby::graphics::setBuffer(1, mesh.pointIdBuffer, woby::graphics::Access::Read);
    woby::graphics::setVertexCount(4);
    woby::graphics::setInstanceCount(indexCount);
    setMarkerRenderState(renderState(WOBY_GPU_STATE_DEPTH_TEST_LEQUAL, true, color, WOBY_GPU_STATE_PT_TRISTRIP), markerIds);
    woby::graphics::submit(viewId, program);
}

void submitHelperBuffer(
    woby::graphics::ViewId viewId,
    const woby::graphics::TransientVertexBuffer& vertexBuffer,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform,
    const std::array<float, 4>& color)
{
    float model[16];
    bx::mtxIdentity(model);
    woby::graphics::setTransform(model);
    woby::graphics::setUniform(colorUniform, color.data());
    woby::graphics::setVertexBuffer(0, &vertexBuffer);
    woby::graphics::setState(renderState(WOBY_GPU_STATE_DEPTH_TEST_ALWAYS, false, color, WOBY_GPU_STATE_PT_LINES));
    woby::graphics::submit(viewId, program);
}

void submitHelperLines(woby::graphics::ViewId viewId, std::span<const HelperLineVertex> vertices,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform, const std::array<float, 4>& color)
{
    if (vertices.empty() || vertices.size() % 2u != 0u) { return; }
    const auto count = sceneBufferBytes(vertices.size(), sizeof(HelperLineVertex)) / sizeof(HelperLineVertex);
    const auto vertexCount = static_cast<uint32_t>(count);
    if (woby::graphics::getAvailTransientVertexBuffer(vertexCount, layout) < vertexCount) { return; }
    woby::graphics::TransientVertexBuffer buffer;
    woby::graphics::allocTransientVertexBuffer(&buffer, vertexCount, layout);
    std::copy(vertices.begin(), vertices.end(), reinterpret_cast<HelperLineVertex*>(buffer.data));
    submitHelperBuffer(viewId, buffer, program, colorUniform, color);
}

} // namespace

woby::graphics::VertexLayout meshVertexLayout()
{
    return {static_cast<uint16_t>(sizeof(Vertex))};
}

woby::graphics::VertexLayout helperLineVertexLayout()
{
    return {static_cast<uint16_t>(3 * sizeof(float))};
}

uint8_t requestedGpuMeshFeatures(const UiFileState& file)
{
    uint8_t features = 0;
    if (!file.fileSettings.visible || file.fileSettings.opacity <= 0) { return features; }
    for (const auto& group : file.groupSettings) {
        if (!group.visible || group.opacity <= 0) { continue; }
        if (group.showTriangles) { features |= gpuMeshEdges; }
        if (group.showVertices) { features |= gpuMeshPoints; }
    }
    return features;
}

GpuMesh createGpuMesh(
    const Mesh& mesh,
    const woby::graphics::VertexLayout& meshLayout,
    uint8_t features)
{
    GpuMesh gpuMesh;
    const auto vertexBytes = sceneBufferBytes(mesh.vertices.size(), sizeof(Vertex));
    const auto indexBytes = sceneBufferBytes(mesh.indices.size(), sizeof(uint32_t));
    if (empty(mesh) || mesh.indices.size() % 3 != 0) {
        throw std::runtime_error("Scene needs a nonempty triangular mesh.");
    }
    for (const auto index : mesh.indices) {
        if (index >= mesh.vertices.size()) { throw std::runtime_error("Scene contains an invalid vertex index."); }
    }
    gpuMesh.nodeRanges.reserve(mesh.nodes.size());
    // Keep compact CPU point ranges for picking and geometry tooltips, even
    // when the compact GPU point-ID buffer has not been requested yet.
    gpuMesh.pointVertexIndices.reserve(mesh.vertices.size());
    std::vector<size_t> vertexGroups(mesh.vertices.size(), mesh.nodes.size());
    for (size_t nodeIndex = 0; nodeIndex < mesh.nodes.size(); ++nodeIndex) {
        const auto& node = mesh.nodes[nodeIndex];
        if (size_t(node.indexOffset) + node.indexCount > mesh.indices.size()
            || node.indexOffset % 3 != 0 || node.indexCount % 3 != 0) {
            throw std::runtime_error("Scene contains an invalid triangle range.");
        }
        GpuNodeRange range;
        range.triangleIndexOffset = node.indexOffset;
        range.triangleIndexCount = node.indexCount;
        range.lineIndexOffset = node.indexOffset * 2u;
        range.lineIndexCount = node.indexCount * 2u;
        range.pointIndexOffset = static_cast<uint32_t>(gpuMesh.pointVertexIndices.size());
        range.pointIndexCount = appendPointIndicesForRange(mesh.indices, node.indexOffset, node.indexCount,
            vertexGroups, nodeIndex, gpuMesh.pointVertexIndices);
        (void)sceneBufferBytes(gpuMesh.pointVertexIndices.size(), sizeof(uint32_t));
        gpuMesh.nodeRanges.push_back(range);
    }
    try {
        gpuMesh.vertexBuffer = woby::graphics::createVertexBuffer(
            woby::graphics::copy(mesh.vertices.data(), vertexBytes),
            meshLayout, WOBY_GPU_BUFFER_COMPUTE_READ);

        gpuMesh.triangleIndexBuffer = woby::graphics::createIndexBuffer(
            woby::graphics::copy(mesh.indices.data(), indexBytes),
            WOBY_GPU_BUFFER_INDEX32);
        if (!woby::graphics::isValid(gpuMesh.vertexBuffer) || !woby::graphics::isValid(gpuMesh.triangleIndexBuffer)) {
            throw std::runtime_error("Failed to allocate scene GPU buffers.");
        }
        prepareGpuMeshFeatures(gpuMesh, mesh, features);
    } catch (...) {
        destroyGpuMesh(gpuMesh);
        throw;
    }
    return gpuMesh;
}

void prepareGpuMeshFeatures(GpuMesh& gpuMesh, const Mesh& mesh,
    uint8_t features)
{
    if ((features & gpuMeshEdges) && !woby::graphics::isValid(gpuMesh.lineIndexBuffer)) {
        (void)sceneBufferBytes(mesh.indices.size(), 2 * sizeof(uint32_t));
        gpuMesh.lineIndexBuffer = woby::graphics::createIndexBuffer(
            ownedBuffer(buildLineIndices(mesh.indices)),
            WOBY_GPU_BUFFER_INDEX32);
        if (!woby::graphics::isValid(gpuMesh.lineIndexBuffer)) { throw std::runtime_error("Failed to allocate scene edge buffer."); }
    }
    if ((features & gpuMeshPoints) && !woby::graphics::isValid(gpuMesh.pointIdBuffer)) {
        const auto& pointIndices = gpuMesh.pointVertexIndices;
        if (pointIndices.empty()) { return; }
        // copy owns the asynchronous upload; CPU IDs remain available for picking.
        gpuMesh.pointIdBuffer = woby::graphics::createIndexBuffer(
            woby::graphics::copy(pointIndices.data(), sceneBufferBytes(pointIndices.size(), sizeof(uint32_t))),
            WOBY_GPU_BUFFER_INDEX32 | WOBY_GPU_BUFFER_COMPUTE_READ);
        if (!woby::graphics::isValid(gpuMesh.pointIdBuffer)) {
            throw std::runtime_error("Failed to allocate scene point-ID buffer.");
        }
    }
}

void destroyGpuMesh(GpuMesh& mesh)
{
    if (woby::graphics::isValid(mesh.pointIdBuffer)) {
        woby::graphics::destroy(mesh.pointIdBuffer);
    }
    if (woby::graphics::isValid(mesh.lineIndexBuffer)) {
        woby::graphics::destroy(mesh.lineIndexBuffer);
    }
    if (woby::graphics::isValid(mesh.triangleIndexBuffer)) {
        woby::graphics::destroy(mesh.triangleIndexBuffer);
    }
    if (woby::graphics::isValid(mesh.vertexBuffer)) {
        woby::graphics::destroy(mesh.vertexBuffer);
    }

    mesh.pointIdBuffer = WOBY_GPU_INVALID_HANDLE;
    mesh.lineIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    mesh.triangleIndexBuffer = WOBY_GPU_INVALID_HANDLE;
    mesh.vertexBuffer = WOBY_GPU_INVALID_HANDLE;
    mesh.nodeRanges.clear();
    mesh.pointVertexIndices.clear();
}

void destroyModelRuntimes(std::vector<LoadedModelRuntime>& runtimes)
{
    for (auto& runtime : runtimes) {
        destroyGpuMesh(runtime.gpuMesh);
    }
    runtimes.clear();
}

std::array<float, 8> pointSpriteParameters(
    float pointSize, uint32_t viewWidth, uint32_t viewHeight, uint32_t pointOffset)
{
    return {pointSize, static_cast<float>(std::max(viewWidth, 1u)),
        static_cast<float>(std::max(viewHeight, 1u)), 0.0f,
        static_cast<float>(pointOffset & 0xffffu), static_cast<float>(pointOffset >> 16u), 0.0f, 0.0f};
}

uint32_t vertexPointSize(float masterSize, float groupScale)
{
    const float scaledSize = std::clamp(
        masterSize * groupScale,
        minVertexPointSize,
        maxVertexPointSize);
    return static_cast<uint32_t>(std::lround(scaledSize));
}

void submitGroupRange(
    woby::graphics::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    size_t fileIndex,
    size_t nodeIndex,
    const float* parentModel,
    float opacityScale,
    float masterVertexPointSize,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight,
    MarkerDrawContext* markers)
{
    if (fileIndex >= files.size() || fileIndex >= runtimes.size()) {
        return;
    }

    const auto& file = files[fileIndex];
    const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
    if (nodeIndex >= file.groupSettings.size() || nodeIndex >= gpuMesh.nodeRanges.size()) {
        return;
    }

    const auto& settings = file.groupSettings[nodeIndex];
    if (!settings.visible) {
        return;
    }

    float groupModel[16];
    float model[16];
    groupTransformMatrix(settings, groupModel);
    bx::mtxMul(model, parentModel, groupModel);
    const auto& range = gpuMesh.nodeRanges[nodeIndex];
    if (settings.showSolidMesh) {
        submitTriangleRange(
            viewId,
            gpuMesh,
            meshProgram,
            uvGridUniform,
            colorUniform,
            model,
            groupColor(settings, 1.0f, opacityScale),
            range.triangleIndexOffset,
            range.triangleIndexCount, markers != nullptr,
            {settings.uvGrid.densityU, settings.uvGrid.densityV,
                settings.uvGrid.enabled && nodeIndex < file.mesh.nodes.size() && file.mesh.nodes[nodeIndex].hasTexcoords ? 1.0f : 0.0f, 0.0f});
    }
    if (settings.showTriangles) {
        submitColorRange(
            viewId,
            gpuMesh,
            gpuMesh.lineIndexBuffer,
            colorProgram,
            colorUniform,
            model,
            groupColor(settings, 1.25f, opacityScale),
            WOBY_GPU_STATE_PT_LINES,
            range.lineIndexOffset,
            range.lineIndexCount, markers != nullptr);
    }
    if (settings.showVertices) {
        const uint32_t pointSize = vertexPointSize(
            masterVertexPointSize,
            file.vertexSizeScale * settings.vertexSizeScale);
        if (markers) {
            MarkerDraw draw;
            std::copy_n(model, 16, draw.model.begin());
            draw.fileIndex = fileIndex; draw.fileId = file.objectId;
            draw.pointOffset = range.pointIndexOffset; draw.count = range.pointIndexCount;
            const auto id = appendMarkerDraw(markers->list, draw, static_cast<float>(pointSize));
            const std::array<float, 4> base = {static_cast<float>(id & 65535u), static_cast<float>(id >> 16u), 0, 0};
            woby::graphics::setUniform(markers->baseUniform, base.data());
        }
        submitPointSpriteRange(
            viewId,
            gpuMesh,
            pointSpriteProgram,
            colorUniform,
            pointParamsUniform,
            model,
            groupColor(settings, 1.5f, opacityScale),
            static_cast<float>(pointSize),
            sceneViewportWidth,
            viewportHeight,
            range.pointIndexOffset,
            range.pointIndexCount, markers != nullptr);
    }
}

void submitSceneNode(
    woby::graphics::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    const UiSceneNode& node,
    const float* parentModel,
    float parentOpacity,
    float masterVertexPointSize,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight,
    MarkerDrawContext* markers)
{
    (void)sceneNodes;
    if (node.kind == UiSceneNodeKind::folder) {
        if (!node.settings.visible) {
            return;
        }

        float nodeModel[16];
        float model[16];
        sceneNodeTransformMatrix(node.settings, nodeModel);
        bx::mtxMul(model, parentModel, nodeModel);
        const float opacity = parentOpacity * node.settings.opacity;
        for (const auto& child : node.children) {
            submitSceneNode(
                viewId,
                files,
                sceneNodes,
                runtimes,
                child,
                model,
                opacity,
                masterVertexPointSize,
                meshProgram,
                uvGridUniform,
                colorProgram,
                pointSpriteProgram,
                colorUniform,
                pointParamsUniform,
                sceneViewportWidth,
                viewportHeight, markers);
        }
        return;
    }

    if (node.kind == UiSceneNodeKind::file) {
        if (node.fileIndex >= files.size() || node.fileIndex >= runtimes.size()) {
            return;
        }

        const auto& file = files[node.fileIndex];
        if (!file.fileSettings.visible) {
            return;
        }

        float fileModel[16];
        float model[16];
        fileTransformMatrix(file.fileSettings, fileModel);
        bx::mtxMul(model, parentModel, fileModel);
        const float opacity = parentOpacity * file.fileSettings.opacity;
        if (node.children.empty()) {
            const size_t groupCount = std::min(file.groupSettings.size(), runtimes[node.fileIndex].gpuMesh.nodeRanges.size());
            for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
                submitGroupRange(
                    viewId,
                    files,
                    runtimes,
                    node.fileIndex,
                    groupIndex,
                    model,
                    opacity,
                    masterVertexPointSize,
                    meshProgram,
                    uvGridUniform,
                    colorProgram,
                    pointSpriteProgram,
                    colorUniform,
                    pointParamsUniform,
                    sceneViewportWidth,
                    viewportHeight, markers);
            }
            return;
        }

        for (const auto& child : node.children) {
            submitSceneNode(
                viewId,
                files,
                sceneNodes,
                runtimes,
                child,
                model,
                opacity,
                masterVertexPointSize,
                meshProgram,
                uvGridUniform,
                colorProgram,
                pointSpriteProgram,
                colorUniform,
                pointParamsUniform,
                sceneViewportWidth,
                viewportHeight, markers);
        }
        return;
    }

    submitGroupRange(
        viewId,
        files,
        runtimes,
        node.fileIndex,
        node.groupIndex,
        parentModel,
        parentOpacity,
        masterVertexPointSize,
        meshProgram,
        uvGridUniform,
        colorProgram,
        pointSpriteProgram,
        colorUniform,
        pointParamsUniform,
        sceneViewportWidth,
        viewportHeight, markers);
}

void submitSceneFiles(
    woby::graphics::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    float masterVertexPointSize,
    woby::graphics::ProgramHandle meshProgram,
    woby::graphics::UniformHandle uvGridUniform,
    woby::graphics::ProgramHandle colorProgram,
    woby::graphics::ProgramHandle pointSpriteProgram,
    woby::graphics::UniformHandle colorUniform,
    woby::graphics::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight,
    MarkerDrawContext* markers)
{
    float identity[16];
    bx::mtxIdentity(identity);
    if (!sceneNodes.empty()) {
        for (const auto& node : sceneNodes) {
            submitSceneNode(
                viewId,
                files,
                sceneNodes,
                runtimes,
                node,
                identity,
                1.0f,
                masterVertexPointSize,
                meshProgram,
                uvGridUniform,
                colorProgram,
                pointSpriteProgram,
                colorUniform,
                pointParamsUniform,
                sceneViewportWidth,
                viewportHeight, markers);
        }
        return;
    }

    const size_t renderFileCount = std::min(files.size(), runtimes.size());
    for (size_t fileIndex = 0; fileIndex < renderFileCount; ++fileIndex) {
        const auto& file = files[fileIndex];
        if (!file.fileSettings.visible) {
            continue;
        }

        float fileModel[16];
        fileTransformMatrix(file.fileSettings, fileModel);
        const float opacity = file.fileSettings.opacity;
        const size_t groupCount = std::min(file.groupSettings.size(), runtimes[fileIndex].gpuMesh.nodeRanges.size());
        for (size_t nodeIndex = 0; nodeIndex < groupCount; ++nodeIndex) {
            submitGroupRange(
                viewId,
                files,
                runtimes,
                fileIndex,
                nodeIndex,
                fileModel,
                opacity,
                masterVertexPointSize,
                meshProgram,
                uvGridUniform,
                colorProgram,
                pointSpriteProgram,
                colorUniform,
                pointParamsUniform,
                sceneViewportWidth,
                viewportHeight, markers);
        }
    }
}

void submitSceneSelection(woby::graphics::ViewId viewId, std::span<const ScenePickPart> parts,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform,
    SceneRenderScratch& scratch)
{
    sceneSelectionLines(parts, scratch.positions);
    submitHelperLines(viewId, scratch.positions, layout, program, colorUniform, {1.0f, .78f, .15f, 1.0f});
}

void submitSceneHelpers(
    woby::graphics::ViewId viewId,
    const UiState& state,
    const woby::graphics::VertexLayout& layout,
    woby::graphics::ProgramHandle program,
    woby::graphics::UniformHandle colorUniform)
{
    const auto grid = sceneGrid(state.sceneBounds, state.upAxis);
    const float spacing = grid.spacing;
    const int lineRadius = grid.radius;
    const float snappedExtent = grid.extent;

    if (state.showGrid) {
        const auto count = static_cast<uint32_t>((lineRadius * 2 + 1) * 4);
        if (woby::graphics::getAvailTransientVertexBuffer(count, layout) >= count) {
            woby::graphics::TransientVertexBuffer buffer;
            woby::graphics::allocTransientVertexBuffer(&buffer, count, layout);
            auto* vertex = reinterpret_cast<HelperLineVertex*>(buffer.data);
            for (int line = -lineRadius; line <= lineRadius; ++line) {
                const float offset = static_cast<float>(line) * spacing;
                if (state.upAxis == SceneUpAxis::y) {
                    *vertex++ = {offset, 0.0f, -snappedExtent};
                    *vertex++ = {offset, 0.0f, snappedExtent};
                    *vertex++ = {-snappedExtent, 0.0f, offset};
                    *vertex++ = {snappedExtent, 0.0f, offset};
                } else {
                    *vertex++ = {offset, -snappedExtent, 0.0f};
                    *vertex++ = {offset, snappedExtent, 0.0f};
                    *vertex++ = {-snappedExtent, offset, 0.0f};
                    *vertex++ = {snappedExtent, offset, 0.0f};
                }
            }
            submitHelperBuffer(viewId, buffer, program, colorUniform, {0.72f, 0.74f, 0.78f, 0.42f});
        }
    }

    if (state.showOrigin) {
        const float axisLength = std::max(spacing * 2.0f, snappedExtent * 0.18f);

        std::array<HelperLineVertex, 2> axisLine{};

        axisLine[1] = {axisLength, 0.0f, 0.0f};
        submitHelperLines(viewId, axisLine, layout, program, colorUniform, {1.0f, 0.20f, 0.20f, 1.0f});

        axisLine[1] = {0.0f, axisLength, 0.0f};
        submitHelperLines(viewId, axisLine, layout, program, colorUniform, {0.20f, 0.85f, 0.35f, 1.0f});

        axisLine[1] = {0.0f, 0.0f, axisLength};
        submitHelperLines(viewId, axisLine, layout, program, colorUniform, {0.30f, 0.55f, 1.0f, 1.0f});
    }
}

} // namespace woby
