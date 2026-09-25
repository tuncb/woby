#include "analysis_index.h"

#include <ankerl/unordered_dense.h>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
using Clock = std::chrono::steady_clock;
using Id = uint64_t;

// Same build/duplicate-insert/hit/miss lifecycle as transient scene tables.
// Destruction is timed too. Batch small tables to avoid timer quantization.
template <typename Work>
void measure(const std::string& workload, const std::string& container, size_t size, Work work)
{
    const auto expected = work();
    size_t batch = 1;
    for (;;) {
        const auto start = Clock::now();
        for (size_t i = 0; i < batch; ++i) {
            if (work() != expected) { throw std::runtime_error("Non-deterministic benchmark result."); }
        }
        if (Clock::now() - start >= std::chrono::milliseconds(8) || batch >= 4096) { break; }
        batch *= 2;
    }
    std::vector<double> samples;
    for (size_t run = 0; run < 7; ++run) {
        const auto start = Clock::now();
        for (size_t i = 0; i < batch; ++i) {
            if (work() != expected) { throw std::runtime_error("Non-deterministic benchmark result."); }
        }
        samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count()
            / static_cast<double>(batch));
    }
    std::sort(samples.begin(), samples.end());
    std::cout << workload << ',' << container << ',' << size << ',' << samples.front() << ','
        << samples[3] << ',' << samples.back() << ',' << expected << std::endl;
}

template <typename Set>
uint64_t idSet(const std::vector<Id>& ids, bool reserve)
{
    Set seen;
    if (reserve) { seen.reserve(ids.size()); }
    uint64_t result = 0;
    for (const auto id : ids) { result += seen.insert(id).second; }
    for (const auto id : ids) { result += seen.insert(id).second; }
    for (const auto id : ids) { result += seen.contains(id) + seen.contains(id + (Id{1} << 48)); }
    return result;
}

template <typename Map>
uint64_t idMap(const std::vector<Id>& ids)
{
    Map lookup;
    lookup.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        lookup.emplace(ids[i], std::pair{static_cast<int>(i / 16), static_cast<int>(i % 16)});
    }
    uint64_t result = 0;
    for (const auto id : ids) {
        const auto found = lookup.find(id);
        result += static_cast<uint64_t>(found->second.first + found->second.second);
        result += lookup.contains(id + (Id{1} << 48));
    }
    return result;
}

template <typename T, size_t N>
size_t findIndex(const woby::AnalysisIndex<T, N>& index, const std::array<T, N>& key)
{
    const auto missing = std::numeric_limits<size_t>::max();
    const size_t mask = index.buckets.size() - 1;
    size_t bucket = woby::analysisKeyHash(key) & mask;
    while (index.buckets[bucket] != missing) {
        if (index.keys[index.buckets[bucket]] == key) { return index.buckets[bucket]; }
        bucket = (bucket + 1) & mask;
    }
    return missing;
}

uint64_t customSet(const std::vector<Id>& ids)
{
    woby::AnalysisIndex<Id, 1> seen;
    woby::reserveAnalysisIndex(seen, ids.size());
    for (const auto id : ids) { (void)woby::analysisIndex(seen, std::array{id}); }
    uint64_t result = seen.keys.size();
    for (const auto id : ids) { result += woby::analysisIndex(seen, std::array{id}) >= ids.size(); }
    for (const auto id : ids) {
        result += findIndex(seen, std::array{id}) != std::numeric_limits<size_t>::max();
        result += findIndex(seen, std::array{id + (Id{1} << 48)}) != std::numeric_limits<size_t>::max();
    }
    return result;
}

uint64_t customMap(const std::vector<Id>& ids)
{
    woby::AnalysisIndex<Id, 1> index;
    woby::reserveAnalysisIndex(index, ids.size());
    std::vector<std::pair<int, int>> values;
    values.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
        if (woby::analysisIndex(index, std::array{ids[i]}) == values.size()) {
            values.emplace_back(static_cast<int>(i / 16), static_cast<int>(i % 16));
        }
    }
    uint64_t result = 0;
    for (const auto id : ids) {
        const auto& value = values[findIndex(index, std::array{id})];
        result += static_cast<uint64_t>(value.first + value.second);
        result += findIndex(index, std::array{id + (Id{1} << 48)}) != std::numeric_limits<size_t>::max();
    }
    return result;
}

using Point = std::array<double, 3>;
struct PointHash {
    size_t operator()(const Point& point) const { return woby::analysisKeyHash(point); }
};

template <typename Map>
uint64_t points(const std::vector<Point>& input)
{
    Map index;
    index.reserve(input.size());
    uint64_t checksum = 0;
    // Same insertion-order IDs required by analysisIndex, including repeated hits.
    for (size_t pass = 0; pass < 4; ++pass) {
        for (const auto& point : input) { checksum += index.try_emplace(point, index.size()).first->second; }
    }
    return checksum;
}

template <typename Set>
uint64_t partSet(const std::vector<Id>& ids)
{
    Set seen;
    if constexpr (requires { seen.reserve(ids.size()); }) { seen.reserve(ids.size()); }
    uint64_t result = 0;
    for (size_t pass = 0; pass < 2; ++pass) {
        for (const auto id : ids) { result += seen.insert(std::array<size_t, 2>{id / 16, id % 16}).second; }
    }
    return result;
}
struct PartHash {
    size_t operator()(const std::array<size_t, 2>& key) const { return woby::analysisKeyHash(key); }
};
}

int main()
{
    try {
        std::cout << "workload,container,size,min_us,median_us,max_us,checksum\n";
        std::mt19937_64 random(123456);
        for (const size_t size : {16u, 256u, 4096u, 65536u}) {
            std::vector<Id> ids(size);
            std::iota(ids.begin(), ids.end(), Id{1});
            std::shuffle(ids.begin(), ids.end(), random);
            measure("id_set", "std", size, [&] { return idSet<std::unordered_set<Id>>(ids, false); });
            measure("id_set", "std_reserved", size, [&] { return idSet<std::unordered_set<Id>>(ids, true); });
            measure("id_set", "woby", size, [&] { return customSet(ids); });
            measure("id_set", "boost", size, [&] { return idSet<boost::unordered_flat_set<Id>>(ids, true); });
            measure("id_set", "ankerl", size, [&] { return idSet<ankerl::unordered_dense::set<Id>>(ids, true); });
            using Value = std::pair<int, int>;
            measure("id_map", "std", size, [&] { return idMap<std::unordered_map<Id, Value>>(ids); });
            measure("id_map", "woby", size, [&] { return customMap(ids); });
            measure("id_map", "boost", size, [&] { return idMap<boost::unordered_flat_map<Id, Value>>(ids); });
            measure("id_map", "ankerl", size, [&] { return idMap<ankerl::unordered_dense::map<Id, Value>>(ids); });
            using Part = std::array<size_t, 2>;
            measure("part_set", "std_tree", size, [&] { return partSet<std::set<Part>>(ids); });
            measure("part_set", "boost", size, [&] { return partSet<boost::unordered_flat_set<Part, PartHash>>(ids); });
            measure("part_set", "ankerl", size, [&] { return partSet<ankerl::unordered_dense::set<Part, PartHash>>(ids); });
            measure("part_set", "woby", size, [&] {
                woby::AnalysisIndex<size_t, 2> index;
                woby::reserveAnalysisIndex(index, ids.size());
                uint64_t result = 0;
                for (size_t pass = 0; pass < 2; ++pass) {
                    for (const auto id : ids) {
                        const auto count = index.keys.size();
                        result += woby::analysisIndex(index, Part{id / 16, id % 16}) == count;
                    }
                }
                return result;
            });
        }
        for (const size_t size : {32768u, 524288u}) {
            std::vector<Point> input;
            input.reserve(size);
            for (size_t i = 0; i < size; ++i) {
                input.push_back({static_cast<double>(i % 1024), static_cast<double>(i / 1024),
                    static_cast<double>(i % 7) * .125});
            }
            std::shuffle(input.begin(), input.end(), random);
            measure("point_index", "std", size, [&] { return points<std::unordered_map<Point, size_t, PointHash>>(input); });
            measure("point_index", "boost", size, [&] { return points<boost::unordered_flat_map<Point, size_t, PointHash>>(input); });
            measure("point_index", "ankerl", size, [&] { return points<ankerl::unordered_dense::map<Point, size_t, PointHash>>(input); });
            measure("point_index", "woby", size, [&] {
                woby::AnalysisIndex<double, 3> index;
                woby::reserveAnalysisIndex(index, size);
                uint64_t checksum = 0;
                for (size_t pass = 0; pass < 4; ++pass) {
                    for (const auto& point : input) { checksum += woby::analysisIndex(index, point); }
                }
                return checksum;
            });
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
