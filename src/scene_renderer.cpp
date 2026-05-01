#include "scene_renderer.h"

#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>

namespace woby {
namespace {

struct PointSpriteVertex {
    std::array<float, 3> position{};
    std::array<float, 2> corner{};
};

struct HelperLineVertex {
    std::array<float, 3> position{};
};

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
    std::vector<uint32_t>& pointIndices)
{
    const uint32_t pointOffset = static_cast<uint32_t>(pointIndices.size());
    std::unordered_set<uint32_t> seen;
    seen.reserve(triangleIndexCount);

    const uint32_t endIndex = triangleIndexOffset + triangleIndexCount;
    for (uint32_t index = triangleIndexOffset; index < endIndex; ++index) {
        const uint32_t vertexIndex = triangleIndices[index];
        if (seen.insert(vertexIndex).second) {
            pointIndices.push_back(vertexIndex);
        }
    }

    return static_cast<uint32_t>(pointIndices.size()) - pointOffset;
}

void buildPointSprites(
    const Mesh& mesh,
    const std::vector<uint32_t>& pointIndices,
    std::vector<PointSpriteVertex>& vertices,
    std::vector<uint32_t>& indices)
{
    constexpr std::array<std::array<float, 2>, 4> corners = {{
        {-0.5f, -0.5f},
        {0.5f, -0.5f},
        {0.5f, 0.5f},
        {-0.5f, 0.5f},
    }};

    vertices.reserve(pointIndices.size() * corners.size());
    indices.reserve(pointIndices.size() * 6u);

    for (uint32_t pointIndex : pointIndices) {
        const uint32_t vertexBase = static_cast<uint32_t>(vertices.size());
        const auto& sourceVertex = mesh.vertices[pointIndex];
        for (const auto& corner : corners) {
            PointSpriteVertex vertex;
            vertex.position = sourceVertex.position;
            vertex.corner = corner;
            vertices.push_back(vertex);
        }

        indices.push_back(vertexBase + 0u);
        indices.push_back(vertexBase + 1u);
        indices.push_back(vertexBase + 2u);
        indices.push_back(vertexBase + 0u);
        indices.push_back(vertexBase + 2u);
        indices.push_back(vertexBase + 3u);
    }
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
    uint32_t indexCount)
{
    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(mesh.triangleIndexBuffer) || indexCount == 0) {
        return;
    }

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(mesh.triangleIndexBuffer, indexOffset, indexCount);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_LESS, true, color, 0u));
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
    uint32_t indexCount)
{
    if (!bgfx::isValid(mesh.vertexBuffer) || !bgfx::isValid(indexBuffer) || indexCount == 0) {
        return;
    }

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, mesh.vertexBuffer);
    bgfx::setIndexBuffer(indexBuffer, indexOffset, indexCount);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_ALWAYS, false, color, primitiveState));
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
    uint32_t indexCount)
{
    if (!bgfx::isValid(mesh.pointSpriteVertexBuffer)
        || !bgfx::isValid(mesh.pointSpriteIndexBuffer)
        || indexCount == 0) {
        return;
    }

    const std::array<float, 4> pointParams = {
        pointSize,
        static_cast<float>(std::max(viewWidth, 1u)),
        static_cast<float>(std::max(viewHeight, 1u)),
        0.0f,
    };

    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setUniform(pointParamsUniform, pointParams.data());
    bgfx::setVertexBuffer(0, mesh.pointSpriteVertexBuffer);
    bgfx::setIndexBuffer(mesh.pointSpriteIndexBuffer, indexOffset, indexCount);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_LEQUAL, true, color, 0u));
    bgfx::submit(viewId, program);
}

float helperGridExtent(const Bounds& bounds, SceneUpAxis upAxis)
{
    const float secondMin = upAxis == SceneUpAxis::y ? bounds.min[2] : bounds.min[1];
    const float secondMax = upAxis == SceneUpAxis::y ? bounds.max[2] : bounds.max[1];
    const float extent = std::max({
        std::abs(bounds.min[0]),
        std::abs(bounds.max[0]),
        std::abs(secondMin),
        std::abs(secondMax),
        defaultDisplayBoundsMax,
    });
    return std::max(extent, 0.001f);
}

float helperGridSpacing(float extent)
{
    constexpr float targetIntervals = 20.0f;
    const float rawSpacing = std::max((extent * 2.0f) / targetIntervals, 0.001f);
    const float magnitude = std::pow(10.0f, std::floor(std::log10(rawSpacing)));
    const float normalized = rawSpacing / magnitude;
    if (normalized <= 1.0f) {
        return magnitude;
    }
    if (normalized <= 2.0f) {
        return 2.0f * magnitude;
    }
    if (normalized <= 5.0f) {
        return 5.0f * magnitude;
    }

    return 10.0f * magnitude;
}

void appendHelperLine(
    std::vector<HelperLineVertex>& vertices,
    const std::array<float, 3>& start,
    const std::array<float, 3>& end)
{
    vertices.push_back({start});
    vertices.push_back({end});
}

void submitHelperLines(
    bgfx::ViewId viewId,
    const std::vector<HelperLineVertex>& vertices,
    const bgfx::VertexLayout& layout,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform,
    const std::array<float, 4>& color)
{
    if (vertices.empty() || vertices.size() % 2u != 0u) {
        return;
    }

    bgfx::TransientVertexBuffer vertexBuffer;
    const auto vertexCount = static_cast<uint32_t>(vertices.size());
    if (bgfx::getAvailTransientVertexBuffer(vertexCount, layout) < vertexCount) {
        return;
    }
    bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, layout);

    auto* destination = reinterpret_cast<HelperLineVertex*>(vertexBuffer.data);
    std::copy(vertices.begin(), vertices.end(), destination);

    float model[16];
    bx::mtxIdentity(model);
    bgfx::setTransform(model);
    bgfx::setUniform(colorUniform, color.data());
    bgfx::setVertexBuffer(0, &vertexBuffer);
    bgfx::setState(renderState(BGFX_STATE_DEPTH_TEST_ALWAYS, false, color, BGFX_STATE_PT_LINES));
    bgfx::submit(viewId, program);
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

bgfx::VertexLayout pointSpriteVertexLayout()
{
    bgfx::VertexLayout layout;
    layout
        .begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
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

GpuMesh createGpuMesh(
    const Mesh& mesh,
    const bgfx::VertexLayout& meshLayout,
    const bgfx::VertexLayout& pointSpriteLayout)
{
    GpuMesh gpuMesh;

    gpuMesh.vertexBuffer = bgfx::createVertexBuffer(
        bgfx::copy(mesh.vertices.data(), static_cast<uint32_t>(mesh.vertices.size() * sizeof(Vertex))),
        meshLayout);

    gpuMesh.triangleIndexBuffer = bgfx::createIndexBuffer(
        bgfx::copy(mesh.indices.data(), static_cast<uint32_t>(mesh.indices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    const std::vector<uint32_t> lineIndices = buildLineIndices(mesh.indices);
    gpuMesh.lineIndexBuffer = bgfx::createIndexBuffer(
        bgfx::copy(lineIndices.data(), static_cast<uint32_t>(lineIndices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    std::vector<uint32_t> pointIndices;
    const auto& nodes = mesh.nodes;
    gpuMesh.nodeRanges.reserve(nodes.size());
    for (const auto& node : nodes) {
        GpuNodeRange range;
        range.triangleIndexOffset = node.indexOffset;
        range.triangleIndexCount = node.indexCount;
        range.lineIndexOffset = (node.indexOffset / 3u) * 6u;
        range.lineIndexCount = (node.indexCount / 3u) * 6u;
        range.pointIndexOffset = static_cast<uint32_t>(pointIndices.size());
        range.pointIndexCount = appendPointIndicesForRange(
            mesh.indices,
            node.indexOffset,
            node.indexCount,
            pointIndices);
        range.pointSpriteIndexOffset = range.pointIndexOffset * 6u;
        range.pointSpriteIndexCount = range.pointIndexCount * 6u;
        gpuMesh.nodeRanges.push_back(range);
    }

    std::vector<PointSpriteVertex> pointSpriteVertices;
    std::vector<uint32_t> pointSpriteIndices;
    buildPointSprites(mesh, pointIndices, pointSpriteVertices, pointSpriteIndices);
    gpuMesh.pointVertexIndices = std::move(pointIndices);

    gpuMesh.pointSpriteVertexBuffer = bgfx::createVertexBuffer(
        bgfx::copy(
            pointSpriteVertices.data(),
            static_cast<uint32_t>(pointSpriteVertices.size() * sizeof(PointSpriteVertex))),
        pointSpriteLayout);
    gpuMesh.pointSpriteIndexBuffer = bgfx::createIndexBuffer(
        bgfx::copy(
            pointSpriteIndices.data(),
            static_cast<uint32_t>(pointSpriteIndices.size() * sizeof(uint32_t))),
        BGFX_BUFFER_INDEX32);

    return gpuMesh;
}

void destroyGpuMesh(GpuMesh& mesh)
{
    if (bgfx::isValid(mesh.pointSpriteIndexBuffer)) {
        bgfx::destroy(mesh.pointSpriteIndexBuffer);
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
    if (bgfx::isValid(mesh.pointSpriteVertexBuffer)) {
        bgfx::destroy(mesh.pointSpriteVertexBuffer);
    }

    mesh.pointSpriteIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.lineIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.triangleIndexBuffer = BGFX_INVALID_HANDLE;
    mesh.vertexBuffer = BGFX_INVALID_HANDLE;
    mesh.pointSpriteVertexBuffer = BGFX_INVALID_HANDLE;
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

uint32_t vertexPointSize(float masterSize, float groupScale)
{
    const float scaledSize = std::clamp(
        masterSize * groupScale,
        minVertexPointSize,
        maxVertexPointSize);
    return static_cast<uint32_t>(std::lround(scaledSize));
}

void submitSceneFiles(
    bgfx::ViewId viewId,
    const std::vector<UiFileState>& files,
    const std::vector<LoadedModelRuntime>& runtimes,
    float masterVertexPointSize,
    bgfx::ProgramHandle meshProgram,
    bgfx::ProgramHandle colorProgram,
    bgfx::ProgramHandle pointSpriteProgram,
    bgfx::UniformHandle colorUniform,
    bgfx::UniformHandle pointParamsUniform,
    uint32_t sceneViewportWidth,
    uint32_t viewportHeight)
{
    const size_t renderFileCount = std::min(files.size(), runtimes.size());
    for (size_t fileIndex = 0; fileIndex < renderFileCount; ++fileIndex) {
        const auto& file = files[fileIndex];
        const auto& gpuMesh = runtimes[fileIndex].gpuMesh;
        if (!file.fileSettings.visible) {
            continue;
        }

        float fileModel[16];
        fileTransformMatrix(file.fileSettings, fileModel);
        for (size_t nodeIndex = 0; nodeIndex < gpuMesh.nodeRanges.size(); ++nodeIndex) {
            const auto& settings = file.groupSettings[nodeIndex];
            if (!settings.visible) {
                continue;
            }

            float groupModel[16];
            float model[16];
            groupTransformMatrix(settings, groupModel);
            bx::mtxMul(model, fileModel, groupModel);
            const auto& range = gpuMesh.nodeRanges[nodeIndex];
            if (settings.showSolidMesh) {
                submitTriangleRange(
                    viewId,
                    gpuMesh,
                    meshProgram,
                    colorUniform,
                    model,
                    groupColor(settings, 1.0f, file.fileSettings.opacity),
                    range.triangleIndexOffset,
                    range.triangleIndexCount);
            }
            if (settings.showTriangles) {
                submitColorRange(
                    viewId,
                    gpuMesh,
                    gpuMesh.lineIndexBuffer,
                    colorProgram,
                    colorUniform,
                    model,
                    groupColor(settings, 1.25f, file.fileSettings.opacity),
                    BGFX_STATE_PT_LINES,
                    range.lineIndexOffset,
                    range.lineIndexCount);
            }
            if (settings.showVertices) {
                const uint32_t pointSize = vertexPointSize(
                    masterVertexPointSize,
                    file.vertexSizeScale * settings.vertexSizeScale);
                submitPointSpriteRange(
                    viewId,
                    gpuMesh,
                    pointSpriteProgram,
                    colorUniform,
                    pointParamsUniform,
                    model,
                    groupColor(settings, 1.5f, file.fileSettings.opacity),
                    static_cast<float>(pointSize),
                    sceneViewportWidth,
                    viewportHeight,
                    range.pointSpriteIndexOffset,
                    range.pointSpriteIndexCount);
            }
        }
    }
}

void submitSceneHelpers(
    bgfx::ViewId viewId,
    const UiState& state,
    const bgfx::VertexLayout& layout,
    bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform)
{
    const float extent = helperGridExtent(state.sceneBounds, state.upAxis);
    const float spacing = helperGridSpacing(extent);
    const int lineRadius = std::max(1, static_cast<int>(std::ceil(extent / spacing)));
    const float snappedExtent = static_cast<float>(lineRadius) * spacing;

    if (state.showGrid) {
        std::vector<HelperLineVertex> gridLines;
        gridLines.reserve(static_cast<size_t>(lineRadius * 4 + 2) * 2u);
        for (int line = -lineRadius; line <= lineRadius; ++line) {
            const float offset = static_cast<float>(line) * spacing;
            if (state.upAxis == SceneUpAxis::y) {
                appendHelperLine(gridLines, {offset, 0.0f, -snappedExtent}, {offset, 0.0f, snappedExtent});
                appendHelperLine(gridLines, {-snappedExtent, 0.0f, offset}, {snappedExtent, 0.0f, offset});
            } else {
                appendHelperLine(gridLines, {offset, -snappedExtent, 0.0f}, {offset, snappedExtent, 0.0f});
                appendHelperLine(gridLines, {-snappedExtent, offset, 0.0f}, {snappedExtent, offset, 0.0f});
            }
        }
        submitHelperLines(viewId, gridLines, layout, program, colorUniform, {0.72f, 0.74f, 0.78f, 0.42f});
    }

    if (state.showOrigin) {
        const float axisLength = std::max(spacing * 2.0f, snappedExtent * 0.18f);

        std::vector<HelperLineVertex> axisLine;
        axisLine.reserve(2u);

        appendHelperLine(axisLine, {0.0f, 0.0f, 0.0f}, {axisLength, 0.0f, 0.0f});
        submitHelperLines(viewId, axisLine, layout, program, colorUniform, {1.0f, 0.20f, 0.20f, 1.0f});

        axisLine.clear();
        appendHelperLine(axisLine, {0.0f, 0.0f, 0.0f}, {0.0f, axisLength, 0.0f});
        submitHelperLines(viewId, axisLine, layout, program, colorUniform, {0.20f, 0.85f, 0.35f, 1.0f});

        axisLine.clear();
        appendHelperLine(axisLine, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, axisLength});
        submitHelperLines(viewId, axisLine, layout, program, colorUniform, {0.30f, 0.55f, 1.0f, 1.0f});
    }
}

} // namespace woby
