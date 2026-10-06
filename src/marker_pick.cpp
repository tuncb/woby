#include "marker_pick.h"
#include "graphics_helpers.h"
#include "hash_utils.h"

#include <algorithm>
#include <stdexcept>

namespace woby {
namespace {

// Numeric ordering is deliberate: graphics's blit ordering must follow the lookup.
constexpr woby::graphics::ViewId sceneView = 1, lookupView = 2, compositeView = 3,
    highlightView = 4, readbackView = 6;
constexpr uint64_t samplerFlags = WOBY_GPU_SAMPLER_POINT | WOBY_GPU_SAMPLER_UVW_CLAMP;

template <typename Handle>
void release(Handle& handle)
{
    if (woby::graphics::isValid(handle)) { woby::graphics::destroy(handle); }
    handle = WOBY_GPU_INVALID_HANDLE;
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
    picker.lineSprite = loadProgram(assets, "vs_line_sprite.bin", "fs_marker_line.bin");
    picker.comparison = loadProgram(assets, "vs_comparison.bin", "fs_marker_comparison.bin");
    picker.composite = loadProgram(assets, "vs_marker_screen.bin", "fs_marker_composite.bin");
    const auto root = assets / "shaders" / rendererShaderFolder(woby::graphics::getRendererType());
    for (size_t i = 0; i < 2; ++i) {
        const std::string suffix = i == 0 ? "single" : "msaa";
        picker.lookup[i] = woby::graphics::createProgram(loadShader(root / ("cs_marker_lookup_" + suffix + ".bin")), true);
        picker.highlight[i] = loadProgram(assets, "vs_marker_highlight.bin", ("fs_marker_highlight_" + suffix + ".bin").c_str());
    }
    picker.context.baseUniform = woby::graphics::createUniform("u_markerBase", woby::graphics::UniformType::Vec4);
    picker.queryUniform = woby::graphics::createUniform("u_markerQuery", woby::graphics::UniformType::Vec4);
    picker.optionsUniform = woby::graphics::createUniform("u_markerOptions", woby::graphics::UniformType::Vec4);
    picker.colorSampler = woby::graphics::createUniform("s_markerColor", woby::graphics::UniformType::Sampler);
    picker.idSampler = woby::graphics::createUniform("s_markerIds", woby::graphics::UniformType::Sampler);
    picker.resultSampler = woby::graphics::createUniform("s_markerResult", woby::graphics::UniformType::Sampler);
    picker.result = woby::graphics::createTexture2D(1, 1, false, 1, woby::graphics::TextureFormat::RGBA32F,
        WOBY_GPU_TEXTURE_COMPUTE_WRITE | samplerFlags);
    for (auto& request : picker.requests) {
        request.staging = woby::graphics::createTexture2D(1, 1, false, 1, woby::graphics::TextureFormat::RGBA32F,
            WOBY_GPU_TEXTURE_READ_BACK | WOBY_GPU_TEXTURE_BLIT_DST);
        if (!woby::graphics::isValid(request.staging)) { throw std::runtime_error("Marker readback allocation failed."); }
    }
    for (auto handle : {picker.point, picker.mesh, picker.line, picker.lineSprite, picker.comparison, picker.composite,
             picker.lookup[0], picker.lookup[1], picker.highlight[0], picker.highlight[1]}) {
        if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Marker shader allocation failed."); }
    }
    if (!woby::graphics::isValid(picker.result)) { throw std::runtime_error("Marker result allocation failed."); }
    picker.initialized = true;
}

void createTargets(GpuMarkerPicker& picker, uint16_t width, uint16_t height, int samples)
{
    destroyTargets(picker);
    const uint64_t msaa = samples == 4 ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
    const uint64_t idFlags = msaa | samplerFlags | (samples == 4 ? WOBY_GPU_TEXTURE_MSAA_SAMPLE : 0u);
    if (!woby::graphics::isTextureValid(0, false, 1, woby::graphics::TextureFormat::RGBA8, idFlags)) {
        throw std::runtime_error("Multisample marker IDs are unsupported.");
    }
    picker.color = woby::graphics::createTexture2D(width, height, false, 1, woby::graphics::TextureFormat::BGRA8, msaa | samplerFlags);
    picker.ids = woby::graphics::createTexture2D(width, height, false, 1, woby::graphics::TextureFormat::RGBA8, idFlags);
    picker.depth = woby::graphics::createTexture2D(width, height, false, 1, woby::graphics::TextureFormat::D24S8, msaa | WOBY_GPU_TEXTURE_RT_WRITE_ONLY);
    if (!woby::graphics::isValid(picker.color) || !woby::graphics::isValid(picker.ids) || !woby::graphics::isValid(picker.depth)) {
        throw std::runtime_error("Marker target allocation failed.");
    }
    const std::array<woby::graphics::TextureHandle, 3> attachments = {picker.color, picker.ids, picker.depth};
    picker.framebuffer = woby::graphics::createFrameBuffer(static_cast<uint8_t>(attachments.size()), attachments.data(), false);
    if (!woby::graphics::isValid(picker.framebuffer)) { throw std::runtime_error("Marker framebuffer allocation failed."); }
    picker.width = width; picker.height = height; picker.samples = samples;
    woby::graphics::setPaletteColor(14, 0x20242affu);
    woby::graphics::setPaletteColor(15, 0x00000000u);
}

} // namespace

bool supportsGpuMarkerPicking(const woby::graphics::Caps& caps)
{
    const uint64_t required = WOBY_GPU_CAPS_COMPUTE | WOBY_GPU_CAPS_TEXTURE_READ_BACK
        | WOBY_GPU_CAPS_TEXTURE_BLIT | WOBY_GPU_CAPS_BLEND_INDEPENDENT;
    // Bottom-left backends retain the existing picker until their orientation
    // and multisample path have been validated.
    return (caps.supported & required) == required && caps.limits.maxFBAttachments >= 2
        && !caps.originBottomLeft && caps.rendererType != woby::graphics::RendererType::Noop;
}

void setMarkerRenderState(uint64_t state, bool markerIds)
{
    if (markerIds) {
        woby::graphics::setState(state | WOBY_GPU_STATE_BLEND_INDEPENDENT,
            WOBY_GPU_STATE_BLEND_FUNC_RT_1(WOBY_GPU_STATE_BLEND_ONE, WOBY_GPU_STATE_BLEND_ZERO));
    } else { woby::graphics::setState(state); }
}

static uint64_t markerSceneRevision(const UiState& state)
{
    uint64_t signature = 17;
    hashCombine(signature, state.revisions.geometry); hashCombine(signature, state.revisions.visibility);
    hashCombine(signature, state.revisions.picking); hashCombine(signature, state.revisions.analysis);
    hashCombine(signature, state.revisions.presentation);
    hashBounds(signature, state.sceneBounds);
    return signature;
}

bool beginGpuMarkerPicking(GpuMarkerPicker& picker, const std::filesystem::path& assets,
    const UiState& state, const SceneViewport& viewport, MousePosition mouse, bool enabled, int samples, uint64_t resourceSignature)
{
    const bool resized = picker.width != viewport.width || picker.height != viewport.height || picker.samples != samples;
    const auto signature = markerSceneRevision(state);
    const bool changed = picker.owner != &state || picker.sceneGeneration != state.sceneGeneration
        || picker.sceneRevision != signature || picker.resourceSignature != resourceSignature || resized
        || picker.camera != state.camera || picker.upAxis != state.upAxis
        || picker.query[0] != mouse.x || picker.query[1] != mouse.y;
    if (changed || !enabled || !picker.active) {
        ++picker.epoch;
        picker.coordinates.reset();
    }
    picker.owner = &state; picker.sceneGeneration = state.sceneGeneration; picker.sceneRevision = signature;
    picker.camera = state.camera; picker.upAxis = state.upAxis; picker.resourceSignature = resourceSignature;
    picker.active = false;
    picker.context.list.draws.clear(); picker.context.list.nextId = 1; picker.context.list.largestPoint = 0;
    woby::graphics::setViewFrameBuffer(sceneView, WOBY_GPU_INVALID_HANDLE);
    woby::graphics::setViewClear(sceneView, WOBY_GPU_CLEAR_NONE);
    if (!enabled || picker.unavailable || viewport.width == 0 || viewport.height == 0) { return false; }
    if (!supportsGpuMarkerPicking(*woby::graphics::getCaps())) { picker.unavailable = true; return false; }
    if (!picker.initialized) { initialize(picker, assets); }
    if (resized || !woby::graphics::isValid(picker.framebuffer)) {
        createTargets(picker, static_cast<uint16_t>(viewport.width), static_cast<uint16_t>(viewport.height), samples);
    }
    picker.active = true;
    picker.query = {mouse.x, mouse.y, static_cast<float>(picker.width), static_cast<float>(picker.height)};
    woby::graphics::setViewFrameBuffer(sceneView, picker.framebuffer);
    woby::graphics::setViewRect(sceneView, 0, 0, picker.width, picker.height);
    woby::graphics::setViewClear(sceneView, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 1.0f, 0, 14, 15);
    woby::graphics::setViewName(lookupView, "Marker lookup");
    woby::graphics::setViewName(compositeView, "Marker composite");
    woby::graphics::setViewName(highlightView, "Marker highlight");
    woby::graphics::setViewName(readbackView, "Marker readback");
    return true;
}

void submitGpuMarkerPicking(GpuMarkerPicker& picker, const SceneViewport& viewport)
{
    if (!picker.active) { return; }
    picker.options = {static_cast<float>(picker.samples), 0, picker.context.list.largestPoint + 5.0f, 0};
    const size_t variant = picker.samples == 4 ? 1u : 0u;
    woby::graphics::setUniform(picker.queryUniform, picker.query.data());
    woby::graphics::setUniform(picker.optionsUniform, picker.options.data());
    woby::graphics::setTexture(0, picker.idSampler, picker.ids);
    woby::graphics::setImage(1, picker.result, 0, woby::graphics::Access::Write, woby::graphics::TextureFormat::RGBA32F);
    woby::graphics::dispatch(lookupView, picker.lookup[variant], 1);
    for (auto view : {compositeView, highlightView}) {
        woby::graphics::setViewFrameBuffer(view, WOBY_GPU_INVALID_HANDLE);
        woby::graphics::setViewRect(view, static_cast<uint16_t>(viewport.x), static_cast<uint16_t>(viewport.y), picker.width, picker.height);
        woby::graphics::setViewTransform(view, nullptr, nullptr);
    }
    woby::graphics::setTexture(0, picker.colorSampler, picker.color);
    woby::graphics::setVertexCount(4);
    woby::graphics::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_TRISTRIP);
    woby::graphics::submit(compositeView, picker.composite);
    woby::graphics::setUniform(picker.queryUniform, picker.query.data());
    woby::graphics::setUniform(picker.optionsUniform, picker.options.data());
    woby::graphics::setTexture(0, picker.idSampler, picker.ids);
    woby::graphics::setTexture(1, picker.resultSampler, picker.result);
    woby::graphics::setVertexCount(4);
    woby::graphics::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_TRISTRIP | WOBY_GPU_STATE_BLEND_ALPHA);
    woby::graphics::submit(highlightView, picker.highlight[variant]);

    const auto slot = std::find_if(picker.requests.begin(), picker.requests.end(), [](const auto& request) { return !request.pending; });
    if (slot == picker.requests.end()) { return; } // Highlight never waits for a readback slot.
    slot->pending = true; slot->requested = false;
    slot->issuedFrame = picker.frame; slot->epoch = picker.epoch; slot->sequence = ++picker.sequence;
    slot->draws = picker.context.list.draws;
    woby::graphics::blit(readbackView, slot->staging, 0, 0, picker.result);
}

std::optional<HoveredVertex> resolveMarkerCoordinates(uint32_t id, std::span<const MarkerDraw> draws,
    const UiState& state, const std::vector<LoadedModelRuntime>& runtimes)
{
    const auto* draw = findMarkerDraw(draws, id);
    if (!draw || draw->fileIndex >= state.files.size() || draw->fileIndex >= runtimes.size()) { return {}; }
    const auto& file = state.files[draw->fileIndex];
    if (file.objectId != draw->fileId) { return {}; }
    const auto points = meshPointVertexIndices(runtimes[draw->fileIndex].gpuMesh);
    const uint64_t rank = uint64_t{draw->pointOffset} + (id - draw->firstId);
    if (rank >= points.size() || points[static_cast<size_t>(rank)] >= file.mesh.vertices.size()) { return {}; }
    const auto& local = file.mesh.vertices[points[static_cast<size_t>(rank)]].position;
    return HoveredVertex{local, transformMarkerPosition(draw->model, local), 0, 0};
}

void pollGpuMarkerPicking(GpuMarkerPicker& picker, uint32_t frame, const UiState& state,
    const std::vector<LoadedModelRuntime>& runtimes, uint64_t resourceSignature)
{
    picker.frame = frame;
    for (auto& request : picker.requests) {
        if (!request.pending) { continue; }
        if (!request.requested) {
            if (markerFrameReached(frame, request.issuedFrame + 3)) {
                request.readyFrame = woby::graphics::readTexture(request.staging, request.pixel.data());
                request.requested = true;
            }
            continue;
        }
        if (!markerFrameReached(frame, request.readyFrame)) { continue; }
        request.pending = false;
        if (picker.active && picker.owner == &state && state.sceneGeneration == picker.sceneGeneration
            && picker.camera == state.camera && picker.upAxis == state.upAxis
            && picker.sceneRevision == markerSceneRevision(state) && picker.resourceSignature == resourceSignature
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
            while (!markerFrameReached(picker.frame, request.readyFrame)) { picker.frame = woby::graphics::frame(); }
        }
    }
    woby::graphics::setViewFrameBuffer(sceneView, WOBY_GPU_INVALID_HANDLE);
    woby::graphics::setViewClear(sceneView, WOBY_GPU_CLEAR_NONE);
    destroyTargets(picker);
    release(picker.point); release(picker.mesh); release(picker.line); release(picker.lineSprite); release(picker.comparison); release(picker.composite);
    for (auto& handle : picker.lookup) { release(handle); }
    for (auto& handle : picker.highlight) { release(handle); }
    release(picker.context.baseUniform); release(picker.queryUniform); release(picker.optionsUniform);
    release(picker.colorSampler); release(picker.idSampler); release(picker.resultSampler); release(picker.result);
    for (auto& request : picker.requests) { release(request.staging); request.pending = false; }
    picker.initialized = picker.active = false;
    picker.coordinates.reset();
}

} // namespace woby
