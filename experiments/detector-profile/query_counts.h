#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>

namespace detector_profile {
// Research counters only. Each worker accumulates locally, then publishes once
// per production-sized batch. These timings are not baseline measurements.
inline size_t queryStride = 1;
struct QueryCounts {
    uint64_t queries = 0, visits = 0, nodes = 0, triangles = 0;
    uint64_t zeroHints = 0, updates = 0, maxVisits = 0;
    std::array<uint64_t,32> visitHistogram{}, triangleHistogram{};
};
inline thread_local QueryCounts queryLocal;
inline std::array<std::atomic<uint64_t>,7> queryTotals{};
inline std::array<std::atomic<uint64_t>,32> visitTotals{}, triangleTotals{};
inline void recordQuery(uint64_t visits,uint64_t triangles) {
    queryLocal.visits += visits;
    queryLocal.maxVisits = std::max(queryLocal.maxVisits,visits);
    ++queryLocal.visitHistogram[std::min(size_t{31},static_cast<size_t>(std::bit_width(visits)))];
    ++queryLocal.triangleHistogram[std::min(size_t{31},static_cast<size_t>(std::bit_width(triangles)))];
}
inline void resetQueries() {
    for (auto& value : queryTotals) { value.store(0,std::memory_order_relaxed); }
    for (auto& value : visitTotals) { value.store(0,std::memory_order_relaxed); }
    for (auto& value : triangleTotals) { value.store(0,std::memory_order_relaxed); }
}
inline void mergeQueries() {
    const std::array values{queryLocal.queries,queryLocal.visits,queryLocal.nodes,queryLocal.triangles,
        queryLocal.zeroHints,queryLocal.updates};
    for (size_t i = 0; i < values.size(); ++i) { queryTotals[i].fetch_add(values[i],std::memory_order_relaxed); }
    auto maximum = queryTotals[6].load(std::memory_order_relaxed);
    while (maximum < queryLocal.maxVisits && !queryTotals[6].compare_exchange_weak(maximum,queryLocal.maxVisits,std::memory_order_relaxed)) {}
    for (size_t i = 0; i < visitTotals.size(); ++i) {
        visitTotals[i].fetch_add(queryLocal.visitHistogram[i],std::memory_order_relaxed);
        triangleTotals[i].fetch_add(queryLocal.triangleHistogram[i],std::memory_order_relaxed);
    }
}
inline nlohmann::json queryData() {
    nlohmann::json result{{"batch_stride",queryStride}};
    constexpr std::array names{"queries","stack_visits","internal_nodes","triangle_tests","zero_distance_hints","best_updates","maximum_stack_visits"};
    for (size_t i = 0; i < names.size(); ++i) { result[names[i]] = queryTotals[i].load(std::memory_order_relaxed); }
    result["histogram_buckets"] = "bucket 0: zero; bucket k: [2^(k-1), 2^k-1]; bucket 31 includes all higher values";
    result["visit_histogram"] = nlohmann::json::array();
    result["triangle_histogram"] = nlohmann::json::array();
    for (size_t i = 0; i < visitTotals.size(); ++i) {
        result["visit_histogram"].push_back(visitTotals[i].load(std::memory_order_relaxed));
        result["triangle_histogram"].push_back(triangleTotals[i].load(std::memory_order_relaxed));
    }
    return result;
}
} // namespace detector_profile
