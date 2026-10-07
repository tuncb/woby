#include "scene_renderer.h"
#include "scene_buffer_size.h"
#include "marker_pick.h"
#include "scene_dimensions.h"
#include "graphics_helpers.h"
#include "adaptive_points.h"

#include <bx/math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>

namespace woby {
namespace {

// point_sprite.vert.sc reads each mesh vertex as two vec4 records.
static_assert(sizeof(Vertex) == 32 && offsetof(Vertex, position) == 0);

using HelperLineVertex = std::array<float, 3>;

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
    const std::array<float, 4>& uvGrid,
    const TransparentSurfacePrograms* transparency)
{
    if (!woby::graphics::isValid(mesh.vertexBuffer) || !woby::graphics::isValid(mesh.triangleIndexBuffer) || indexCount == 0) {
        return;
    }

    woby::graphics::setUniform(uvGridUniform, uvGrid.data());
    woby::graphics::setTransform(model);
    woby::graphics::setUniform(colorUniform, color.data());
    woby::graphics::setVertexBuffer(0, mesh.vertexBuffer);
    woby::graphics::setIndexBuffer(mesh.triangleIndexBuffer, indexOffset, indexCount);
    const auto state = renderState(WOBY_GPU_STATE_DEPTH_TEST_LESS, true, color, 0u);
    if (transparency && color[3] < .999f) {
        graphics::setState(state & ~WOBY_GPU_STATE_BLEND_ALPHA);
        graphics::submitTransparent(viewId, transparency->mesh, transparency->resolve);
    } else {
        setMarkerRenderState(state, markerIds);
        graphics::submit(viewId, program);
    }
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

GpuMeshUpload beginGpuMeshUpload(SceneMeshPreparation prepared)
{
    GpuMeshUpload upload;
    upload.mesh.nodeRanges = std::move(prepared.nodeRanges);
    upload.mesh.pointVertexIndices = std::move(prepared.pointVertexIndices);
    upload.mesh.pointCloud = std::move(prepared.pointCloud);
    upload.mesh.compactOnly = prepared.compactOnly;
    upload.edgeIndices = std::move(prepared.edgeIndices);
    upload.features = prepared.features;
    upload.totalBytes = prepared.uploadBytes;
    return upload;
}

bool stepGpuMeshUpload(GpuMeshUpload& upload, const Mesh& source,
    const woby::graphics::VertexLayout& layout, uint32_t byteBudget)
{
    if (byteBudget < sizeof(Vertex)) { throw std::invalid_argument("Mesh upload budget must fit one vertex."); }
    // Keep chunks aligned for both Vulkan copies and vertex-buffer strides.
    byteBudget -= byteBudget % sizeof(Vertex);
    while (upload.bufferIndex < 7) {
        if (upload.bufferIndex >= 5) {
            if (!upload.mesh.pointCloud) { ++upload.bufferIndex; continue; }
            const bool proxy = upload.bufferIndex == 6;
            const auto& points = proxy ? upload.mesh.pointCloud->proxies : upload.mesh.pointCloud->points;
            auto& chunks = proxy ? upload.mesh.proxyChunks : upload.mesh.pointChunks;
            const size_t begin = size_t(upload.pointChunk) * pointChunkSize;
            if (begin >= points.size()) { ++upload.bufferIndex; upload.pointChunk = 0; continue; }
            if (byteBudget < sizeof(Vertex)) { return false; }
            const auto bytes = sceneBufferBytes(std::min<size_t>(pointChunkSize, points.size() - begin), sizeof(points::Point));
            if (!upload.bufferOffset) { chunks.push_back(graphics::createVertexBufferStorage(bytes, {sizeof(points::Point)})); }
            const auto count = std::min(bytes - upload.bufferOffset, byteBudget);
            graphics::uploadBufferRange(chunks.back(), upload.bufferOffset,
                reinterpret_cast<const uint8_t*>(points.data() + begin) + upload.bufferOffset, count);
            upload.bufferOffset += count; upload.uploadedBytes += count; byteBudget -= count;
            if (upload.bufferOffset == bytes) { upload.bufferOffset = 0; ++upload.pointChunk; }
            continue;
        }
        const void* data = nullptr;
        uint32_t bytes = 0;
        woby::graphics::IndexBufferHandle* indexBuffer = nullptr;
        uint16_t flags = WOBY_GPU_BUFFER_INDEX32;
        switch (upload.bufferIndex) {
        case 0:
            data = source.vertices.data();
            bytes = upload.mesh.compactOnly ? 0 : sceneBufferBytes(source.vertices.size(), sizeof(Vertex));
            break;
        case 1:
            data = source.indices.data();
            bytes = sceneBufferBytes(source.indices.size(), sizeof(uint32_t));
            indexBuffer = &upload.mesh.triangleIndexBuffer;
            flags |= WOBY_GPU_BUFFER_COMPUTE_READ;
            break;
        case 2:
            data = source.lineIndices.data();
            bytes = sceneBufferBytes(source.lineIndices.size(), sizeof(uint32_t));
            indexBuffer = &upload.mesh.importedLineBuffer;
            flags |= WOBY_GPU_BUFFER_COMPUTE_READ;
            break;
        case 3:
            data = upload.edgeIndices.data();
            bytes = sceneBufferBytes(upload.edgeIndices.size(), sizeof(uint32_t));
            indexBuffer = &upload.mesh.lineIndexBuffer;
            break;
        case 4:
            data = upload.mesh.pointVertexIndices.data();
            if ((upload.features & gpuMeshPoints) && !upload.mesh.pointCloud) {
                bytes = sceneBufferBytes(upload.mesh.pointVertexIndices.size(), sizeof(uint32_t));
            }
            indexBuffer = &upload.mesh.pointIdBuffer;
            flags |= WOBY_GPU_BUFFER_COMPUTE_READ;
            break;
        }
        if (bytes == upload.bufferOffset) {
            ++upload.bufferIndex;
            upload.bufferOffset = 0;
            continue;
        }
        if (byteBudget < sizeof(Vertex)) { return false; }
        if (upload.bufferOffset == 0) {
            if (indexBuffer) { *indexBuffer = woby::graphics::createIndexBufferStorage(bytes, flags); }
            else { upload.mesh.vertexBuffer = woby::graphics::createVertexBufferStorage(bytes, layout); }
        }
        const uint32_t count = std::min(bytes - upload.bufferOffset, byteBudget);
        const auto* chunk = static_cast<const uint8_t*>(data) + upload.bufferOffset;
        if (indexBuffer) { woby::graphics::uploadBufferRange(*indexBuffer, upload.bufferOffset, chunk, count); }
        else { woby::graphics::uploadBufferRange(upload.mesh.vertexBuffer, upload.bufferOffset, chunk, count); }
        upload.bufferOffset += count;
        upload.uploadedBytes += count;
        byteBudget -= count;
    }
    return true;
}

void abortGpuMeshUpload(GpuMeshUpload& upload)
{
    destroyGpuMesh(upload.mesh);
    upload = {};
}

GpuMesh createGpuMesh(const Mesh& mesh, const woby::graphics::VertexLayout& meshLayout, uint8_t features)
{
    auto upload = beginGpuMeshUpload(*prepareSceneMesh(mesh, features));
    try {
        while (!stepGpuMeshUpload(upload, mesh, meshLayout, 4u * 1024 * 1024)) {}
    } catch (...) {
        abortGpuMeshUpload(upload);
        throw;
    }
    return std::move(upload.mesh);
}

void prepareGpuMeshFeatures(GpuMesh& gpuMesh, const Mesh& mesh,
    uint8_t features)
{
    gpuMesh.resourceRevision = nextMeshContentRevision();
    if ((features & gpuMeshEdges) && !mesh.indices.empty() && !woby::graphics::isValid(gpuMesh.lineIndexBuffer)) {
        (void)sceneBufferBytes(mesh.indices.size(), 2 * sizeof(uint32_t));
        gpuMesh.lineIndexBuffer = woby::graphics::createIndexBuffer(
            ownedBuffer(buildLineIndices(mesh.indices)),
            WOBY_GPU_BUFFER_INDEX32);
        if (!woby::graphics::isValid(gpuMesh.lineIndexBuffer)) { throw std::runtime_error("Failed to allocate scene edge buffer."); }
    }
    if ((features & gpuMeshPoints) && !gpuMesh.pointCloud && !woby::graphics::isValid(gpuMesh.pointIdBuffer)) {
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
    mesh.resourceRevision = nextMeshContentRevision();
    for (auto& chunks : {&mesh.pointChunks, &mesh.proxyChunks}) {
        for (const auto buffer : *chunks) { if (graphics::isValid(buffer)) graphics::destroy(buffer); }
        chunks->clear();
    }
    mesh.pointCloud.reset();
    mesh.compactOnly = false;
    if (woby::graphics::isValid(mesh.importedLineBuffer)) { woby::graphics::destroy(mesh.importedLineBuffer); }
    mesh.importedLineBuffer = WOBY_GPU_INVALID_HANDLE;
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

TriangleEdgePrograms createTriangleEdgePrograms(const std::filesystem::path& assets, bool forceVertexPulling)
{
    TriangleEdgePrograms result;
    result.nativeBarycentrics = !forceVertexPulling
        && (graphics::getCaps()->supported & WOBY_GPU_CAPS_FRAGMENT_BARYCENTRIC) != 0;
    try {
        const auto* vertex = result.nativeBarycentrics ? "vs_mesh.bin" : "vs_mesh_edges.bin";
        result.surface = loadProgram(assets, vertex,
            result.nativeBarycentrics ? "fs_mesh_edges.bin" : "fs_mesh_edges_pulled.bin");
        result.markerSurface = loadProgram(assets, vertex,
            result.nativeBarycentrics ? "fs_marker_mesh_edges.bin" : "fs_marker_mesh_edges_pulled.bin");
        result.transparentSurface = loadProgram(assets, vertex,
            result.nativeBarycentrics ? "fs_transparent_edges.bin" : "fs_transparent_edges_pulled.bin");
        result.lines = loadProgram(assets, "vs_triangle_lines.bin", "fs_color.bin");
        result.markerLines = loadProgram(assets, "vs_triangle_lines.bin", "fs_marker_line.bin");
        result.parameters = graphics::createUniform("u_triangleEdges", graphics::UniformType::Vec4);
    } catch (...) { destroyTriangleEdgePrograms(result); throw; }
    return result;
}

void destroyTriangleEdgePrograms(TriangleEdgePrograms& programs)
{
    for (auto handle : {programs.surface, programs.markerSurface, programs.transparentSurface, programs.lines, programs.markerLines}) {
        if (graphics::isValid(handle)) { graphics::destroy(handle); }
    }
    if (graphics::isValid(programs.parameters)) { graphics::destroy(programs.parameters); }
    programs = {};
}

TransparentSurfacePrograms createTransparentSurfacePrograms(const std::filesystem::path& assets)
{
    TransparentSurfacePrograms result;
    try {
        result.mesh = loadProgram(assets, "vs_mesh.bin", "fs_transparent_mesh.bin");
        result.resolve[0] = loadProgram(assets, "vs_marker_screen.bin", "fs_transparency_resolve_single.bin");
        result.resolve[1] = loadProgram(assets, "vs_marker_screen.bin", "fs_transparency_resolve_msaa.bin");
    } catch (...) { destroyTransparentSurfacePrograms(result); throw; }
    return result;
}

void destroyTransparentSurfacePrograms(TransparentSurfacePrograms& programs)
{
    for (auto handle : {programs.mesh, programs.resolve[0], programs.resolve[1]}) {
        if (graphics::isValid(handle)) { graphics::destroy(handle); }
    }
    programs = {};
}

void submitSceneFiles(
    graphics::ViewId viewId, const SceneDrawPlan& plan, const std::vector<LoadedModelRuntime>& runtimes,
    graphics::ProgramHandle meshProgram, graphics::UniformHandle uvGridUniform,
    graphics::ProgramHandle colorProgram, graphics::ProgramHandle pointSpriteProgram,
    graphics::UniformHandle colorUniform, graphics::UniformHandle pointParamsUniform,
    const TriangleEdgePrograms& edges, uint32_t width, uint32_t height,
    MarkerDrawContext* markers, bool importedLinesOnly, AdaptivePointRuntime* adaptivePoints,
    const TransparentSurfacePrograms* transparency)
{
    const bool markerIds = markers != nullptr;
    MarkerDrawList localMarkers;
    const auto each = [&](const auto& draw) {
        for (const auto& item : plan.items) {
            if (item.importedLines != importedLinesOnly || item.fileIndex >= runtimes.size()) { continue; }
            const auto& mesh = runtimes[item.fileIndex].gpuMesh;
            if (item.groupIndex >= mesh.nodeRanges.size() || (!graphics::isValid(mesh.vertexBuffer) && !mesh.pointCloud)) { continue; }
            draw(item, mesh, mesh.nodeRanges[item.groupIndex]);
        }
    };
    const auto points = [&](const SceneDrawItem& item, const GpuMesh& mesh, const GpuNodeRange& range) {
        if (!item.points || !range.pointIndexCount || (!mesh.pointCloud && !graphics::isValid(mesh.pointIdBuffer))) { return; }
        auto color = scaledRgbColor(item.color, 1.5f);
        color[3] = 1.0f;
        uint32_t firstId=0;
        if (markers || mesh.pointCloud) {
            MarkerDraw draw;
            draw.model = item.model; draw.fileIndex = item.fileIndex; draw.fileId = item.fileId;
            draw.pointOffset = range.pointIndexOffset; draw.count = range.pointIndexCount;
            firstId = appendMarkerDraw(markers ? markers->list : localMarkers, draw, item.pointSize);
            if (markers) {
                const std::array<float, 4> base{float(firstId & 65535u), float(firstId >> 16u), 0, 0};
                graphics::setUniform(markers->baseUniform, base.data());
            }
        }
        if (adaptivePoints && queueAdaptivePoints(*adaptivePoints,mesh,item,firstId)) return;
        if (mesh.pointCloud) {
            for (const auto root:mesh.pointCloud->roots) {
                const auto& node=mesh.pointCloud->nodes[root];
                if (node.group!=item.groupIndex) continue;
                for (uint32_t used=0;used<node.count;) {
                    const auto offset=node.begin+used, chunk=offset/pointChunkSize;
                    const auto count=std::min(node.count-used,pointChunkSize-offset%pointChunkSize);
                    auto params=pointSpriteParameters(item.pointSize,width,height,offset%pointChunkSize);
                    params[3]=1;
                    const auto sourceFirst=range.pointIndexOffset+1;
                    params[6]=float(sourceFirst&65535u); params[7]=float(sourceFirst>>16u);
                    graphics::setTransform(item.model.data()); graphics::setUniform(colorUniform,color.data());
                    graphics::setUniform(pointParamsUniform,params.data(),2);
                    if (markers) {
                        const std::array<float,4> base{float(firstId&65535u),float(firstId>>16u),0,0};
                        graphics::setUniform(markers->baseUniform,base.data());
                    }
                    graphics::setBuffer(0,mesh.pointChunks.at(chunk),graphics::Access::Read);
                    graphics::setVertexCount(4); graphics::setInstanceCount(count);
                    setMarkerRenderState(renderState(WOBY_GPU_STATE_DEPTH_TEST_LEQUAL,true,color,WOBY_GPU_STATE_PT_TRISTRIP),markerIds);
                    graphics::submit(viewId,pointSpriteProgram); used+=count;
                }
            }
            return;
        }
        submitPointSpriteRange(viewId, mesh, pointSpriteProgram, colorUniform, pointParamsUniform,
            item.model.data(), color, item.pointSize, width, height,
            range.pointIndexOffset, range.pointIndexCount, markerIds);
    };
    if (importedLinesOnly) {
        each([&](const auto& item, const auto& mesh, const auto& range) {
            if (item.color[3] > 0 && item.lineIndexCount && graphics::isValid(mesh.importedLineBuffer)) {
                const auto params = pointSpriteParameters(item.lineWidth, width, height, item.lineIndexOffset);
                graphics::setTransform(item.model.data());
                graphics::setUniform(colorUniform, item.color.data());
                graphics::setUniform(pointParamsUniform, params.data(), 2);
                graphics::setBuffer(0, mesh.vertexBuffer, graphics::Access::Read);
                graphics::setBuffer(1, mesh.importedLineBuffer, graphics::Access::Read);
                graphics::setVertexCount(4); graphics::setInstanceCount(item.lineIndexCount / 2);
                setMarkerRenderState(renderState(item.lineDepthTest ? WOBY_GPU_STATE_DEPTH_TEST_LEQUAL
                    : WOBY_GPU_STATE_DEPTH_TEST_ALWAYS, item.lineDepthTest, item.color, WOBY_GPU_STATE_PT_TRISTRIP), markerIds);
                graphics::submit(viewId, colorProgram);
            }
            points(item, mesh, range);
        });
        return;
    }
    const auto triangleEdges = [&](const SceneDrawItem& item, const GpuMesh& mesh,
        const GpuNodeRange& range, bool hardwareLines) {
        if (!range.triangleIndexCount || !graphics::isValid(mesh.triangleIndexBuffer)) { return; }
        const std::array<float, 4> params{.5f, item.solid ? 1.0f : 0.0f,
            float(range.triangleIndexOffset & 65535u), float(range.triangleIndexOffset >> 16u)};
        const auto color = hardwareLines ? scaledRgbColor(item.color, 1.25f) : item.color;
        graphics::setTransform(item.model.data());
        graphics::setUniform(colorUniform, color.data());
        graphics::setUniform(uvGridUniform, item.uvGrid.data());
        graphics::setUniform(edges.parameters, params.data());
        graphics::setVertexBuffer(0, mesh.vertexBuffer);
        if (!hardwareLines && edges.nativeBarycentrics) {
            graphics::setIndexBuffer(mesh.triangleIndexBuffer, range.triangleIndexOffset, range.triangleIndexCount);
        } else {
            graphics::setBuffer(1, mesh.triangleIndexBuffer, graphics::Access::Read);
            graphics::setVertexCount(range.triangleIndexCount * (hardwareLines ? 2u : 1u));
        }
        auto flags = renderState(hardwareLines && plan.triangleEdgeXray ? WOBY_GPU_STATE_DEPTH_TEST_ALWAYS
            : item.solid && !hardwareLines ? WOBY_GPU_STATE_DEPTH_TEST_LESS : WOBY_GPU_STATE_DEPTH_TEST_LEQUAL,
            !hardwareLines && item.solid, color, hardwareLines ? WOBY_GPU_STATE_PT_LINES : 0);
        if (!hardwareLines && !item.solid) { flags |= WOBY_GPU_STATE_BLEND_ALPHA; }
        if (!hardwareLines && item.solid && transparency && color[3] < .999f) {
            graphics::setState(flags & ~WOBY_GPU_STATE_BLEND_ALPHA);
            graphics::submitTransparent(viewId, edges.transparentSurface, transparency->resolve);
        } else {
            setMarkerRenderState(flags, markerIds);
            graphics::submit(viewId, hardwareLines ? (markerIds ? edges.markerLines : edges.lines)
                : (markerIds ? edges.markerSurface : edges.surface));
        }
    };
    // Hidden-line occluders precede every color draw, including other files.
    each([&](const auto& item, const auto& mesh, const auto& range) {
        if (!item.solid && item.edges && !plan.triangleEdgeXray && item.color[3] >= .999f
            && range.triangleIndexCount && graphics::isValid(mesh.triangleIndexBuffer)) {
            graphics::setTransform(item.model.data());
            // The picking variant discards zero-alpha fragments even when color
            // writes are disabled; supply the opaque occluder's alpha explicitly.
            graphics::setUniform(colorUniform, item.color.data());
            graphics::setVertexBuffer(0, mesh.vertexBuffer);
            graphics::setIndexBuffer(mesh.triangleIndexBuffer, range.triangleIndexOffset, range.triangleIndexCount);
            setMarkerRenderState(WOBY_GPU_STATE_WRITE_Z | WOBY_GPU_STATE_DEPTH_TEST_LESS | WOBY_GPU_STATE_MSAA, markerIds);
            graphics::submit(viewId, colorProgram);
        }
    });
    // Opaque surfaces establish visibility before transparent content or markers.
    // Transparent surfaces and their edges share opacity and accumulation;
    // resolve before standalone/X-ray edges and points.
    for (const bool opaque : {true, false}) {
        each([&](const auto& item, const auto& mesh, const auto& range) {
            if (!item.solid || item.color[3] <= 0 || (item.color[3] >= .999f) != opaque) { return; }
            if (item.edges && !plan.triangleEdgeXray) { triangleEdges(item, mesh, range, false); }
            else { submitTriangleRange(viewId, mesh, meshProgram, uvGridUniform, colorUniform,
                item.model.data(), item.color, range.triangleIndexOffset, range.triangleIndexCount, markerIds, item.uvGrid, transparency); }
        });
    }
    each([&](const auto& item, const auto& mesh, const auto& range) {
        if (!item.edges || item.color[3] <= 0) { return; }
        const bool hardwareLines = plan.triangleEdgeXray || item.color[3] < .999f;
        if (plan.triangleEdgeXray || !item.solid) { triangleEdges(item, mesh, range, hardwareLines); }
    });
    each(points);
    if (adaptivePoints) {
        try { submitAdaptivePoints(*adaptivePoints,viewId,markerIds); }
        catch (const std::exception& error) {
            adaptivePoints->error=error.what(); adaptivePoints->unavailable=true;
            adaptivePoints->enabled=adaptivePoints->active=false;
            adaptivePoints->keys.clear(); adaptivePoints->draws.clear();
            each([&](const auto& item,const auto& mesh,const auto& range) {
                if (mesh.pointCloud) points(item,mesh,range);
            });
        }
    }
}

void submitSceneSelection(woby::graphics::ViewId viewId, std::span<const ScenePickPart> parts, const UiState& state,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform,
    SceneRenderScratch& scratch)
{
    sceneSelectionLines(parts, scratch.positions);
    submitHelperLines(viewId, scratch.positions, layout, program, colorUniform, {1.0f, .78f, .15f, 1.0f});
    uvProbeLines(parts,state,scratch.positions, &scratch.queries);
    submitHelperLines(viewId, scratch.positions, layout, program, colorUniform, {.1f,1,.8f,1});
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
