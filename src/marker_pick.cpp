#include "marker_pick.h"
#include "bgfx_helpers.h"

#include <algorithm>
#include <stdexcept>

namespace woby {
namespace {

// Numeric ordering is deliberate: bgfx's blit ordering must follow the lookup.
constexpr bgfx::ViewId sceneView = 1, lookupView = 2, compositeView = 3,
    highlightView = 4, readbackView = 6;
constexpr uint64_t samplerFlags = BGFX_SAMPLER_POINT | BGFX_SAMPLER_UVW_CLAMP;

template <typename Handle>
void release(Handle& handle)
{
    if (bgfx::isValid(handle)) { bgfx::destroy(handle); }
    handle = BGFX_INVALID_HANDLE;
}

void destroyTargets(GpuMarkerPicker& picker)
{
    release(picker.framebuffer);
    release(picker.color);
    release(picker.ids);
    release(picker.depth);
    picker.width = picker.height = 0;
}

void initialize(GpuMarkerPicker& picker, const std::filesystem::path& assets)
{
    picker.point = loadProgram(assets, "vs_marker_point.bin", "fs_marker_point.bin");
    picker.mesh = loadProgram(assets, "vs_mesh.bin", "fs_marker_mesh.bin");
    picker.line = loadProgram(assets, "vs_color.bin", "fs_marker_line.bin");
    picker.comparison = loadProgram(assets, "vs_comparison.bin", "fs_marker_comparison.bin");
    picker.composite = loadProgram(assets, "vs_marker_screen.bin", "fs_marker_composite.bin");
    const auto root = assets / "shaders" / rendererShaderFolder(bgfx::getRendererType());
    for (size_t i = 0; i < 2; ++i) {
        const std::string suffix = i == 0 ? "single" : "msaa";
        picker.lookup[i] = bgfx::createProgram(loadShader(root / ("cs_marker_lookup_" + suffix + ".bin")), true);
        picker.highlight[i] = loadProgram(assets, "vs_marker_highlight.bin", ("fs_marker_highlight_" + suffix + ".bin").c_str());
    }
    picker.context.baseUniform = bgfx::createUniform("u_markerBase", bgfx::UniformType::Vec4);
    picker.queryUniform = bgfx::createUniform("u_markerQuery", bgfx::UniformType::Vec4);
    picker.optionsUniform = bgfx::createUniform("u_markerOptions", bgfx::UniformType::Vec4);
    picker.colorSampler = bgfx::createUniform("s_markerColor", bgfx::UniformType::Sampler);
    picker.idSampler = bgfx::createUniform("s_markerIds", bgfx::UniformType::Sampler);
    picker.resultSampler = bgfx::createUniform("s_markerResult", bgfx::UniformType::Sampler);
    picker.result = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
        BGFX_TEXTURE_COMPUTE_WRITE | samplerFlags);
    for (auto& request : picker.requests) {
        request.staging = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA32F,
            BGFX_TEXTURE_READ_BACK | BGFX_TEXTURE_BLIT_DST);
        if (!bgfx::isValid(request.staging)) { throw std::runtime_error("Marker readback allocation failed."); }
    }
    for (auto handle : {picker.point, picker.mesh, picker.line, picker.comparison, picker.composite,
             picker.lookup[0], picker.lookup[1], picker.highlight[0], picker.highlight[1]}) {
        if (!bgfx::isValid(handle)) { throw std::runtime_error("Marker shader allocation failed."); }
    }
    if (!bgfx::isValid(picker.result)) { throw std::runtime_error("Marker result allocation failed."); }
    picker.initialized = true;
}

void createTargets(GpuMarkerPicker& picker, uint16_t width, uint16_t height, int samples)
{
    destroyTargets(picker);
    const uint64_t msaa = samples == 4 ? BGFX_TEXTURE_RT_MSAA_X4 : BGFX_TEXTURE_RT;
    const uint64_t idFlags = msaa | samplerFlags | (samples == 4 ? BGFX_TEXTURE_MSAA_SAMPLE : 0u);
    if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::RGBA8, idFlags)) {
        throw std::runtime_error("Multisample marker IDs are unsupported.");
    }
    picker.color = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::BGRA8, msaa | samplerFlags);
    picker.ids = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8, idFlags);
    picker.depth = bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::D24S8, msaa | BGFX_TEXTURE_RT_WRITE_ONLY);
    if (!bgfx::isValid(picker.color) || !bgfx::isValid(picker.ids) || !bgfx::isValid(picker.depth)) {
        throw std::runtime_error("Marker target allocation failed.");
    }
    const std::array<bgfx::TextureHandle, 3> attachments = {picker.color, picker.ids, picker.depth};
    picker.framebuffer = bgfx::createFrameBuffer(static_cast<uint8_t>(attachments.size()), attachments.data(), false);
    if (!bgfx::isValid(picker.framebuffer)) { throw std::runtime_error("Marker framebuffer allocation failed."); }
    picker.width = width; picker.height = height; picker.samples = samples;
    bgfx::setPaletteColor(14, 0x20242affu);
    bgfx::setPaletteColor(15, 0x00000000u);
}

} // namespace

bool supportsGpuMarkerPicking(const bgfx::Caps& caps)
{
    const uint64_t required = BGFX_CAPS_COMPUTE | BGFX_CAPS_TEXTURE_READ_BACK
        | BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_BLEND_INDEPENDENT;
    // Bottom-left backends retain the existing picker until their orientation
    // and multisample path have been validated.
    return (caps.supported & required) == required && caps.limits.maxFBAttachments >= 2
        && !caps.originBottomLeft && caps.rendererType != bgfx::RendererType::Noop;
}

void setMarkerRenderState(uint64_t state, bool markerIds)
{
    if (markerIds) {
        bgfx::setState(state | BGFX_STATE_BLEND_INDEPENDENT,
            BGFX_STATE_BLEND_FUNC_RT_1(BGFX_STATE_BLEND_ONE, BGFX_STATE_BLEND_ZERO));
    } else { bgfx::setState(state); }
}

bool beginGpuMarkerPicking(GpuMarkerPicker& picker, const std::filesystem::path& assets,
    const UiState& state, const SceneViewport& viewport, MousePosition mouse, bool enabled, int samples)
{
    const bool resized = picker.width != viewport.width || picker.height != viewport.height || picker.samples != samples;
    const bool changed = picker.sceneGeneration != state.sceneGeneration || picker.sceneRevision != state.sceneEditRevision || resized;
    if (changed || !enabled || !picker.active) {
        ++picker.epoch;
        picker.coordinates.reset();
    }
    picker.sceneGeneration = state.sceneGeneration; picker.sceneRevision = state.sceneEditRevision;
    picker.active = false;
    picker.context.list.draws.clear(); picker.context.list.nextId = 1; picker.context.list.largestPoint = 0;
    bgfx::setViewFrameBuffer(sceneView, BGFX_INVALID_HANDLE);
    bgfx::setViewClear(sceneView, BGFX_CLEAR_NONE);
    if (!enabled || picker.unavailable || viewport.width == 0 || viewport.height == 0) { return false; }
    if (!supportsGpuMarkerPicking(*bgfx::getCaps())) { picker.unavailable = true; return false; }
    if (!picker.initialized) { initialize(picker, assets); }
    if (resized || !bgfx::isValid(picker.framebuffer)) {
        createTargets(picker, static_cast<uint16_t>(viewport.width), static_cast<uint16_t>(viewport.height), samples);
    }
    picker.active = true;
    picker.query = {mouse.x, mouse.y, static_cast<float>(picker.width), static_cast<float>(picker.height)};
    bgfx::setViewFrameBuffer(sceneView, picker.framebuffer);
    bgfx::setViewRect(sceneView, 0, 0, picker.width, picker.height);
    bgfx::setViewClear(sceneView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 1.0f, 0, 14, 15);
    bgfx::setViewName(lookupView, "Marker lookup");
    bgfx::setViewName(compositeView, "Marker composite");
    bgfx::setViewName(highlightView, "Marker highlight");
    bgfx::setViewName(readbackView, "Marker readback");
    return true;
}

void submitGpuMarkerPicking(GpuMarkerPicker& picker, const SceneViewport& viewport)
{
    if (!picker.active) { return; }
    picker.options = {static_cast<float>(picker.samples), 0, picker.context.list.largestPoint + 5.0f, 0};
    const size_t variant = picker.samples == 4 ? 1u : 0u;
    bgfx::setUniform(picker.queryUniform, picker.query.data());
    bgfx::setUniform(picker.optionsUniform, picker.options.data());
    bgfx::setTexture(0, picker.idSampler, picker.ids);
    bgfx::setImage(1, picker.result, 0, bgfx::Access::Write, bgfx::TextureFormat::RGBA32F);
    bgfx::dispatch(lookupView, picker.lookup[variant], 1);
    for (auto view : {compositeView, highlightView}) {
        bgfx::setViewFrameBuffer(view, BGFX_INVALID_HANDLE);
        bgfx::setViewRect(view, static_cast<uint16_t>(viewport.x), static_cast<uint16_t>(viewport.y), picker.width, picker.height);
        bgfx::setViewTransform(view, nullptr, nullptr);
    }
    bgfx::setTexture(0, picker.colorSampler, picker.color);
    bgfx::setVertexCount(4);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP);
    bgfx::submit(compositeView, picker.composite);
    bgfx::setUniform(picker.queryUniform, picker.query.data());
    bgfx::setUniform(picker.optionsUniform, picker.options.data());
    bgfx::setTexture(0, picker.idSampler, picker.ids);
    bgfx::setTexture(1, picker.resultSampler, picker.result);
    bgfx::setVertexCount(4);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_TRISTRIP | BGFX_STATE_BLEND_ALPHA);
    bgfx::submit(highlightView, picker.highlight[variant]);

    const auto slot = std::find_if(picker.requests.begin(), picker.requests.end(), [](const auto& request) { return !request.pending; });
    if (slot == picker.requests.end()) { return; } // Highlight never waits for a readback slot.
    slot->pending = true; slot->requested = false;
    slot->issuedFrame = picker.frame; slot->epoch = picker.epoch; slot->sequence = ++picker.sequence;
    slot->draws = picker.context.list.draws;
    bgfx::blit(readbackView, slot->staging, 0, 0, picker.result);
}

std::optional<HoveredVertex> resolveMarkerCoordinates(uint32_t id, std::span<const MarkerDraw> draws,
    const UiState& state, const std::vector<LoadedModelRuntime>& runtimes)
{
    const auto* draw = findMarkerDraw(draws, id);
    if (!draw || draw->fileIndex >= state.files.size() || draw->fileIndex >= runtimes.size()) { return {}; }
    const auto& file = state.files[draw->fileIndex];
    if (file.objectId != draw->fileId) { return {}; }
    const auto& points = runtimes[draw->fileIndex].gpuMesh.pointVertexIndices;
    const uint64_t rank = uint64_t{draw->pointOffset} + (id - draw->firstId);
    if (rank >= points.size() || points[static_cast<size_t>(rank)] >= file.mesh.vertices.size()) { return {}; }
    const auto& local = file.mesh.vertices[points[static_cast<size_t>(rank)]].position;
    return HoveredVertex{local, transformMarkerPosition(draw->model, local), 0, 0};
}

void pollGpuMarkerPicking(GpuMarkerPicker& picker, uint32_t frame, const UiState& state,
    const std::vector<LoadedModelRuntime>& runtimes)
{
    picker.frame = frame;
    for (auto& request : picker.requests) {
        if (!request.pending) { continue; }
        if (!request.requested) {
            if (markerFrameReached(frame, request.issuedFrame + 3)) {
                request.readyFrame = bgfx::readTexture(request.staging, request.pixel.data());
                request.requested = true;
            }
            continue;
        }
        if (!markerFrameReached(frame, request.readyFrame)) { continue; }
        request.pending = false;
        if (picker.active && state.sceneGeneration == picker.sceneGeneration && state.sceneEditRevision == picker.sceneRevision
            && acceptMarkerCompletion(request.epoch, picker.epoch, request.sequence, picker.latestSequence)) {
            picker.latestSequence = request.sequence;
            const auto id = decodeMarkerPixel(request.pixel);
            picker.coordinates = id ? resolveMarkerCoordinates(*id, request.draws, state, runtimes) : std::nullopt;
        }
        request.draws.clear();
    }
}

void destroyGpuMarkerPicker(GpuMarkerPicker& picker)
{
    // readTexture writes to caller-owned memory on the render thread.
    for (const auto& request : picker.requests) {
        if (request.pending && request.requested) {
            while (!markerFrameReached(picker.frame, request.readyFrame)) { picker.frame = bgfx::frame(); }
        }
    }
    bgfx::setViewFrameBuffer(sceneView, BGFX_INVALID_HANDLE);
    bgfx::setViewClear(sceneView, BGFX_CLEAR_NONE);
    destroyTargets(picker);
    release(picker.point); release(picker.mesh); release(picker.line); release(picker.comparison); release(picker.composite);
    for (auto& handle : picker.lookup) { release(handle); }
    for (auto& handle : picker.highlight) { release(handle); }
    release(picker.context.baseUniform); release(picker.queryUniform); release(picker.optionsUniform);
    release(picker.colorSampler); release(picker.idSampler); release(picker.resultSampler); release(picker.result);
    for (auto& request : picker.requests) { release(request.staging); request.pending = false; }
    picker.initialized = picker.active = false;
    picker.coordinates.reset();
}

} // namespace woby
