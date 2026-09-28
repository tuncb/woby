#pragma once
#include "mapping_probe.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace mapping_probe {
struct Growth {
    uint64_t count = 0, copied_bytes = 0;
    double ms = 0;
};
struct Counters {
    uint64_t direct_new = 0, direct_hit = 0, primary_new = 0, primary_hit = 0;
    uint64_t secondary_new = 0, secondary_hit = 0, secondary_lookups = 0;
    uint64_t hash_calls = 0, lookup_slots = 0, max_lookup_slots = 0;
    uint64_t rehashes = 0, rehashed_entries = 0, rehash_slots = 0, growth_placement_slots = 0;
    // Exact lengths 1..8, then 9..16, 17..32, 33..64, 65+.
    std::array<uint64_t, 12> probe_histogram{};
    double rehash_ms = 0;
    Growth key_growth, vertex_growth;
};
inline Counters counters;
inline void recordProbes(uint64_t slots) {
    counters.lookup_slots += slots;
    counters.max_lookup_slots = std::max(counters.max_lookup_slots, slots);
    const size_t bin = slots <= 8 ? static_cast<size_t>(slots - 1)
        : slots <= 16 ? 8 : slots <= 32 ? 9 : slots <= 64 ? 10 : 11;
    ++counters.probe_histogram[bin];
}
template<typename T> void trackedPush(std::vector<T>& values, const T& value, Growth& growth) {
    if (values.size() != values.capacity()) { values.push_back(value); return; }
    const auto begin = Clock::now();
    ++growth.count;
    growth.copied_bytes += values.size() * sizeof(T);
    values.push_back(value);
    growth.ms += elapsed(begin);
}
inline Json growthJson(const Growth& growth) {
    return {{"count", growth.count}, {"copied_bytes", growth.copied_bytes}, {"ms", growth.ms}};
}
inline Json diagnosticJson() {
    const auto& c = counters;
    return {{"direct_new", c.direct_new}, {"direct_hit", c.direct_hit},
        {"primary_new", c.primary_new}, {"primary_hit", c.primary_hit},
        {"secondary_new", c.secondary_new}, {"secondary_hit", c.secondary_hit},
        {"secondary_lookups", c.secondary_lookups}, {"hash_calls", c.hash_calls},
        {"lookup_slots", c.lookup_slots}, {"max_lookup_slots", c.max_lookup_slots},
        {"probe_histogram", c.probe_histogram}, {"rehashes", c.rehashes},
        {"rehashed_entries", c.rehashed_entries}, {"rehash_slots", c.rehash_slots},
        {"growth_placement_slots", c.growth_placement_slots}, {"rehash_ms", c.rehash_ms},
        {"key_growth", growthJson(c.key_growth)}, {"vertex_growth", growthJson(c.vertex_growth)}};
}
}
