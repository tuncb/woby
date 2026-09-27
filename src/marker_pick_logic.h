#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace woby {

using MarkerPixel = std::array<float, 4>;

// One range per submitted draw, including repeated instances of the same mesh.
// Only values are retained across frames; no scene pointers are borrowed.
struct MarkerDraw {
    std::array<float, 16> model{};
    uint32_t firstId = 0, pointOffset = 0, count = 0;
    size_t fileIndex = 0;
    uint64_t fileId = 0;
};

struct MarkerDrawList {
    std::vector<MarkerDraw> draws;
    uint64_t nextId = 1;
    float largestPoint = 0;
};

[[nodiscard]] uint32_t appendMarkerDraw(MarkerDrawList& list, MarkerDraw draw, float pointSize);
[[nodiscard]] std::optional<uint32_t> decodeMarkerPixel(const MarkerPixel& pixel);
[[nodiscard]] const MarkerDraw* findMarkerDraw(std::span<const MarkerDraw> draws, uint32_t id);
[[nodiscard]] std::array<float, 3> transformMarkerPosition(
    const std::array<float, 16>& model, const std::array<float, 3>& local);
[[nodiscard]] bool acceptMarkerCompletion(uint64_t epoch, uint64_t currentEpoch,
    uint64_t sequence, uint64_t latestSequence);
// bgfx frame counters wrap after UINT32_MAX.
[[nodiscard]] bool markerFrameReached(uint32_t frame, uint32_t target);

} // namespace woby
