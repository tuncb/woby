#pragma once

#include "parallel_work.h"
#include <array>
#include <cstdint>
#include <utility>

namespace woby {

inline constexpr size_t spatialKeyBatchSize = 65536;

// Stable LSD radix sort. Each input chunk scans in order and writes to its own
// range in each bucket. Prefixes use input chunk order, not worker finish order.
template <typename Key, typename Morton, typename Check>
void stableSpatialRadixSort(std::vector<Key>& keys, const Morton& morton, const Check& check)
{
    check();
    if (keys.size() < 2) { return; }
    const size_t chunks = std::min(size_t{analysisWorkerLimit()} + 1,
        size_t{1} + (keys.size() - 1) / spatialKeyBatchSize);
    const auto bounds = [&](size_t chunk) {
        const size_t size = keys.size() / chunks, extra = keys.size() % chunks;
        const size_t begin = chunk * size + std::min(chunk, extra);
        return std::pair{begin, begin + size + (chunk < extra ? 1 : 0)};
    };
    std::vector<Key> scratch(keys.size());
    std::vector<std::array<size_t, 256>> offsets(chunks);
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
            for (size_t chunk = first; chunk < end; ++chunk) {
                auto& histogram = offsets[chunk]; histogram.fill(0);
                const auto [begin, finish] = bounds(chunk);
                for (size_t i = begin; i < finish; ++i) {
                    if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                    ++histogram[(morton(keys[i]) >> shift) & 255];
                }
            }
        }, 2);
        size_t total = 0;
        for (size_t bucket = 0; bucket < 256; ++bucket) {
            for (auto& chunk : offsets) {
                const size_t count = chunk[bucket]; chunk[bucket] = total; total += count;
            }
        }
        parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
            for (size_t chunk = first; chunk < end; ++chunk) {
                auto& output = offsets[chunk];
                const auto [begin, finish] = bounds(chunk);
                for (size_t i = begin; i < finish; ++i) {
                    if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                    const auto key = keys[i];
                    scratch[output[(morton(key) >> shift) & 255]++] = key;
                }
            }
        }, 2);
        keys.swap(scratch);
    }
    check();
}

} // namespace woby
