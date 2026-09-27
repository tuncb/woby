#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

namespace marker_experiment {
struct Draw {
    std::array<float,16> model{};
    uint32_t begin=0, count=0;
};
inline std::optional<size_t> drawForIdentity(std::span<const Draw> draws,uint32_t id) {
    if (!id) { return {}; }
    const uint32_t rank=id-1;
    for (size_t i=0;i<draws.size();++i) {
        if (rank>=draws[i].begin && rank-draws[i].begin<draws[i].count) { return i; }
    }
    return {};
}
using Pixel = std::array<float, 4>; // rank+1 in two 16-bit limbs, depth, draw ordinal
inline uint32_t identity(const Pixel& p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 16u);
}
inline bool validPixel(const Pixel& p) {
    return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]) && std::isfinite(p[3])
        && p[0] >= 0 && p[0] <= 65535 && p[1] >= 0 && p[1] <= 65535
        && std::floor(p[0]) == p[0] && std::floor(p[1]) == p[1]
        && p[2] >= 0 && p[2] <= 1 && p[3] >= 0 && std::floor(p[3]) == p[3];
}
// Independent CPU reference for the 7x7 neighborhood, four entries per pixel.
inline Pixel selectPixel(std::span<const Pixel> pixels, float x, float y, int width, int height, int samples) {
    Pixel best{}; float bestDistance = 1e30f;
    for (int i = 0; i < 49; ++i) {
        const int px = static_cast<int>(std::floor(x)) + i % 7 - 3;
        const int py = static_cast<int>(std::floor(y)) + i / 7 - 3;
        if (px < 0 || py < 0 || px >= width || py >= height) { continue; }
        const float dx = static_cast<float>(px) + .5f - x, dy = static_cast<float>(py) + .5f - y;
        const float distance = dx*dx + dy*dy;
        if (distance > 9) { continue; }
        for (int s = 0; s < samples; ++s) {
            const auto& candidate = pixels[static_cast<size_t>(i*4+s)];
            if (!validPixel(candidate) || identity(candidate) == 0) { continue; }
            if (identity(best) == 0 || distance < bestDistance || (distance == bestDistance
                && (candidate[2] < best[2] || (candidate[2] == best[2]
                && (identity(candidate) < identity(best) || (identity(candidate) == identity(best) && candidate[3] < best[3])))))) {
                best = candidate; bestDistance = distance;
            }
        }
    }
    return best;
}
inline std::optional<uint32_t> acceptResult(const Pixel& p, uint32_t issued, uint32_t latest,
    size_t scenario, size_t currentScenario, size_t drawCount) {
    if (!validPixel(p) || issued < latest || scenario != currentScenario
        || (identity(p)!=0 && p[3] >= static_cast<float>(drawCount))) { return {}; }
    return identity(p);
}
}
