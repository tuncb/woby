// Standalone representation probe. It does not change Woby's position access.
// Build from an x64 MSVC tools prompt with /O2 /std:c++20 /EHsc /W4 /WX /fp:precise.
#include "../src/model_mesh.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Point = woby::Coordinate;
using Clock = std::chrono::steady_clock;
static_assert(sizeof(Point) == 24 && sizeof(uint32_t) == 4 && sizeof(woby::Vertex) == 32);
volatile double observable = 0;

struct Fixture {
    std::vector<Point> source, expanded;
    std::vector<uint32_t> mapping, queries;
    bool identity = false;
};

Fixture fixture(size_t count, std::string_view kind)
{
    Fixture f;
    f.identity = kind == "identity";
    const size_t sources = f.identity ? count : kind == "unused" ? count * 4 : count / 4;
    f.source.reserve(sources);
    for (size_t i = 0; i < sources; ++i) {
        // Binary fractions make the ordered sums exact at these input sizes.
        f.source.push_back({double(i % 8192) * .125, double(i % 4096) * .25, double(i % 2048) * .5});
    }
    f.source[0] = {-0.0, 0.0, -0.0};
    f.source[1] = f.source[2]; // Equal coordinates must still have different IDs.
    f.mapping.resize(count);
    for (size_t i = 0; i < count; ++i) {
        f.mapping[i] = static_cast<uint32_t>(f.identity ? i : kind == "unused" ? i * 4 : i / 4);
    }
    std::mt19937 random(1729);
    if (kind == "seams_shuffled" || kind == "unused") { std::shuffle(f.mapping.begin(), f.mapping.end(), random); }
    f.expanded.reserve(count);
    for (auto id : f.mapping) { f.expanded.push_back(f.source.at(id)); }
    f.queries.resize(count);
    std::iota(f.queries.begin(), f.queries.end(), uint32_t{0});
    std::shuffle(f.queries.begin(), f.queries.end(), random);
    for (size_t i = 0; i < count; ++i) {
        const auto& candidate = f.source.at(f.identity ? i : f.mapping.at(i));
        for (size_t axis = 0; axis < 3; ++axis) {
            if (std::bit_cast<uint64_t>(candidate[axis]) != std::bit_cast<uint64_t>(f.expanded[i][axis])) {
                throw std::runtime_error("Position bits differ");
            }
        }
    }
    return f;
}

// Measure the same bounds-checked coordinate loads and arithmetic in each mode.
// Template specialization removes representation dispatch from the measured loop.
template<bool Mapped, bool Random>
double scan(const Fixture& f)
{
    Point sum{};
    for (size_t i = 0; i < f.expanded.size(); ++i) {
        const size_t render = Random ? f.queries[i] : i;
        const auto& p = Mapped ? f.source.at(f.mapping.at(render)) : f.expanded.at(render);
        for (size_t axis = 0; axis < 3; ++axis) { sum[axis] += p[axis]; }
    }
    return sum[0] + sum[1] + sum[2];
}

template<bool Random>
double identityScan(const Fixture& f)
{
    Point sum{};
    for (size_t i = 0; i < f.expanded.size(); ++i) {
        const auto& p = f.source.at(Random ? f.queries[i] : i);
        for (size_t axis = 0; axis < 3; ++axis) { sum[axis] += p[axis]; }
    }
    return sum[0] + sum[1] + sum[2];
}

double read(const Fixture& f, bool candidate, bool random)
{
    if (!candidate) { return random ? scan<false, true>(f) : scan<false, false>(f); }
    if (f.identity) { return random ? identityScan<true>(f) : identityScan<false>(f); }
    return random ? scan<true, true>(f) : scan<true, false>(f);
}

void measure(const Fixture& f, std::string_view kind, bool random, int run)
{
    std::array<std::vector<double>, 2> samples;
    const auto expected = read(f, false, random);
    // One warm-up pair followed by nine pairs; reverse execution order each pair.
    for (int repeat = -1; repeat < 9; ++repeat) {
        for (int order = 0; order < 2; ++order) {
            const int variant = (order + repeat + run + 2) % 2;
            const auto start = Clock::now();
            const auto sum = read(f, variant != 0, random);
            const auto end = Clock::now();
            observable = sum;
            if (sum != expected) { throw std::runtime_error("Read checksum differs"); }
            if (repeat >= 0) { samples[variant].push_back(std::chrono::duration<double, std::milli>(end - start).count()); }
        }
    }
    for (size_t variant = 0; variant < 2; ++variant) {
        auto& times = samples[variant];
        std::sort(times.begin(), times.end());
        const size_t payload = variant == 0 ? f.expanded.size() * sizeof(Point)
            : f.identity ? 0 : f.mapping.size() * sizeof(uint32_t);
        std::cout << run << ',' << kind << ',' << f.expanded.size() << ',' << f.source.size()
            << ',' << (random ? "random" : "sequential") << ','
            << (variant == 0 ? "copied" : f.identity ? "identity" : "mapped") << ','
            << times.front() << ',' << times[4] << ',' << times.back() << ',' << payload
            << ',' << f.source.size() * sizeof(Point) << ',' << expected << '\n';
    }
}
}

int main(int argc, char** argv)
{
    try {
        const int run = argc > 1 ? std::stoi(argv[1]) : 1;
        if (run < 1 || run > 100) { throw std::runtime_error("Run number must be from 1 to 100"); }
        std::cout.precision(12);
        std::cout << "run,case,render_vertices,source_points,access,variant,min_ms,median_ms,max_ms,extra_position_bytes,source_position_bytes,checksum\n";
        for (const size_t count : {size_t{1000000}, size_t{4000000}}) {
            for (const auto kind : {"identity", "seams_local", "seams_shuffled", "unused"}) {
                if (count > 1000000 && std::string_view(kind) == "unused") { continue; }
                const auto f = fixture(count, kind);
                measure(f, kind, false, run);
                measure(f, kind, true, run);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
