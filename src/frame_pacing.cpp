#include "frame_pacing.h"

#include <cmath>
#include <limits>

namespace woby {
namespace {

uint64_t followingDeadline(uint64_t timestamp, uint64_t period)
{
    const auto maximum = std::numeric_limits<uint64_t>::max();
    return timestamp > maximum - period ? maximum : timestamp + period;
}

} // namespace

uint64_t mailboxFramePeriodNanoseconds(double displayRefreshRate)
{
    if (!std::isfinite(displayRefreshRate) || displayRefreshRate < 20.0 || displayRefreshRate > 1000.0) {
        displayRefreshRate = 60.0;
    }
    return static_cast<uint64_t>(std::llround(1'000'000'000.0 / (displayRefreshRate * 2.0)));
}

void resetFramePacing(FramePacingState& state)
{
    state = {};
}

uint64_t advanceFramePacing(FramePacingState& state, uint64_t nowNanoseconds, double displayRefreshRate, bool active)
{
    const auto period = active ? mailboxFramePeriodNanoseconds(displayRefreshRate) : uint64_t{50'000'000};
    if (state.periodNanoseconds != period || nowNanoseconds < state.lastTimestampNanoseconds) {
        state = {period, followingDeadline(nowNanoseconds, period), nowNanoseconds};
        return 0;
    }
    state.lastTimestampNanoseconds = nowNanoseconds;
    if (nowNanoseconds >= state.nextDeadlineNanoseconds) {
        state.nextDeadlineNanoseconds = followingDeadline(nowNanoseconds, period);
        return 0;
    }
    const auto delay = state.nextDeadlineNanoseconds - nowNanoseconds;
    // Advance from the planned deadline, so normal wakeup/CPU-time variation
    // does not accumulate into a progressively slower cap.
    state.nextDeadlineNanoseconds = followingDeadline(state.nextDeadlineNanoseconds, period);
    return delay;
}

} // namespace woby
