#include "frame_pacing.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <limits>

TEST_CASE("mailbox frame pacing supplies twice the target display refresh")
{
    CHECK(woby::mailboxFramePeriodNanoseconds(60.0) == 8'333'333);
    CHECK(woby::mailboxFramePeriodNanoseconds(120.0) == 4'166'667);
    CHECK(woby::mailboxFramePeriodNanoseconds(165.0) == 3'030'303);
    CHECK(woby::mailboxFramePeriodNanoseconds(20.0) == 25'000'000);
    CHECK(woby::mailboxFramePeriodNanoseconds(1000.0) == 500'000);
    for (const double invalid : {0.0, -1.0, 19.999, 1000.001,
             std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK(woby::mailboxFramePeriodNanoseconds(invalid) == 8'333'333);
    }
}

TEST_CASE("mailbox frame pacing uses deadlines without accumulating CPU and wakeup delays")
{
    woby::FramePacingState state;
    REQUIRE(woby::advanceFramePacing(state, 0, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 100'000, 120.0) == 4'066'667);
    // The next frame starts 200 us after its scheduled time, including normal
    // wakeup overshoot and CPU work. That cost does not shift future deadlines.
    CHECK(woby::advanceFramePacing(state, 4'366'667, 120.0) == 3'966'667);
    CHECK(state.nextDeadlineNanoseconds == 12'500'001);
}

TEST_CASE("mailbox frame pacing preserves fractional refresh over many frames")
{
    CHECK(woby::mailboxFramePeriodNanoseconds(59.94) == 8'341'675);
    woby::FramePacingState state;
    REQUIRE(woby::advanceFramePacing(state, 0, 59.94) == 0);
    uint64_t now = 0;
    for (uint64_t frame = 0; frame < 1000; ++frame) {
        now += 500'000 + (frame % 3) * 100'000;
        now += woby::advanceFramePacing(state, now, 59.94);
    }
    CHECK(now == 8'341'675'000);
}

TEST_CASE("mailbox frame pacing skips overruns without waiting on GPU-bound frames or catching up")
{
    woby::FramePacingState state;
    REQUIRE(woby::advanceFramePacing(state, 0, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 30'000'000, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 60'000'000, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 90'000'000, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 90'100'000, 120.0) == 4'066'667);
    // A long debugger pause or window restore starts one fresh interval.
    CHECK(woby::advanceFramePacing(state, 10'000'000'000, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 10'000'000'001, 120.0) == 4'166'666);
}

TEST_CASE("mailbox frame pacing resets immediately when the display rate changes or becomes invalid")
{
    woby::FramePacingState state;
    REQUIRE(woby::advanceFramePacing(state, 0, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 1'000'000, 165.0) == 0);
    CHECK(state.nextDeadlineNanoseconds == 4'030'303);
    CHECK(woby::advanceFramePacing(state, 1'100'000, 165.0) == 2'930'303);
    CHECK(woby::advanceFramePacing(state, 2'000'000, 0.0) == 0);
    CHECK(state.nextDeadlineNanoseconds == 10'333'333);
    CHECK(woby::advanceFramePacing(state, 2'100'000, std::numeric_limits<double>::quiet_NaN()) == 8'233'333);
}

TEST_CASE("mailbox frame pacing supports explicit reset and a restarted monotonic clock")
{
    woby::FramePacingState state;
    REQUIRE(woby::advanceFramePacing(state, 100'000'000, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 1'000'000, 120.0) == 0);
    CHECK(state.nextDeadlineNanoseconds == 5'166'667);
    woby::resetFramePacing(state);
    CHECK(state.periodNanoseconds == 0);
    CHECK(woby::advanceFramePacing(state, 1'100'000, 120.0) == 0);
    CHECK(state.nextDeadlineNanoseconds == 5'266'667);
}

TEST_CASE("mailbox frame pacing cannot wrap a deadline into the past")
{
    woby::FramePacingState state;
    const auto maximum = std::numeric_limits<uint64_t>::max();
    REQUIRE(woby::advanceFramePacing(state, maximum - 100, 120.0) == 0);
    CHECK(state.nextDeadlineNanoseconds == maximum);
    CHECK(woby::advanceFramePacing(state, maximum - 50, 120.0) == 50);
    CHECK(woby::advanceFramePacing(state, maximum, 120.0) == 0);
}

TEST_CASE("minimized frame pacing stays bounded and resumes the display rate immediately")
{
    woby::FramePacingState state;
    REQUIRE(woby::advanceFramePacing(state, 0, 165.0) == 0);
    CHECK(woby::advanceFramePacing(state, 1'000'000, 165.0, false) == 0);
    CHECK(woby::advanceFramePacing(state, 2'000'000, 165.0, false) == 49'000'000);
    CHECK(woby::advanceFramePacing(state, 52'000'000, 0.0, false) == 49'000'000);
    CHECK(woby::advanceFramePacing(state, 53'000'000, 120.0) == 0);
    CHECK(woby::advanceFramePacing(state, 54'000'000, 120.0) == 3'166'667);
}
