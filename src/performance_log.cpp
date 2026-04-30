#include "performance_log.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace woby {
namespace {

constexpr size_t frameStageCount = static_cast<size_t>(FrameStage::count);

void appendStageTimings(std::ostringstream& stream, const std::array<double, frameStageCount>& timings)
{
    for (size_t index = 0; index < frameStageCount; ++index) {
        const auto stage = static_cast<FrameStage>(index);
        if (index > 0u) {
            stream << ' ';
        }
        stream << frameStageName(stage) << "_ms=" << timings[index];
    }
}

} // namespace

const char* frameStageName(FrameStage stage)
{
    switch (stage) {
    case FrameStage::events:
        return "events";
    case FrameStage::pendingIo:
        return "pending_io";
    case FrameStage::stateUpdate:
        return "state_update";
    case FrameStage::imguiBuild:
        return "imgui_build";
    case FrameStage::sceneState:
        return "scene_state";
    case FrameStage::viewSetup:
        return "view_setup";
    case FrameStage::hoverPick:
        return "hover_pick";
    case FrameStage::submitScene:
        return "submit_scene";
    case FrameStage::submitHelpers:
        return "submit_helpers";
    case FrameStage::imguiRender:
        return "imgui_render";
    case FrameStage::bgfxFrame:
        return "bgfx_frame";
    case FrameStage::count:
        break;
    }

    throw std::runtime_error("Unsupported frame stage.");
}

double millisecondsBetween(PerformanceClock::time_point begin, PerformanceClock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

void accumulateFrameTiming(FrameTimingAccumulator& accumulator, const FrameTimings& timings)
{
    ++accumulator.frameCount;
    accumulator.totalMilliseconds += timings.totalMilliseconds;
    accumulator.maxFrameMilliseconds = std::max(accumulator.maxFrameMilliseconds, timings.totalMilliseconds);

    for (size_t index = 0; index < frameStageCount; ++index) {
        accumulator.stageTotalMilliseconds[index] += timings.stageMilliseconds[index];
        accumulator.stageMaxMilliseconds[index] =
            std::max(accumulator.stageMaxMilliseconds[index], timings.stageMilliseconds[index]);
    }
}

void resetFrameTimingAccumulator(FrameTimingAccumulator& accumulator)
{
    accumulator = FrameTimingAccumulator{};
}

void logDuration(const char* name, double milliseconds)
{
    spdlog::info("perf event={} duration_ms={}", name, milliseconds);
}

void logFrameSummary(const FrameTimingAccumulator& accumulator)
{
    if (accumulator.frameCount == 0u) {
        return;
    }

    std::array<double, frameStageCount> averageStageMilliseconds{};
    for (size_t index = 0; index < frameStageCount; ++index) {
        averageStageMilliseconds[index] =
            accumulator.stageTotalMilliseconds[index] / static_cast<double>(accumulator.frameCount);
    }

    std::ostringstream averages;
    appendStageTimings(averages, averageStageMilliseconds);

    std::ostringstream maximums;
    appendStageTimings(maximums, accumulator.stageMaxMilliseconds);

    spdlog::info(
        "perf frame_summary frames={} avg_frame_ms={} max_frame_ms={} avg_stages=[{}] max_stages=[{}]",
        accumulator.frameCount,
        accumulator.totalMilliseconds / static_cast<double>(accumulator.frameCount),
        accumulator.maxFrameMilliseconds,
        averages.str(),
        maximums.str());
}

void logSlowFrame(const FrameTimings& timings, double thresholdMilliseconds)
{
    if (timings.totalMilliseconds < thresholdMilliseconds) {
        return;
    }

    std::ostringstream stages;
    appendStageTimings(stages, timings.stageMilliseconds);

    if (timings.hasBgfxGpuFrameMilliseconds) {
        spdlog::info(
            "perf slow_frame frame={} total_ms={} threshold_ms={} stages=[{}] bgfx_cpu_frame_ms={} bgfx_cpu_submit_ms={} bgfx_gpu_frame_ms={}",
            timings.frameIndex,
            timings.totalMilliseconds,
            thresholdMilliseconds,
            stages.str(),
            timings.bgfxCpuFrameMilliseconds,
            timings.bgfxCpuSubmitMilliseconds,
            timings.bgfxGpuFrameMilliseconds);
        return;
    }

    spdlog::info(
        "perf slow_frame frame={} total_ms={} threshold_ms={} stages=[{}] bgfx_cpu_frame_ms={} bgfx_cpu_submit_ms={}",
        timings.frameIndex,
        timings.totalMilliseconds,
        thresholdMilliseconds,
        stages.str(),
        timings.bgfxCpuFrameMilliseconds,
        timings.bgfxCpuSubmitMilliseconds);
}

} // namespace woby
