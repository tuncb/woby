#include "marker_pick_logic.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace woby {

uint32_t appendMarkerDraw(MarkerDrawList& list, MarkerDraw draw, float pointSize)
{
    if (draw.count == 0 || list.nextId + draw.count - 1 > std::numeric_limits<uint32_t>::max()) {
        return 0;
    }
    draw.firstId = static_cast<uint32_t>(list.nextId);
    list.nextId += draw.count;
    list.largestPoint = std::max(list.largestPoint, pointSize);
    list.draws.push_back(draw);
    return draw.firstId;
}

std::optional<uint32_t> decodeMarkerPixel(const MarkerPixel& pixel)
{
    for (size_t i = 0; i < 2; ++i) {
        if (!std::isfinite(pixel[i]) || pixel[i] < 0 || pixel[i] > 65535
            || std::floor(pixel[i]) != pixel[i]) { return {}; }
    }
    return static_cast<uint32_t>(pixel[0]) | (static_cast<uint32_t>(pixel[1]) << 16u);
}

const MarkerDraw* findMarkerDraw(std::span<const MarkerDraw> draws, uint32_t id)
{
    if (id == 0) { return nullptr; }
    for (const auto& draw : draws) {
        if (id >= draw.firstId && id - draw.firstId < draw.count) { return &draw; }
    }
    return nullptr;
}

std::array<float, 3> transformMarkerPosition(const std::array<float, 16>& model,
    const std::array<float, 3>& local)
{
    const float w = model[3]*local[0] + model[7]*local[1] + model[11]*local[2] + model[15];
    const float divisor = std::abs(w) > 0.000001f ? w : 1.0f;
    std::array<float, 3> result{};
    for (size_t k = 0; k < 3; ++k) {
        result[k] = (model[k]*local[0] + model[4+k]*local[1] + model[8+k]*local[2] + model[12+k]) / divisor;
    }
    return result;
}

bool acceptMarkerCompletion(uint64_t epoch, uint64_t currentEpoch, uint64_t sequence, uint64_t latestSequence)
{
    return epoch == currentEpoch && sequence > latestSequence;
}

bool markerFrameReached(uint32_t frame, uint32_t target)
{
    return frame - target < (uint32_t{1} << 31u);
}

} // namespace woby
