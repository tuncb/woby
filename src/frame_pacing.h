#pragma once

#include <cstdint>

namespace woby {

struct FramePacingState {
    uint64_t periodNanoseconds = 0;
    uint64_t nextDeadlineNanoseconds = 0;
    uint64_t lastTimestampNanoseconds = 0;
};

// Mailbox receives at most twice the target display's nominal refresh rate.
// Missing/nonfinite rates and values outside 20..1000 Hz use 60 Hz instead.
uint64_t mailboxFramePeriodNanoseconds(double displayRefreshRate);

void resetFramePacing(FramePacingState& state);

// Call once per frame using a monotonic nanosecond clock, after GPU resource
// reuse has completed. Sleep for the returned duration. First use and rate
// changes render immediately. Inactive windows are limited to 20 FPS.
// Missed deadlines also render immediately and
// discard the old schedule instead of adding waits or issuing catch-up frames.
uint64_t advanceFramePacing(FramePacingState& state, uint64_t nowNanoseconds, double displayRefreshRate,
    bool active = true);

} // namespace woby
