#include "frame_pacing.h"
#include <iostream>
int main() {
    woby::FramePacingState state;
    const auto first = woby::advanceFramePacing(state, 0, 60, false);
    const auto second = woby::advanceFramePacing(state, 100'000, 60, false);
    if (first || second != 900'000 || state.periodNanoseconds != 1'000'000) {
        std::cerr << "Research inactive frame period mismatch\n";
        return 1;
    }
    woby::resetFramePacing(state);
    if (woby::advanceFramePacing(state, 0, 60, true) || state.periodNanoseconds != woby::mailboxFramePeriodNanoseconds(60)) {
        std::cerr << "Active frame period mismatch\n";
        return 1;
    }
    return 0;
}
