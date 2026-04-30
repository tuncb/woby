#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace woby {

enum class FrameStage {
    events,
    pendingIo,
    stateUpdate,
    imguiBuild,
    sceneState,
    viewSetup,
    hoverPick,
    submitScene,
    submitHelpers,
    imguiRender,
    bgfxFrame,
    count,
};

using PerformanceClock = std::chrono::steady_clock;

struct FrameTimings {
    uint64_t frameIndex = 0;
    std::array<double, static_cast<size_t>(FrameStage::count)> stageMilliseconds{};
    double totalMilliseconds = 0.0;
    double bgfxCpuFrameMilliseconds = 0.0;
    double bgfxCpuSubmitMilliseconds = 0.0;
    double bgfxGpuFrameMilliseconds = 0.0;
    bool hasBgfxGpuFrameMilliseconds = false;
};

struct FrameTimingAccumulator {
    uint64_t frameCount = 0;
    std::array<double, static_cast<size_t>(FrameStage::count)> stageTotalMilliseconds{};
    std::array<double, static_cast<size_t>(FrameStage::count)> stageMaxMilliseconds{};
    double totalMilliseconds = 0.0;
    double maxFrameMilliseconds = 0.0;
};

const char* frameStageName(FrameStage stage);
double millisecondsBetween(PerformanceClock::time_point begin, PerformanceClock::time_point end);
void accumulateFrameTiming(FrameTimingAccumulator& accumulator, const FrameTimings& timings);
void resetFrameTimingAccumulator(FrameTimingAccumulator& accumulator);
void logDuration(const char* name, double milliseconds);
void logFrameSummary(const FrameTimingAccumulator& accumulator);
void logSlowFrame(const FrameTimings& timings, double thresholdMilliseconds);

} // namespace woby
