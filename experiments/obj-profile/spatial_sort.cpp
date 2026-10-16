// Replay captured Morton keys. Input copies and exact output checks are not timed.
#include "spatial_sort.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>

namespace {
using Key = std::pair<uint32_t, uint32_t>;
static_assert(sizeof(Key) == 8);
using Clock = std::chrono::steady_clock;
const auto morton = [](const Key& key) { return key.first; };
const auto check = [] {};

template <unsigned Bits>
void lsd(std::vector<Key>& keys, unsigned helpers)
{
    constexpr size_t buckets = size_t{1} << Bits;
    const size_t chunks = std::min(size_t{woby::analysisWorkerLimit()} + 1,
        size_t{1} + (keys.size() - 1) / woby::spatialKeyBatchSize);
    const auto bounds = [&](size_t chunk) {
        const size_t size = keys.size() / chunks, extra = keys.size() % chunks;
        const size_t begin = chunk * size + std::min(chunk, extra);
        return std::pair{begin, begin + size + (chunk < extra ? 1 : 0)};
    };
    std::vector<Key> scratch(keys.size());
    std::vector<std::array<size_t, buckets>> offsets(chunks);
    for (unsigned shift = 0; shift < 32; shift += Bits) {
        woby::parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
            for (size_t chunk = first; chunk < end; ++chunk) {
                auto& histogram = offsets[chunk]; histogram.fill(0);
                const auto [begin, finish] = bounds(chunk);
                for (size_t i = begin; i < finish; ++i) {
                    ++histogram[(keys[i].first >> shift) & (buckets - 1)];
                }
            }
        }, 2, helpers);
        size_t total = 0;
        for (size_t bucket = 0; bucket < buckets; ++bucket) {
            for (auto& chunk : offsets) {
                const size_t count = chunk[bucket]; chunk[bucket] = total; total += count;
            }
        }
        woby::parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
            for (size_t chunk = first; chunk < end; ++chunk) {
                auto& output = offsets[chunk];
                const auto [begin, finish] = bounds(chunk);
                for (size_t i = begin; i < finish; ++i) {
                    const auto key = keys[i]; scratch[output[(key.first >> shift) & (buckets - 1)]++] = key;
                }
            }
        }, 2, helpers);
        keys.swap(scratch);
    }
}

template <unsigned HighBits, unsigned LowBits>
void partitioned(std::vector<Key>& keys, unsigned helpers)
{
    constexpr unsigned highShift = 32 - HighBits;
    constexpr size_t highBuckets = size_t{1} << HighBits;
    constexpr size_t lowBuckets = size_t{1} << LowBits;
    constexpr unsigned lowPasses = (highShift + LowBits - 1) / LowBits;
    const size_t chunks = std::min(size_t{woby::analysisWorkerLimit()} + 1,
        size_t{1} + (keys.size() - 1) / woby::spatialKeyBatchSize);
    const auto bounds = [&](size_t chunk) {
        const size_t size = keys.size() / chunks, extra = keys.size() % chunks;
        const size_t begin = chunk * size + std::min(chunk, extra);
        return std::pair{begin, begin + size + (chunk < extra ? 1 : 0)};
    };
    std::vector<Key> scratch(keys.size());
    std::vector<std::array<size_t, highBuckets>> offsets(chunks);
    woby::parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
        for (size_t chunk = first; chunk < end; ++chunk) {
            auto& histogram = offsets[chunk]; histogram.fill(0);
            const auto [begin, finish] = bounds(chunk);
            for (size_t i = begin; i < finish; ++i) { ++histogram[keys[i].first >> highShift]; }
        }
    }, 2, helpers);
    std::vector<size_t> starts(highBuckets + 1);
    size_t total = 0;
    for (size_t bucket = 0; bucket < highBuckets; ++bucket) {
        starts[bucket] = total;
        for (auto& chunk : offsets) {
            const size_t count = chunk[bucket]; chunk[bucket] = total; total += count;
        }
    }
    starts[highBuckets] = total;
    woby::parallelWorkBatches(chunks, 1, check, [&](size_t first, size_t end) {
        for (size_t chunk = first; chunk < end; ++chunk) {
            auto& output = offsets[chunk];
            const auto [begin, finish] = bounds(chunk);
            for (size_t i = begin; i < finish; ++i) {
                const auto key = keys[i]; scratch[output[key.first >> highShift]++] = key;
            }
        }
    }, 2, helpers);
    std::vector<size_t> order(highBuckets);
    std::iota(order.begin(), order.end(), size_t{0});
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return starts[a + 1] - starts[a] > starts[b + 1] - starts[b]; });
    woby::parallelWorkBatches(highBuckets, 1, check, [&](size_t first, size_t end) {
        for (size_t task = first; task < end; ++task) {
            const size_t bucket = order[task], begin = starts[bucket], finish = starts[bucket + 1];
            if (begin == finish) { continue; }
            std::array<std::array<size_t, lowBuckets>, lowPasses> histograms{};
            for (size_t i = begin; i < finish; ++i) {
                const auto key = scratch[i].first;
                for (unsigned pass = 0; pass < lowPasses; ++pass) {
                    ++histograms[pass][(key >> (pass * LowBits)) & (lowBuckets - 1)];
                }
            }
            auto* input = scratch.data(); auto* output = keys.data();
            for (unsigned pass = 0; pass < lowPasses; ++pass) {
                auto& histogram = histograms[pass];
                if (*std::max_element(histogram.begin(), histogram.end()) == finish - begin) { continue; }
                size_t prefix = begin;
                for (auto& value : histogram) { const auto count = value; value = prefix; prefix += count; }
                for (size_t i = begin; i < finish; ++i) {
                    const auto key = input[i]; output[histogram[(key.first >> (pass * LowBits)) & (lowBuckets - 1)]++] = key;
                }
                std::swap(input, output);
            }
            auto* final = lowPasses % 2 == 0 ? scratch.data() : keys.data();
            if (input != final) { std::copy(input + begin, input + finish, final + begin); }
        }
    }, 2, helpers);
    if constexpr (lowPasses % 2 == 0) { keys.swap(scratch); }
}
}

int main(int argc, char** argv)
{
    try {
        if (argc != 4 && argc != 5) { throw std::invalid_argument("Expected KEYS HELPERS ROUNDS [current-byte]"); }
        const bool productionOnly = argc == 5;
        if (productionOnly && std::string_view(argv[4]) != "current-byte") { throw std::invalid_argument("Unknown variant set"); }
        const std::filesystem::path path(argv[1]);
        const auto helpers = static_cast<unsigned>(std::stoul(argv[2]));
        const auto rounds = std::stoul(argv[3]);
        if (!rounds || rounds > 20 || std::filesystem::file_size(path) % sizeof(Key)) { throw std::invalid_argument("Invalid input"); }
        std::vector<Key> original(std::filesystem::file_size(path) / sizeof(Key));
        if (original.empty()) { throw std::invalid_argument("Empty input"); }
        std::ifstream stream(path, std::ios::binary); stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size() * sizeof(Key)));
        auto expected = original;
        woby::stableSpatialRadixSort(expected, morton, check, helpers);
        struct Variant { const char* name; void (*sort)(std::vector<Key>&, unsigned); };
        const std::array variants{
            Variant{"current", [](auto& keys, auto cap) { woby::stableSpatialRadixSort(keys, morton, check, cap); }},
            Variant{"byte", lsd<8>},
            Variant{"lsd11", lsd<11>}, Variant{"partition8x8", partitioned<8, 8>},
            Variant{"partition10x8", partitioned<10, 8>}, Variant{"partition11x8", partitioned<11, 8>},
            Variant{"partition12x10", partitioned<12, 10>}, Variant{"partition12x8", partitioned<12, 8>},
            Variant{"partition10x11", partitioned<10, 11>}, Variant{"partition11x11", partitioned<11, 11>},
            Variant{"partition13x10", partitioned<13, 10>}, Variant{"partition14x9", partitioned<14, 9>},
            Variant{"partition16x8", partitioned<16, 8>}};
        const size_t variantCount = productionOnly ? 2 : variants.size();
        for (size_t round = 0; round < rounds; ++round) {
            for (size_t v = 0; v < variantCount; ++v) {
                const auto& variant = variants[(v + round) % variantCount];
                auto keys = original;
                const auto start = Clock::now(); variant.sort(keys, helpers);
                const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                if (keys != expected) { throw std::runtime_error(std::string("Output mismatch: ") + variant.name); }
                std::cout << nlohmann::json{{"variant", variant.name}, {"round", round + 1},
                    {"helpers", helpers}, {"count", keys.size()}, {"wall_ms", elapsed}}.dump() << std::endl;
            }
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
