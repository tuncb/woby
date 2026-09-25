#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace woby {

// Contiguous, insertion-ordered keys for analysis. Only bucket references are
// rehashed, so source IDs and the order of findings never depend on hash order.
template <typename T, size_t N>
struct AnalysisIndex {
    std::vector<std::array<T, N>> keys;
    std::vector<size_t> buckets;
};

template <typename T, size_t N>
size_t analysisKeyHash(const std::array<T, N>& key)
{
    uint64_t hash = 0;
    for (const auto value : key) {
        uint64_t bits;
        if constexpr (std::is_floating_point_v<T>) {
            // Equality treats signed zeros alike; all callers validate finiteness.
            bits = std::bit_cast<uint64_t>(static_cast<double>(value == 0 ? 0 : value));
        } else { bits = static_cast<uint64_t>(value); }
        bits ^= bits >> 30; bits *= 0xbf58476d1ce4e5b9ull;
        bits ^= bits >> 27; bits *= 0x94d049bb133111ebull;
        hash ^= (bits ^ (bits >> 31)) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    }
    return static_cast<size_t>(hash);
}

template <typename T, size_t N>
void reserveAnalysisIndex(AnalysisIndex<T, N>& index, size_t capacity)
{
    if (capacity > (std::numeric_limits<size_t>::max() >> 2)) {
        throw std::length_error("Analysis index exceeds supported size.");
    }
    index.keys.reserve(capacity);
    index.buckets.assign(std::bit_ceil(std::max(size_t{16}, capacity + capacity / 2)),
        std::numeric_limits<size_t>::max());
    const size_t mask = index.buckets.size() - 1;
    for (size_t i = 0; i < index.keys.size(); ++i) {
        size_t bucket = analysisKeyHash(index.keys[i]) & mask;
        while (index.buckets[bucket] != std::numeric_limits<size_t>::max()) { bucket = (bucket + 1) & mask; }
        index.buckets[bucket] = i;
    }
}

template <typename T, size_t N>
size_t analysisIndex(AnalysisIndex<T, N>& index, const std::array<T, N>& key)
{
    if (index.buckets.empty()) { reserveAnalysisIndex(index, 16); }
    size_t mask = index.buckets.size() - 1;
    size_t bucket = analysisKeyHash(key) & mask;
    while (index.buckets[bucket] != std::numeric_limits<size_t>::max()) {
        const auto id = index.buckets[bucket];
        if (index.keys[id] == key) { return id; }
        bucket = (bucket + 1) & mask;
    }
    if (index.keys.size() >= index.buckets.size() * 3 / 4) {
        reserveAnalysisIndex(index, index.buckets.size());
        mask = index.buckets.size() - 1;
        bucket = analysisKeyHash(key) & mask;
        while (index.buckets[bucket] != std::numeric_limits<size_t>::max()) { bucket = (bucket + 1) & mask; }
    }
    const size_t id = index.keys.size();
    index.keys.push_back(key);
    index.buckets[bucket] = id;
    return id;
}

} // namespace woby
