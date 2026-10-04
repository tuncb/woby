#pragma once

#include "performance_log.h"

#include <stdexcept>
#include <vector>

namespace woby {

// Runtime measurement data, deliberately outside UiState. Never owns renderer resources.
struct FrameEnvironment {
    int windowWidth = 0, windowHeight = 0;
    int drawableWidth = 0, drawableHeight = 0;
    uint32_t viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
    uint32_t submittedWidth = 0, submittedHeight = 0, displayId = 0;
    uint64_t windowFlags = 0, pacingPeriodNanoseconds = 0;
    uint64_t sceneEditRevision = 0, sceneGeneration = 0;
    float pixelDensity = 0, displayScale = 0, uiScale = 0;
    float scenePaneWidth = 0, propertiesPaneWidth = 0;
    double refreshRate = 0;
    bool scenePaneVisible = false, propertiesPaneVisible = false;
    bool presentationSubmitted = false, mailbox = false;
    friend bool operator==(const FrameEnvironment&, const FrameEnvironment&) = default;
};

struct FrameEvent {
    FrameTimings timing;
    FrameEnvironment environment;
    // target xyz, yaw/pitch/roll radians, distance, vertical FOV degrees, near plane.
    std::array<float, 9> camera{};
    double completedMilliseconds = 0;
};

inline constexpr size_t maxCapturedFrames = 16384;
struct FrameCapture {
    bool active = false;
    uint64_t afterFrame = 0, droppedFrames = 0;
    PerformanceClock::time_point start;
    std::vector<FrameEvent> events;
};

inline void beginFrameCapture(FrameCapture& capture, uint64_t currentFrame, PerformanceClock::time_point now)
{
    if (capture.active) { throw std::invalid_argument("A performance capture is already active."); }
    capture.events.clear();
    capture.events.reserve(maxCapturedFrames);
    capture.afterFrame = currentFrame;
    capture.start = now;
    capture.droppedFrames = 0;
    capture.active = true;
}

inline void captureFrame(FrameCapture& capture, const FrameEvent& event)
{
    // The begin RPC frame includes setup work and is intentionally outside the window.
    if (!capture.active || event.timing.frameIndex <= capture.afterFrame) { return; }
    if (capture.events.size() == maxCapturedFrames) { ++capture.droppedFrames; return; }
    capture.events.push_back(event);
}

inline void endFrameCapture(FrameCapture& capture)
{
    if (!capture.active) { throw std::invalid_argument("No performance capture is active."); }
    capture.active = false;
}

} // namespace woby
