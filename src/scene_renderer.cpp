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
const bgfx::Memory* ownedBuffer(std::vector<T> values)
{
    const auto bytes = sceneBufferBytes(values.size(), sizeof(T));
    auto owner = std::make_unique<std::vector<T>>(std::move(values));
    // bgfx releases this allocation on its render thread after consuming it.
    // The callback owns only immutable upload data, never UI state or a mesh.
    const auto* memory = bgfx::makeRef(owner->data(), bytes, [](void*, void* data) {
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
    uint64_t state = BGFX_STATE_WRITE_RGB
        | BGFX_STATE_WRITE_A
        | depthTest
        | BGFX_STATE_MSAA
        | primitiveState;

    if (writeDepth && color[3] >= 0.999f) {
        state |= BGFX_STATE_WRITE_Z;
    }
    if (color[3] < 0.999f) {
        state |= BGFX_STATE_BLEND_FUNC(
            BGFX_STATE_BLEND_SRC_ALPHA,
            BGFX_STATE_BLEND_INV_SRC_ALPHA);
    }

    return state;
}

void submitTriangleRange(
    bgfx::ViewId viewId,
    const GpuMesh& mesh,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const float* model,
    const std::array<float, 4>& color,
    uint32_t indexOffset,
    uint32_t indexCount,
    bool markerIds)
{
    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(mesh.triangleIndexBuffer) || indexCount == 0) {
        return;
    }

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.triangleIndexBuffer, indexOffset, indexCount);
    setMarkerRenderState(renderState(BGFX_STATE_DEPTH_TEST_LESS, true, color, 0u), markerIds);
    bgfx::submit(viewId, program);
}

void submitColorRange(
    bgfx::ViewId viewId,
    const GpuMesh& mesh,
    bgfx::IndexBufferHandle indexBuffer,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const float* model,
    const std::array<float, 4>& color,
    uint64_t primitiveState,
    uint32_t indexOffset,
    uint32_t indexCount,
    bool markerIds)
{
    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(indexBuffer) || indexCount == 0) {
        return;
    }

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(indexBuffer, indexOffset, indexCount);
    setMarkerRenderState(renderState(BGFX_STATE_DEPTH_TEST_ALWAYS, false, color, primitiveState), markerIds);
    bgfx::submit(viewId, program);
}

void submitPointSpriteRange(
    bgfx::ViewId viewId,
    const GpuMesh& mesh,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
    const float* model,
    const std::array<float, 4>& color,
    float pointSize,
    uint32_t viewWidth,
    uint32_t viewHeight,
    uint32_t indexOffset,
    uint32_t indexCount,
    bool markerIds)
{
    if (!bgfx::isValid(mesh.vertexBuffer)
        || !bgfx::isValid(mesh.pointIdBuffer)
        || indexCount == 0) {
        return;
    }

    const auto pointParams = pointSpriteParameters(pointSize, viewWidth, viewHeight, indexOffset);
    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setUniform(pointParamsUniform, pointParams.data(), 2);
    bgfx::setBuffer(0, mesh.vertexBuffer, bgfx::Access::Read);
    bgfx::setBuffer(1, mesh.pointIdBuffer, bgfx::Access::Read);
    bgfx::setVertexCount(4);
    bgfx::setInstanceCount(indexCount);
    setMarkerRenderState(renderState(BGFX_STATE_DEPTH_TEST_LEQUAL, true, color, BGFX_STATE_PT_TRISTRIP), markerIds);
    bgfx::submit(viewId, program);
}

void submitHelperBuffer(
    bgfx::ViewId viewId,
    const bgfx::TransientVertexBuffer& vertexBuffer,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const std::array<float, 4>& color)
{
    float model[16];
    bx::mtxIdentity(model);
    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, &vertexBuffer);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_ALWAYS, false, color, BGFX_STATE_PT_LINES));
    bgfx::submit(viewId, program);
}

void submitHelperLines(bgfx::ViewId viewId, std::span<const HelperLineVertex> vertices,
    const bgfx::VertexLayout& layout, bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform, const std::array<float, 4>& color)
{
    if (vertices.empty() || vertices.size() % 2u != 0u) { return; }
    const auto count = sceneBufferBytes(vertices.size(), sizeof(HelperLineVertex)) / sizeof(HelperLineVertex);
    const auto vertexCount = static_cast<uint32_t>(count);
    if (bgfx::getAvailTransientVertexBuffer(vertexCount, layout) < vertexCount) { return; }
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, vertexCount, layout);
    std::copy(vertices.begin(), vertices.end(), reinterpret_cast<HelperLineVertex*>(buffer.data));
    submitHelperBuffer(viewId, buffer, program, colorUniform, color);
}

} // namespace

bgfx::VertexLayout meshVertexLayout()
{
    bgfx::VertexLayout layout;
    layout
        .begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    return layout;
}

bgfx::VertexLayout helperLineVertexLayout()
{
    bgfx::VertexLayout layout;
    layout
        .begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .end();
    return layout;
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
    const bgfx::VertexLayout& meshLayout,
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
        gpuMesh.vertexBuffer = bgfx::createVertexBuffer(
            bgfx::copy(mesh.vertices.data(), vertexBytes),
            meshLayout, BGFX_BUFFER_COMPUTE_READ);

        gpuMesh.triangleIndexBuffer = bgfx::createIndexBuffer(
            bgfx::copy(mesh.indices.data(), indexBytes),
            BGFX_BUFFER_INDEX32);
        if (!bgfx::isValid(gpuMesh.vertexBuffer) || !bgfx::isValid(gpuMesh.triangleIndexBuffer)) {
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
    if ((features & gpuMeshEdges) && !bgfx::isValid(gpuMesh.lineIndexBuffer)) {
        (void)sceneBufferBytes(mesh.indices.size(), 2 * sizeof(uint32_t));
        gpuMesh.lineIndexBuffer = bgfx::createIndexBuffer(
            ownedBuffer(buildLineIndices(mesh.indices)),
            BGFX_BUFFER_INDEX32);
        if (!bgfx::isValid(gpuMesh.lineIndexBuffer)) { throw std::runtime_error("Failed to allocate scene edge buffer."); }
    }
    if ((features & gpuMeshPoints) && !bgfx::isValid(gpuMesh.pointIdBuffer)) {
        const auto& pointIndices = gpuMesh.pointVertexIndices;
        if (pointIndices.empty()) { return; }
        // copy owns the asynchronous upload; CPU IDs remain available for picking.
        gpuMesh.pointIdBuffer = bgfx::createIndexBuffer(
            bgfx::copy(pointIndices.data(), sceneBufferBytes(pointIndices.size(), sizeof(uint32_t))),
            BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
        if (!bgfx::isValid(gpuMesh.pointIdBuffer)) {
            throw std::runtime_error("Failed to allocate scene point-ID buffer.");
        }
    }
}

void destroyGpuMesh(GpuMesh& mesh)
{
    if (bgfx::isValid(mesh.pointIdBuffer)) {
        bgfx::destroy(mesh.pointIdBuffer);
    }
    if (bgfx::isValid(mesh.lineIndexBuffer)) {
        bgfx::destroy(mesh.lineIndexBuffer);
    }
    if (bgfx::isValid(mesh.triangleIndexBuffer)) {
        bgfx::destroy(mesh.triangleIndexBuffer);
    }
    if (bgfx::isValid(mesh.vertexBuffer)) {
        bgfx::destroy(mesh.vertexBuffer);
    }

    mesh.pointIdBuffer = BGFX_INVALID_HANDLE;
    mesh.lineIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.triangleIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.vertexBuffer = BGFX_INVALID_HANDLE;
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
    bgfx::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    size_t fileIndex,
    size_t nodeIndex,
    const float* parentModel,
    float opacityScale,
    float masterVertexPointSize,
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
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
            colorUniform,
            model,
            groupColor(settings, 1.0f, opacityScale),
            range.triangleIndexOffset,
            range.triangleIndexCount, markers != nullptr);
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
            BGFX_STATE_PT_LINES,
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
            bgfx::setUniform(markers->baseUniform, base.data());
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
    bgfx::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    const UiSceneNode& node,
    const float* parentModel,
    float parentOpacity,
    float masterVertexPointSize,
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
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
        colorProgram,
        pointSpriteProgram,
        colorUniform,
        pointParamsUniform,
        sceneViewportWidth,
        viewportHeight, markers);
}

void submitSceneFiles(
    bgfx::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes,
    const std::vector<LoadedModelRuntime>& runtimes,
    float masterVertexPointSize,
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
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
                colorProgram,
                pointSpriteProgram,
                colorUniform,
                pointParamsUniform,
                sceneViewportWidth,
                viewportHeight, markers);
        }
    }
}

void submitSceneSelection(bgfx::ViewId viewId, std::span<const ScenePickPart> parts,
    const bgfx::VertexLayout& layout, bgfx::ProgramHandle program, bgfx::UniformHandle colorUniform,
    SceneRenderScratch& scratch)
{
    sceneSelectionLines(parts, scratch.positions);
    submitHelperLines(viewId, scratch.positions, layout, program, colorUniform, {1.0f, .78f, .15f, 1.0f});
}

void submitSceneHelpers(
    bgfx::ViewId viewId,
    const UiState& state,
    const bgfx::VertexLayout& layout,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform)
{
    const auto grid = sceneGrid(state.sceneBounds, state.upAxis);
    const float spacing = grid.spacing;
    const int lineRadius = grid.radius;
    const float snappedExtent = grid.extent;

    if (state.showGrid) {
        const auto count = static_cast<uint32_t>((lineRadius * 2 + 1) * 4);
        if (bgfx::getAvailTransientVertexBuffer(count, layout) >= count) {
            bgfx::TransientVertexBuffer buffer;
            bgfx::allocTransientVertexBuffer(&buffer, count, layout);
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
