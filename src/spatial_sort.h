#pragma once

#include "parallel_work.h"
#include <array>
#include <cstdint>
#include <utility>

namespace woby {

inline constexpr size_t spatialKeyBatchSize = 65536;

namespace spatial_sort_detail {

// Each input chunk scans in order and writes to its own range in each bucket.
// Prefixes use input chunk order, not worker finish order.
template <typename Key, typename Morton, typename Check>
void byteSort(std::vector<Key>& keys, const Morton& morton, const Check& check, unsigned maximumHelpers,
    uint32_t different = UINT32_MAX)
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
        if (((different >> shift) & 255) == 0) { continue; }
        parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
            for (size_t chunk = first; chunk < end; ++chunk) {
                auto& histogram = offsets[chunk]; histogram.fill(0);
                const auto [begin, finish] = bounds(chunk);
                for (size_t i = begin; i < finish; ++i) {
                    if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                    ++histogram[(morton(keys[i]) >> shift) & 255];
                }
            }
        }, 2, maximumHelpers);
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
        }, 2, maximumHelpers);
        keys.swap(scratch);
    }
    check();
}

// Partition on the high 13 bits, then sort each range on its two low digits.
// The small ranges reuse cache instead of scattering across the whole
// key array on every pass. A range's digit counts do not change between passes,
// so one scan supplies both local histograms. Return the varying key bits if
// the caller must use the byte sort, or zero when the sort is complete.
template <typename Key, typename Morton, typename Check>
uint32_t partitionedSort(std::vector<Key>& keys, const Morton& morton, const Check& check, unsigned maximumHelpers)
{
    constexpr unsigned highShift = 19, lowBits = 10;
    constexpr size_t highBuckets = size_t{1} << (32 - highShift), lowBuckets = size_t{1} << lowBits;
    const size_t chunks = std::min(size_t{analysisWorkerLimit()} + 1,
        size_t{1} + (keys.size() - 1) / spatialKeyBatchSize);
    const auto bounds = [&](size_t chunk) {
        const size_t size = keys.size() / chunks, extra = keys.size() % chunks;
        const size_t begin = chunk * size + std::min(chunk, extra);
        return std::pair{begin, begin + size + (chunk < extra ? 1 : 0)};
    };
    std::vector<std::array<size_t, highBuckets>> offsets(chunks);
    std::vector<uint32_t> differences(chunks);
    const uint32_t firstKey = static_cast<uint32_t>(morton(keys.front()));
    parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
        for (size_t chunk = first; chunk < end; ++chunk) {
            auto& histogram = offsets[chunk]; histogram.fill(0);
            uint32_t different = 0;
            const auto [begin, finish] = bounds(chunk);
            for (size_t i = begin; i < finish; ++i) {
                if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                const uint32_t key = static_cast<uint32_t>(morton(keys[i]));
                ++histogram[key >> highShift]; different |= key ^ firstKey;
            }
            differences[chunk] = different;
        }
    }, 2, maximumHelpers);
    uint32_t different = 0;
    for (const auto value : differences) { different |= value; }
    if (different == 0) { return 0; }
    std::vector<size_t> starts(highBuckets + 1), order;
    order.reserve(highBuckets);
    size_t total = 0, largest = 0;
    for (size_t bucket = 0; bucket < highBuckets; ++bucket) {
        starts[bucket] = total;
        for (auto& chunk : offsets) {
            const size_t count = chunk[bucket]; chunk[bucket] = total; total += count;
        }
        const auto count = total - starts[bucket];
        largest = std::max(largest, count);
        if (count) { order.push_back(bucket); }
    }
    starts[highBuckets] = total;
    // Outliers can put most keys in one partition. Keep that work parallel with
    // the byte sort instead of making one range the serial tail of this job.
    if (largest > keys.size() / 4) { return different; }
    std::vector<Key> scratch(keys.size());
    parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
        for (size_t chunk = first; chunk < end; ++chunk) {
            auto& output = offsets[chunk];
            const auto [begin, finish] = bounds(chunk);
            for (size_t i = begin; i < finish; ++i) {
                if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                const auto key = keys[i];
                scratch[output[static_cast<uint32_t>(morton(key)) >> highShift]++] = key;
            }
        }
    }, 2, maximumHelpers);
    // Schedule long ranges first. Each range owns disjoint parts of both arrays;
    // scheduling changes cannot change the order of equal keys.
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return starts[a + 1] - starts[a] > starts[b + 1] - starts[b];
    });
    parallelWorkBatches(order.size(), 1, check, [&](size_t first, size_t end) {
        for (size_t task = first; task < end; ++task) {
            const size_t bucket = order[task], begin = starts[bucket], finish = starts[bucket + 1];
            std::array<std::array<size_t, lowBuckets>, 2> histograms{};
            for (size_t i = begin; i < finish; ++i) {
                if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                const uint32_t key = static_cast<uint32_t>(morton(scratch[i]));
                ++histograms[0][key & (lowBuckets - 1)];
                ++histograms[1][(key >> lowBits) & (lowBuckets - 1)];
            }
            auto* input = scratch.data(); auto* output = keys.data();
            for (unsigned pass = 0; pass < 2; ++pass) {
                auto& histogram = histograms[pass];
                if (*std::max_element(histogram.begin(), histogram.end()) == finish - begin) { continue; }
                size_t prefix = begin;
                for (auto& value : histogram) { const auto count = value; value = prefix; prefix += count; }
                for (size_t i = begin; i < finish; ++i) {
                    if ((i - begin) % spatialKeyBatchSize == 0) { check(); }
                    const auto key = input[i];
                    output[histogram[(morton(key) >> (pass * lowBits)) & (lowBuckets - 1)]++] = key;
                }
                std::swap(input, output);
            }
            // Skipping one constant digit leaves this range in the other array.
            if (input != scratch.data()) {
                for (size_t i = begin; i < finish;) {
                    check();
                    const auto count = std::min(spatialKeyBatchSize, finish - i);
                    std::copy_n(input + i, count, scratch.data() + i); i += count;
                }
            }
        }
    }, 2, maximumHelpers);
    keys.swap(scratch);
    return 0;
}
} // namespace spatial_sort_detail

// Stable ascending order of the low 32 key bits. Equal keys retain input order.
template <typename Key, typename Morton, typename Check>
void stableSpatialRadixSort(std::vector<Key>& keys, const Morton& morton, const Check& check,
    unsigned maximumHelpers = std::numeric_limits<unsigned>::max())
{
    check();
    if (keys.size() < 2) { return; }
    // Small groups do not justify the partition table and per-range histograms.
    if (keys.size() < (size_t{1} << 20)) {
        spatial_sort_detail::byteSort(keys, morton, check, maximumHelpers);
    } else if (const auto different = spatial_sort_detail::partitionedSort(keys, morton, check, maximumHelpers)) {
        // Reuse the first scan to omit constant bytes in concentrated inputs.
        spatial_sort_detail::byteSort(keys, morton, check, maximumHelpers, different);
    }
    check();
}

} // namespace woby
