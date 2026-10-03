#include "gpu_runtime.h"
#include "graphics_helpers.h"
#include <bx/math.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mesh_lab {
namespace g = woby::graphics;
void upload(GpuCapture& current, const Trace& trace)
{
    if (g::isValid(current.vertices) && !current.complete) {
        throw std::runtime_error("Cannot replace a capture while its readback is in flight.");
    }
    GpuCapture capture;
    try {
        capture.vertexUpload = vertexBytes(trace);
        capture.indexUpload = indexBytes(trace);
        capture.vertexReadback.resize(capture.vertexUpload.size());
        capture.indexReadback.resize(capture.indexUpload.size());
        capture.vertices = g::createVertexBuffer(g::copy(capture.vertexUpload.data(),
            static_cast<uint32_t>(capture.vertexUpload.size())), {sizeof(woby::Vertex)}, WOBY_GPU_BUFFER_COMPUTE_READ);
        capture.indices = g::createIndexBuffer(g::copy(capture.indexUpload.data(),
            static_cast<uint32_t>(capture.indexUpload.size())), WOBY_GPU_BUFFER_INDEX32);
        if (!g::isValid(capture.vertices) || !g::isValid(capture.indices)) {
            throw std::runtime_error("Failed to allocate mesh GPU buffers.");
        }
        const auto vertexReady = g::readBuffer(capture.vertices, capture.vertexReadback.data());
        const auto indexReady = g::readBuffer(capture.indices, capture.indexReadback.data());
        capture.readyFrame = std::max(vertexReady, indexReady);
    } catch (...) {
        // A queued read may already refer to these owned destinations.
        for (int i = 0; i < 4; ++i) { g::frame(); }
        destroy(capture);
        throw;
    }
    destroy(current);
    current = std::move(capture);
}
void pollReadback(GpuCapture& capture, uint32_t frame)
{
    if (!capture.complete && frame >= capture.readyFrame) {
        capture.complete = true;
        capture.matches = capture.vertexUpload == capture.vertexReadback && capture.indexUpload == capture.indexReadback;
    }
}
void destroy(GpuCapture& capture)
{
    if (g::isValid(capture.indices)) { g::destroy(capture.indices); }
    if (g::isValid(capture.vertices)) { g::destroy(capture.vertices); }
    capture = {};
}
void initViewport(Viewport& view, const std::filesystem::path& assets)
{
    view.program = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
    view.presentation = woby::loadProgram(assets, "vs_marker_screen.bin", "fs_marker_composite.bin");
    view.colorUniform = g::createUniform("u_color", g::UniformType::Vec4);
}
void resizeViewport(Viewport& view, uint16_t width, uint16_t height)
{
    if (view.width == width && view.height == height) { return; }
    if (g::isValid(view.target)) { g::destroy(view.target); }
    if (g::isValid(view.color)) { g::destroy(view.color); }
    if (g::isValid(view.depth)) { g::destroy(view.depth); }
    view.width = width; view.height = height;
    view.color = g::createTexture2D(width, height, false, 1, g::TextureFormat::RGBA8, WOBY_GPU_TEXTURE_RT);
    view.depth = g::createTexture2D(width, height, false, 1, g::TextureFormat::D24S8, WOBY_GPU_TEXTURE_RT);
    const std::array attachments{view.color, view.depth};
    view.target = g::createFrameBuffer(2, attachments.data());
    if (!g::isValid(view.target)) { throw std::runtime_error("Could not allocate the mesh viewport."); }
}
ViewMatrices viewMatrices(const Trace& trace, const UiState& state, float aspect, bool homogeneousDepth)
{
    const float radius = trace.mesh.bounds.radius;
    const auto& center = trace.mesh.bounds.center;
    const float distance = radius * 3.4f / state.zoom;
    const bx::Vec3 target{center[0], center[1], center[2]};
    const bx::Vec3 eye{center[0] + std::sin(state.yaw) * std::cos(state.pitch) * distance,
        center[1] + std::sin(state.pitch) * distance,
        center[2] + std::cos(state.yaw) * std::cos(state.pitch) * distance};
    ViewMatrices result;
    bx::mtxLookAt(result.view.data(), eye, target);
    bx::mtxProj(result.projection.data(), 43.0f, aspect,
        radius * 0.01f, radius * 20.0f, homogeneousDepth);
    return result;
}
std::array<float, 2> projectVertex(const ViewMatrices& matrices, const woby::Vertex& vertex)
{
    const bx::Vec3 p{vertex.position[0], vertex.position[1], vertex.position[2]};
    const auto camera = bx::mul(p, matrices.view.data());
    const auto projected = bx::mulH(camera, matrices.projection.data());
    return {(projected.x + 1) * 0.5f, (1 - projected.y) * 0.5f};
}
void renderViewport(const Viewport& view, const GpuCapture& gpu, const Trace& trace, const UiState& state, g::ViewId viewId)
{
    if (!g::isValid(view.target)) { return; }
    const auto matrices = viewMatrices(trace, state, static_cast<float>(view.width) / static_cast<float>(view.height), g::getCaps()->homogeneousDepth);
    float model[16];
    bx::mtxIdentity(model);
    g::setViewFrameBuffer(viewId, view.target);
    g::setViewRect(viewId, 0, 0, view.width, view.height);
    g::setViewClear(viewId, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 0x111920ff);
    g::setViewTransform(viewId, matrices.view.data(), matrices.projection.data());
    g::setViewMode(viewId, g::ViewMode::Sequential);
    g::touch(viewId);
    const std::array<float, 4> base{0.33f, 0.47f, 0.57f, 1.0f}, selected{0.27f, 0.9f, 0.7f, 1.0f};
    const auto submit = [&](const auto& color, uint32_t first, uint32_t count) {
        g::setTransform(model);
        g::setVertexBuffer(0, gpu.vertices);
        g::setIndexBuffer(gpu.indices, first, count);
        g::setUniform(view.colorUniform, color.data());
        g::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_WRITE_Z | WOBY_GPU_STATE_DEPTH_TEST_LEQUAL);
        g::submit(viewId, view.program);
    };
    submit(base, 0, static_cast<uint32_t>(trace.mesh.indices.size()));
    submit(selected, static_cast<uint32_t>(state.triangle * 3), 3);
}
void destroy(Viewport& view)
{
    if (g::isValid(view.target)) { g::destroy(view.target); }
    if (g::isValid(view.color)) { g::destroy(view.color); }
    if (g::isValid(view.depth)) { g::destroy(view.depth); }
    if (g::isValid(view.program)) { g::destroy(view.program); }
    if (g::isValid(view.presentation)) { g::destroy(view.presentation); }
    if (g::isValid(view.colorUniform)) { g::destroy(view.colorUniform); }
    view = {};
}
} // namespace mesh_lab
