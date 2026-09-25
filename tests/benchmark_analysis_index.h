#pragma once

// Benchmark-only adapter: preserve the production hash, stable first-use IDs,
// and key-vector interface while substituting the lookup table.
#include "analysis_index.h"
#include <ankerl/unordered_dense.h>
#include <boost/unordered/unordered_flat_map.hpp>

namespace woby {
template <typename T, size_t N>
struct BenchmarkKeyHash {
    size_t operator()(const std::array<T, N>& key) const { return analysisKeyHash(key); }
};

template <typename T, size_t N>
struct BenchmarkAnalysisIndex {
    std::vector<std::array<T, N>> keys;
#if defined(WOBY_BENCHMARK_boost)
    boost::unordered_flat_map<std::array<T, N>, size_t, BenchmarkKeyHash<T, N>> lookup;
#else
    ankerl::unordered_dense::map<std::array<T, N>, size_t, BenchmarkKeyHash<T, N>> lookup;
#endif
};

template <typename T, size_t N>
void reserveAnalysisIndex(BenchmarkAnalysisIndex<T, N>& index, size_t capacity)
{
    index.keys.reserve(capacity);
    index.lookup.reserve(capacity);
}

template <typename T, size_t N>
size_t analysisIndex(BenchmarkAnalysisIndex<T, N>& index, const std::array<T, N>& key)
{
    const auto [entry, inserted] = index.lookup.try_emplace(key, index.keys.size());
    if (inserted) { index.keys.push_back(key); }
    return entry->second;
}
}

// Only the generated .cpp files include this adapter. The original definition
// remains available above for its shared hash and is never used by the probes.
#define AnalysisIndex BenchmarkAnalysisIndex
