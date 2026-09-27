#include "bgfx_helpers.h"
#include "obj_mesh.h"
#include "scene_renderer.h"
#include "utf8_path.h"

#include <bx/math.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace vertex_probe { uint16_t meshReadFlags = 0; }
#include "production.inc"

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}

struct Options {
    std::filesystem::path model, captures;
    bool shader = false, flat = false, quick = false;
    double seconds = 2.0;
    uint16_t width = 1280, height = 720;
};

struct Graphics {
    bool initialized = false;
    woby::GpuMesh mesh;
    bgfx::IndexBufferHandle pointIds = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle solid = BGFX_INVALID_HANDLE, points = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle color = BGFX_INVALID_HANDLE, params = BGFX_INVALID_HANDLE, base = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle target = BGFX_INVALID_HANDLE, depth = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle readback = BGFX_INVALID_HANDLE, fence = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
    uint32_t frame = 0;
    ~Graphics() {
        if (!initialized) { return; }
        woby::destroyGpuMesh(mesh);
        if (bgfx::isValid(pointIds)) { bgfx::destroy(pointIds); }
        if (bgfx::isValid(solid)) { bgfx::destroy(solid); }
        if (bgfx::isValid(points)) { bgfx::destroy(points); }
        for (auto handle : {color, params, base}) { if (bgfx::isValid(handle)) { bgfx::destroy(handle); } }
        if (bgfx::isValid(framebuffer)) { bgfx::destroy(framebuffer); }
        for (auto handle : {target, depth, readback, fence}) { if (bgfx::isValid(handle)) { bgfx::destroy(handle); } }
        bgfx::shutdown();
    }
};

void initialize(Graphics& g, const Options& options) {
    bgfx::Init init;
#ifdef _WIN32
    init.type = bgfx::RendererType::Direct3D11;
#else
    init.type = bgfx::RendererType::Vulkan;
#endif
    init.vendorId = 0x10de; // Select the discrete NVIDIA device on this benchmark machine.
    // Windowless device, as in the viewer's headless path; draw into our FBO.
    init.resolution.width = 0;
    init.resolution.height = 0;
    init.resolution.reset = BGFX_RESET_NONE;
    init.profile = true;
    g.initialized = bgfx::init(init);
    require(g.initialized, "bgfx initialization failed");
    bgfx::setDebug(BGFX_DEBUG_PROFILER); // Enables per-view GPU timestamps, without a stats overlay.
    const auto* caps = bgfx::getCaps();
    require((caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0, "readback unsupported");
    require((caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0, "blit unsupported");
    if (options.shader) {
        require((caps->supported & BGFX_CAPS_VERTEX_ID) != 0, "generated vertices unsupported");
        require((caps->supported & BGFX_CAPS_INSTANCING) != 0, "instancing unsupported");
        require(caps->limits.maxComputeBindings >= 2, "buffer bindings unsupported");
    }
    const auto assets = std::filesystem::path(VERTEX_PROBE_ASSETS);
    g.solid = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
    g.points = woby::loadProgram(assets, options.flat ? "vs_point_pull_flat.bin" : options.shader ? "vs_point_pull.bin" : "vs_point_sprite.bin", "fs_point_sprite.bin");
    require(bgfx::isValid(g.solid) && bgfx::isValid(g.points), "program creation failed");
    g.color = bgfx::createUniform("u_color", bgfx::UniformType::Vec4);
    g.params = bgfx::createUniform("u_pointParams", bgfx::UniformType::Vec4);
    g.base = bgfx::createUniform("u_pointBase", bgfx::UniformType::Vec4);
    g.target = bgfx::createTexture2D(options.width, options.height, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
    g.depth = bgfx::createTexture2D(options.width, options.height, false, 1, bgfx::TextureFormat::D24S8, BGFX_TEXTURE_RT);
    const uint64_t readFlags = BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK;
    g.readback = bgfx::createTexture2D(options.width, options.height, false, 1, bgfx::TextureFormat::RGBA8, readFlags);
    g.fence = bgfx::createTexture2D(1, 1, false, 1, bgfx::TextureFormat::RGBA8, readFlags);
    const bgfx::TextureHandle attachments[] = {g.target, g.depth};
    g.framebuffer = bgfx::createFrameBuffer(2, attachments, false);
    require(bgfx::isValid(g.framebuffer) && bgfx::isValid(g.readback) && bgfx::isValid(g.fence), "framebuffer creation failed");
    for (bgfx::ViewId view : {bgfx::ViewId{0}, bgfx::ViewId{1}}) {
        bgfx::setViewFrameBuffer(view, g.framebuffer);
        bgfx::setViewRect(view, 0, 0, options.width, options.height);
        bgfx::setViewMode(view, bgfx::ViewMode::Sequential);
    }
    bgfx::setViewName(0, "Solid and clear");
    bgfx::setViewName(1, "Vertex markers");
    bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x101820ff, 1.0f, 0);
}

// Readback is used outside timed steady-state drawing, to complete preceding GPU work.
void drain(Graphics& g) {
    std::array<uint8_t, 4> pixel{};
    bgfx::blit(2, g.fence, 0, 0, g.target, 0, 0, 1, 1);
    const auto ready = bgfx::readTexture(g.fence, pixel.data());
    while (g.frame < ready) { g.frame = bgfx::frame(); }
}

Json memory() {
    Json result;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        result["working_set_bytes"] = counters.WorkingSetSize;
        result["peak_working_set_bytes"] = counters.PeakWorkingSetSize;
        result["commit_bytes"] = counters.PrivateUsage;
        result["peak_commit_bytes"] = counters.PeakPagefileUsage;
    }
#endif
    const auto* stats = bgfx::getStats();
    result["bgfx_gpu_memory_used"] = stats->gpuMemoryUsed;
    result["bgfx_gpu_memory_max"] = stats->gpuMemoryMax;
    return result;
}

uint64_t hashBytes(uint64_t hash, const void* data, size_t bytes) {
    const auto* values = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { hash = (hash ^ values[i]) * 1099511628211ull; }
    return hash;
}

struct Scenario { const char* name; bool solid; float diameter, opacity; bool orthographic = false; };

void submit(Graphics& g, const Options& options, const woby::Bounds& bounds, const Scenario& scenario) {
    float model[16], view[16], projection[16];
    bx::mtxIdentity(model);
    const float scale = 1.0f / bounds.radius;
    model[0] = model[5] = model[10] = scale;
    for (size_t axis = 0; axis < 3; ++axis) { model[12 + axis] = -bounds.center[axis] * scale; }
    const bx::Vec3 eye{1.7f, 1.1f, -2.4f}, at{0, 0, 0};
    bx::mtxLookAt(view, eye, at);
    const float aspect = static_cast<float>(options.width) / options.height;
    if (scenario.orthographic) {
        bx::mtxOrtho(projection, -1.25f * aspect, 1.25f * aspect, -1.25f, 1.25f, 0.01f, 10.0f, 0, bgfx::getCaps()->homogeneousDepth);
    } else {
        bx::mtxProj(projection, 50.0f, aspect, 0.01f, 10.0f, bgfx::getCaps()->homogeneousDepth);
    }
    bgfx::setViewTransform(0, view, projection);
    bgfx::setViewTransform(1, view, projection);
    bgfx::touch(0);
    for (size_t i = 0; i < g.mesh.nodeRanges.size(); ++i) {
        const auto& range = g.mesh.nodeRanges[i];
        const std::array<float, 4> color{0.35f + static_cast<float>(i % 3) * 0.15f, 0.65f, 0.85f, scenario.opacity};
        if (scenario.solid) {
            woby::submitTriangleRange(0, g.mesh, g.solid, g.color, model, color, range.triangleIndexOffset, range.triangleIndexCount);
        }
        if (range.pointIndexCount == 0) { continue; }
        if (!options.shader) {
            woby::submitPointSpriteRange(1, g.mesh, g.points, g.color, g.params, model, color,
                scenario.diameter, options.width, options.height, range.pointSpriteIndexOffset, range.pointSpriteIndexCount);
            continue;
        }
        const std::array<float, 4> params{scenario.diameter, static_cast<float>(options.width), static_cast<float>(options.height), 0};
        const std::array<float, 4> base{static_cast<float>(range.pointIndexOffset & 0xffffu),
            static_cast<float>(range.pointIndexOffset >> 16u), 0, 0};
        bgfx::setTransform(model);
        bgfx::setUniform(g.color, color.data());
        bgfx::setUniform(g.params, params.data());
        bgfx::setUniform(g.base, base.data());
        bgfx::setBuffer(0, g.mesh.vertexBuffer, bgfx::Access::Read);
        bgfx::setBuffer(1, g.pointIds, bgfx::Access::Read);
        bgfx::setVertexCount(options.flat ? woby::sceneBufferBytes(range.pointIndexCount, 6) : 4u);
        bgfx::setInstanceCount(options.flat ? 1u : range.pointIndexCount);
        bgfx::setState(woby::renderState(BGFX_STATE_DEPTH_TEST_LEQUAL, true, color, options.flat ? 0 : BGFX_STATE_PT_TRISTRIP));
        bgfx::submit(1, g.points);
    }
}

Json capture(Graphics& g, const Options& options, const Scenario& scenario) {
    std::vector<uint8_t> pixels(static_cast<size_t>(options.width) * options.height * 4);
    bgfx::blit(2, g.readback, 0, 0, g.target);
    const auto ready = bgfx::readTexture(g.readback, pixels.data());
    while (g.frame < ready) { g.frame = bgfx::frame(); }
    if (!options.captures.empty()) {
        std::filesystem::create_directories(options.captures);
        std::ofstream file(options.captures / (std::string(scenario.name) + ".rgba"), std::ios::binary);
        file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
        require(static_cast<bool>(file), "capture write failed");
    }
    size_t covered = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] != 0x10 || pixels[i + 1] != 0x18 || pixels[i + 2] != 0x20) { ++covered; }
    }
    return {{"hash", hashBytes(14695981039346656037ull, pixels.data(), pixels.size())}, {"covered_pixels", covered}};
}

Json distribution(std::vector<double> values) {
    if (values.empty()) { return {{"count", 0}}; }
    std::sort(values.begin(), values.end());
    return {{"count", values.size()}, {"median_ms", values[values.size() / 2]},
        {"p95_ms", values[static_cast<size_t>(static_cast<double>(values.size() - 1) * .95)]},
        {"min_ms", values.front()}, {"max_ms", values.back()}};
}

Json measure(Graphics& g, const Options& options, const woby::Bounds& bounds, const Scenario& scenario) {
    std::cerr << "scenario " << scenario.name << '\n';
    const auto warm = Clock::now();
    uint32_t warmFrames = 0;
    do { submit(g, options, bounds, scenario); g.frame = bgfx::frame(); ++warmFrames; }
    while (elapsed(warm) < (options.quick ? 0 : 1000) || warmFrames < (options.quick ? 3u : 12u));
    drain(g);
    std::vector<double> cpu, wall, gpu, markers;
    cpu.reserve(100000); wall.reserve(100000); gpu.reserve(100000); markers.reserve(100000);
    std::set<uint32_t> gpuFrames, markerFrames;
    const uint32_t firstFrame = g.frame;
    const auto collect = [&] {
        const auto* stats = bgfx::getStats();
        if (stats->gpuTimerFreq <= 0) { return; }
        const double tickMs = 1000.0 / static_cast<double>(stats->gpuTimerFreq);
        if (stats->gpuFrameNum >= firstFrame && stats->gpuTimeEnd > stats->gpuTimeBegin
            && gpuFrames.insert(stats->gpuFrameNum).second) {
            gpu.push_back(static_cast<double>(stats->gpuTimeEnd - stats->gpuTimeBegin) * tickMs);
        }
        for (uint16_t i = 0; i < stats->numViews; ++i) {
            const auto& view = stats->viewStats[i];
            if (view.view == 1 && view.gpuFrameNum >= firstFrame && view.gpuTimeEnd > view.gpuTimeBegin
                && markerFrames.insert(view.gpuFrameNum).second) {
                markers.push_back(static_cast<double>(view.gpuTimeEnd - view.gpuTimeBegin) * tickMs);
            }
        }
    };
    const auto begin = Clock::now();
    do {
        const auto frameStart = Clock::now();
        submit(g, options, bounds, scenario);
        cpu.push_back(elapsed(frameStart));
        g.frame = bgfx::frame();
        wall.push_back(elapsed(frameStart));
        collect();
    } while (elapsed(begin) < options.seconds * 1000 || wall.size() < (options.quick ? 4u : 30u));
    const uint32_t numDraw = bgfx::getStats()->numDraw;
    // Complete the queued scene work; no readbacks contaminate the interval above.
    drain(g);
    const double completedMs = elapsed(begin);
    require(options.quick || (!gpu.empty() && !markers.empty()), "GPU timing samples unavailable");
    submit(g, options, bounds, scenario);
    g.frame = bgfx::frame();
    return {{"name", scenario.name}, {"diameter", scenario.diameter}, {"opacity", scenario.opacity},
        {"solid", scenario.solid}, {"draw_calls", numDraw}, {"cpu_submit", distribution(cpu)},
        {"wall_frame", distribution(wall)}, {"gpu_frame", distribution(gpu)}, {"gpu_markers", distribution(markers)},
        {"completed_interval_ms", completedMs}, {"completed_frames", wall.size()},
        {"completed_mean_frame_ms", completedMs / static_cast<double>(wall.size())},
        {"image", capture(g, options, scenario)}, {"memory", memory()}};
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--quick") { options.quick = true; options.seconds = 0; continue; }
        require(i + 1 < argc, "missing option value");
        const std::string value = argv[++i];
        if (argument == "--model") { options.model = woby::pathFromUtf8(value); }
        else if (argument == "--captures") { options.captures = woby::pathFromUtf8(value); }
        else if (argument == "--mode") {
            require(value == "current" || value == "shader" || value == "flat", "invalid mode");
            options.shader = value != "current"; options.flat = value == "flat";
        }
        else if (argument == "--seconds") { options.seconds = std::stod(value); require(options.seconds > 0 && options.seconds <= 60, "invalid duration"); }
        else if (argument == "--width" || argument == "--height") {
            const int size = std::stoi(value); require(size >= 64 && size <= 4096, "invalid image size");
            if (argument == "--width") { options.width = static_cast<uint16_t>(size); }
            else { options.height = static_cast<uint16_t>(size); }
        } else { throw std::runtime_error("unknown option: " + argument); }
    }
    require(options.model.is_absolute(), "absolute model path required");
    require(options.captures.empty() || options.captures.is_absolute(), "absolute capture path required");
    return options;
}
}

int main(int argc, char** argv) {
    Json output;
    try {
        const auto options = parse(argc, argv);
        static_assert(sizeof(woby::Vertex) == 32 && offsetof(woby::Vertex, position) == 0);
        const auto load = Clock::now();
        auto mesh = woby::loadObjMesh(options.model);
        output = {{"mode", options.flat ? "flat" : options.shader ? "shader" : "current"}, {"model", woby::pathToUtf8(options.model)},
            {"load_ms", elapsed(load)}, {"vertices", mesh.vertices.size()}, {"indices", mesh.indices.size()},
            {"groups", mesh.nodes.size()}, {"width", options.width}, {"height", options.height},
            {"msaa", 1}, {"vsync", false}};
        Graphics g;
        initialize(g, options);
        const auto* caps = bgfx::getCaps();
        output["renderer"] = bgfx::getRendererName(caps->rendererType);
        output["vendor_id"] = caps->vendorId; output["device_id"] = caps->deviceId;
        vertex_probe::meshReadFlags = options.shader ? BGFX_BUFFER_COMPUTE_READ : 0;
        const auto baseStart = Clock::now();
        g.mesh = woby::createGpuMesh(mesh, woby::meshVertexLayout(), woby::pointSpriteVertexLayout());
        output["base_cpu_ms"] = elapsed(baseStart);
        bgfx::touch(0); g.frame = bgfx::frame(); drain(g);
        output["base_ready_ms"] = elapsed(baseStart);
        output["before_points_memory"] = memory();
        output["point_entries"] = g.mesh.pointVertexIndices.size();
        output["point_hash"] = hashBytes(14695981039346656037ull, g.mesh.pointVertexIndices.data(), g.mesh.pointVertexIndices.size() * sizeof(uint32_t));
        const auto enable = Clock::now();
        std::cerr << "prepare " << g.mesh.pointVertexIndices.size() << " markers\n";
        if (options.shader) {
            g.pointIds = bgfx::createIndexBuffer(bgfx::copy(g.mesh.pointVertexIndices.data(),
                woby::sceneBufferBytes(g.mesh.pointVertexIndices.size(), sizeof(uint32_t))),
                BGFX_BUFFER_INDEX32 | BGFX_BUFFER_COMPUTE_READ);
            require(bgfx::isValid(g.pointIds), "point-ID allocation failed");
        } else {
            woby::prepareGpuMeshFeatures(g.mesh, mesh, woby::pointSpriteVertexLayout(), woby::gpuMeshPoints);
        }
        output["enable_cpu_ms"] = elapsed(enable);
        bgfx::touch(0); g.frame = bgfx::frame(); drain(g);
        output["enable_ready_ms"] = elapsed(enable);
        // Force actual use as well: drivers may defer resource work until a draw.
        submit(g, options, mesh.bounds, {"first_draw", false, 4, 1});
        g.frame = bgfx::frame(); drain(g);
        output["first_draw_ready_ms"] = elapsed(enable);
        output["marker_payload_bytes"] = g.mesh.pointVertexIndices.size() * (options.shader ? 4ull : 104ull);
        output["base_payload_bytes"] = mesh.vertices.size() * sizeof(woby::Vertex) + mesh.indices.size() * sizeof(uint32_t);
        output["after_points_memory"] = memory();
        std::vector<Scenario> scenarios{{"points", false, 4, 1}, {"solid_points", true, 4, 1}, {"transparent", false, 8, .4f}};
        if (options.quick) {
            scenarios.push_back({"one_pixel", false, 1, 1});
            scenarios.push_back({"ortho_40", false, 40, .4f, true});
        }
        for (const auto& scenario : scenarios) { output["scenarios"].push_back(measure(g, options, mesh.bounds, scenario)); }
        output["ok"] = true;
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        output["ok"] = false; output["error"] = error.what();
        std::cout << output.dump() << '\n';
        return 1;
    }
}
