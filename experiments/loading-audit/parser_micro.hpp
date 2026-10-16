#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>
#include <nlohmann/json.hpp>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace parser_micro {
using Clock = std::chrono::steady_clock;
struct Growth {
    uint64_t count = 0, allocated = 0, copied = 0, largest = 0;
};
struct AtomicGrowth {
    std::atomic<uint64_t> count = 0, allocated = 0, copied = 0, largest = 0;
};
inline std::array<AtomicGrowth, 6> global{};
inline thread_local std::array<Growth, 6> local{};
inline std::mutex range_mutex;
inline std::vector<nlohmann::json> ranges;
inline constexpr std::array<const char*, 6> labels{
    "position-double-tuples", "index-triplets", "double-attributes", "enum-bytes", "byte-buffers(flags-and-face-counts)", "other"};

template <typename T> constexpr size_t kind() {
    if constexpr (sizeof(T) == 24) { return 0; }
    else if constexpr (sizeof(T) == 12) { return 1; }
    else if constexpr (std::is_same_v<T, double>) { return 2; }
    else if constexpr (std::is_enum_v<T> && sizeof(T) == 1) { return 3; }
    else if constexpr (sizeof(T) == 1) { return 4; }
    else { return 5; }
}
template <typename T> void growth(size_t old_size, size_t capacity, bool copied) {
    const auto allocated = static_cast<uint64_t>(capacity * sizeof(T));
    const auto payload = copied ? static_cast<uint64_t>(old_size * sizeof(T)) : 0;
    auto& item = global[kind<T>()];
    item.count.fetch_add(1, std::memory_order_relaxed);
    item.allocated.fetch_add(allocated, std::memory_order_relaxed);
    item.copied.fetch_add(payload, std::memory_order_relaxed);
    auto largest = item.largest.load(std::memory_order_relaxed);
    while (largest < allocated && !item.largest.compare_exchange_weak(largest, allocated, std::memory_order_relaxed)) {}
    auto& owned = local[kind<T>()];
    ++owned.count; owned.allocated += allocated; owned.copied += payload;
    owned.largest = (std::max)(owned.largest, allocated);
}
inline uint64_t cpu_ticks() {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) { return 0; }
    const auto ticks = [](FILETIME value) { return (uint64_t(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
    return ticks(kernel) + ticks(user);
}
inline void range_record(size_t begin, size_t size, Clock::time_point start, uint64_t cpu_start,
    double io_ms, size_t positions, size_t texcoords, size_t faces, size_t indices) {
    nlohmann::json values = nlohmann::json::array();
    uint64_t copies = 0, allocated = 0;
    for (size_t i = 0; i < local.size(); ++i) {
        const auto& row = local[i]; copies += row.copied; allocated += row.allocated;
        values.push_back({{"kind", labels[i]}, {"growths", row.count}, {"allocated_bytes", row.allocated},
                         {"copy_payload_bytes", row.copied}, {"largest_allocation_bytes", row.largest}});
    }
    const auto wall = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const auto cpu_ms = static_cast<double>(cpu_ticks() - cpu_start) / 10000.0;
    std::lock_guard lock(range_mutex);
    ranges.push_back({{"byte_begin", begin}, {"input_bytes", size}, {"thread_id", GetCurrentThreadId()},
        {"wall_ms", wall}, {"cpu_ms", cpu_ms}, {"io_submit_wait_ms", io_ms}, {"positions", positions},
        {"texcoords", texcoords}, {"faces", faces}, {"corners", indices},
        {"copy_payload_bytes", copies}, {"cumulative_allocated_bytes", allocated}, {"buffers", std::move(values)}});
}
inline nlohmann::json snapshot() {
    nlohmann::json buffers = nlohmann::json::array();
    for (size_t i = 0; i < global.size(); ++i) {
        const auto& row = global[i];
        buffers.push_back({{"kind", labels[i]}, {"growths", row.count.load()}, {"allocated_bytes", row.allocated.load()},
            {"copy_payload_bytes", row.copied.load()}, {"largest_allocation_bytes", row.largest.load()}});
    }
    std::lock_guard lock(range_mutex);
    return {{"buffers", std::move(buffers)}, {"ranges", ranges},
        {"scope", "rapidobj growable Buffer allocations and growth copies only; excludes output vectors, native read buffers, Windows file cache, and other allocator traffic"}};
}
} // namespace parser_micro
