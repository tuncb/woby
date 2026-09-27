#pragma once
// Disposable measurement adapter. No production state or renderer changes.
#include "performance_log.h"
#include "ui_operations.h"
#include <SDL3/SDL.h>
#include <bgfx/bgfx.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace render_fps {
using Json = nlohmann::json;
using Clock = woby::PerformanceClock;
inline std::string environment(const char* name) {
    char* value = nullptr; size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) { return {}; }
    std::string result(value); std::free(value); return result;
}
struct Sample {
    size_t scenario = 0;
    bool measured = false;
    uint32_t frame = 0, gpuFrame = 0, draws = 0, width = 0, height = 0;
    double wall = 0, time = 0;
    woby::FrameTimings timing;
};
struct Capture {
    Json config;
    std::filesystem::path output;
    std::vector<Sample> samples;
    woby::SceneCamera base;
    size_t scenario = 0, frames = 0, measuredFrames = 0;
    bool initialized = false, measuring = false, pendingNext = false, done = false;
    Clock::time_point start{}, measureStart{}, lastEnd{};
    double elapsed = 0;
};
inline void initialize(Capture& c, woby::UiState& ui) {
    auto& config = c.config; auto& output = c.output;
    auto& samples = c.samples; auto& base = c.base;
    const auto path = environment("WOBY_RENDER_FPS_CONFIG");
    if (path.empty()) { throw std::runtime_error("Missing WOBY_RENDER_FPS_CONFIG"); }
    std::ifstream input(path); input >> config;
    output = config.at("output").get<std::string>();
    samples.reserve(200000);
    woby::setCameraView(ui, woby::CameraView::isometric);
    woby::fitCameraToScene(ui);
    base = ui.camera;
    Json metadata = config;
    metadata["renderer"] = bgfx::getRendererName(bgfx::getRendererType());
    metadata["vendor_id"] = bgfx::getCaps()->vendorId;
    metadata["device_id"] = bgfx::getCaps()->deviceId;
    metadata["distance"] = base.distance;
    metadata["near_plane"] = base.nearPlane;
    for (const auto& file : ui.files) {
        metadata["models"].push_back({{"path", file.path.string()}, {"vertices", file.mesh.vertices.size()},
            {"triangles", file.mesh.indices.size()/3}, {"groups", file.groupSettings.size()}});
    }
    std::ofstream(output.string() + ".json") << metadata.dump(2);
}
inline const Json& current(const Capture& c) { return c.config.at("scenarios").at(c.scenario); }
inline bool dragging(const Capture& c) { return current(c).value("drag", false); }
inline std::array<float, 2> mouse(const Capture& c, float x, float y, float width, float height) {
    const auto pointer = current(c).value("pointer", std::string("outside"));
    if (pointer == "outside") { return {-100.0f, -100.0f}; }
    const float offset = pointer == "sweep" ? 0.15f * std::sin(static_cast<float>(c.elapsed)*2.0f) : 0.0f;
    return {x + width*(0.5f + offset), y + height*0.5f};
}
inline void prepare(Capture& c, woby::UiState& ui, SDL_Window* window, uint32_t& flags) {
    auto& done = c.done; auto& pendingNext = c.pendingNext; auto& scenario = c.scenario;
    auto& initialized = c.initialized; auto& start = c.start; auto& lastEnd = c.lastEnd;
    auto& frames = c.frames; auto& measuredFrames = c.measuredFrames; auto& measuring = c.measuring;
    auto& elapsed = c.elapsed; auto& measureStart = c.measureStart;
    const auto& config = c.config; const auto& base = c.base;
    if (done) { return; }
    if (pendingNext) {
        ++scenario; initialized = false; pendingNext = false;
    }
    const auto& s = current(c);
    if (!initialized) {
        woby::setAllSceneVisible(ui, true);
        woby::setAllSceneRenderModes(ui, woby::UiRenderMode::solidMesh, s.value("solid", true));
        woby::setAllSceneRenderModes(ui, woby::UiRenderMode::triangles, s.value("edges", false));
        woby::setAllSceneRenderModes(ui, woby::UiRenderMode::vertices, s.value("vertices", false));
        woby::setMasterVertexPointSize(ui, s.value("point_size", 4.0f));
        for (auto& file : ui.files) { woby::setFileOpacity(file.fileSettings, s.value("opacity", 1.0f)); }
        woby::clearSceneSelection(ui);
        if (s.value("selected", false) && !ui.files.empty()) { woby::selectSceneObject(ui, ui.files.front().objectId); }
        woby::setViewerPaneVisible(ui, s.value("pane", false));
        woby::setPropertiesPaneVisible(ui, s.value("inspector", false));
        woby::setShowGrid(ui, s.value("helpers", false));
        woby::setShowOrigin(ui, s.value("helpers", false));
        woby::setShowDimensions(ui, s.value("selected", false));
        woby::setAllSceneVisible(ui, s.value("visible", true));
        const auto width = s.value("width", 1280), height = s.value("height", 720);
        SDL_SetWindowSize(window, width, height);
        flags = (s.value("vsync", false) ? BGFX_RESET_VSYNC : 0u)
            | (s.value("msaa", true) ? BGFX_RESET_MSAA_X4 : 0u);
        bgfx::reset(static_cast<uint32_t>(width), static_cast<uint32_t>(height), flags);
        start = lastEnd = Clock::now(); frames = measuredFrames = 0; measuring = false;
        initialized = true;
        std::cout << "scenario " << s.at("name").get<std::string>() << std::endl;
    }
    elapsed = woby::millisecondsBetween(start, Clock::now())/1000.0;
    if (!measuring && frames >= 20 && elapsed >= config.value("warmup_seconds", 1.5)) {
        measuring = true; measureStart = Clock::now();
    }
    // Identical two-second path repeats during warmup and measurement.
    const float phase = static_cast<float>(elapsed)*3.141592654f;
    const auto motion = s.value("motion", std::string("static"));
    woby::CameraPlacement p;
    p.target = base.target;
    p.yawDegrees = base.yawRadians*57.295779513f;
    p.pitchDegrees = base.pitchRadians*57.295779513f;
    p.rollDegrees = base.rollRadians*57.295779513f;
    p.distance = base.distance*s.value("distance_factor", 1.0f);
    p.nearPlane = std::max(base.nearPlane, base.distance*s.value("near_fraction", 0.0f));
    if (motion == "orbit") { *p.yawDegrees += 30.0f*std::sin(phase); }
    if (motion == "zoom") { *p.distance *= std::exp(0.55f*std::sin(phase)); }
    woby::setUiCamera(ui, p);
    if (motion == "pan") {
        woby::CameraNavigation nav;
        nav.right = base.distance*0.15f*std::sin(phase);
        woby::navigateUiCamera(ui, nav);
    }
    if (s.value("offscreen", false)) {
        woby::CameraNavigation nav; nav.right = base.distance*10.0f;
        woby::navigateUiCamera(ui, nav);
    }
}
inline void end(Capture& c, woby::UiState& ui, const woby::FrameTimings& timing, uint32_t frame) {
    auto& done = c.done; auto& pendingNext = c.pendingNext; auto& frames = c.frames;
    auto& measuredFrames = c.measuredFrames; auto& lastEnd = c.lastEnd; auto& samples = c.samples;
    const auto& config = c.config; const auto& output = c.output;
    const auto scenario = c.scenario; const auto measuring = c.measuring;
    const auto start = c.start; const auto measureStart = c.measureStart;
    if (done) { return; }
    const auto now = Clock::now();
    const auto* stats = bgfx::getStats();
    Sample sample;
    sample.scenario = scenario; sample.measured = measuring; sample.frame = frame;
    sample.gpuFrame = stats->gpuFrameNum; sample.draws = stats->numDraw;
    sample.width = stats->width; sample.height = stats->height;
    sample.wall = woby::millisecondsBetween(lastEnd, now);
    sample.time = woby::millisecondsBetween(start, now);
    sample.timing = timing;
    samples.push_back(sample); lastEnd = now; ++frames;
    if (frames == 3 && config.value("captures", false)) {
        const auto path = output.string() + "-" + current(c).at("name").get<std::string>() + ".tga";
        bgfx::requestScreenShot(BGFX_INVALID_HANDLE, path.c_str());
    }
    if (measuring) { ++measuredFrames; }
    if (measuring && measuredFrames >= config.value("min_frames", 30u)
        && woby::millisecondsBetween(measureStart, now) >= config.value("measure_seconds", 3.0)*1000.0) {
        if (scenario+1 == config.at("scenarios").size()) { done = true; woby::requestQuit(ui); }
        else { pendingNext = true; }
    }
}
inline void save(const Capture& c) {
    const auto& output = c.output; const auto& samples = c.samples;
    std::ofstream stream(output.string() + ".csv");
    stream << "scenario,measured,frame,gpu_frame,draws,width,height,wall_ms,time_ms,total_ms,gpu_ms,render_cpu_ms";
    for (size_t i = 0; i < static_cast<size_t>(woby::FrameStage::count); ++i) {
        stream << ',' << woby::frameStageName(static_cast<woby::FrameStage>(i));
    }
    stream << '\n' << std::setprecision(10);
    for (const auto& s : samples) {
        stream << s.scenario << ',' << s.measured << ',' << s.frame << ',' << s.gpuFrame << ',' << s.draws
            << ',' << s.width << ',' << s.height << ',' << s.wall << ',' << s.time << ',' << s.timing.totalMilliseconds
            << ',' << (s.timing.hasBgfxGpuFrameMilliseconds ? s.timing.bgfxGpuFrameMilliseconds : -1.0)
            << ',' << s.timing.bgfxCpuSubmitMilliseconds;
        for (auto ms : s.timing.stageMilliseconds) { stream << ',' << ms; }
        stream << '\n';
    }
}
} // namespace render_fps
