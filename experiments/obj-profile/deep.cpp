#include "profile.h"
#include "model_mesh.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <numeric>
#include <span>
#include <thread>
#include <immintrin.h>
#include <intrin.h>

namespace obj_profile {
namespace {
using Box = std::array<woby::Coordinate, 2>;
Box emptyBox() {
    return {{{INFINITY, INFINITY, INFINITY}, {-INFINITY, -INFINITY, -INFINITY}}};
}
void expand(Box& box, const woby::Coordinate& p) {
    for (size_t axis = 0; axis < 3; ++axis) {
        box[0][axis] = std::min(box[0][axis], p[axis]);
        box[1][axis] = std::max(box[1][axis], p[axis]);
    }
}
template<int Mode> Box boundsRange(const woby::Mesh& mesh, const std::vector<woby::Coordinate>& packed,
    size_t begin, size_t end, size_t prefetch) {
    const auto& g = woby::meshGeometry(mesh);
    const auto& source = *g.sourcePositions;
    const auto& indices = mesh.sourceData->indices;
    const bool identity = g.positionIndexing == woby::PositionIndexing::identity;
    Box out = emptyBox();
    for (size_t i = begin; i < end; ++i) {
        if constexpr (Mode == 0) {
            expand(out, woby::originalPosition(woby::meshPosition(mesh, g.indices[i]), mesh.origin));
        } else if constexpr (Mode == 1) {
            const auto render = g.indices[i];
            const auto p = source[identity ? render : g.positionIndices[render]];
            expand(out, {p[0] + mesh.origin[0], p[1] + mesh.origin[1], p[2] + mesh.origin[2]});
        } else if constexpr (Mode == 2) {
            if (prefetch && i + prefetch < end) {
                _mm_prefetch(reinterpret_cast<const char*>(&source[indices[i + prefetch]]), _MM_HINT_T0);
            }
            const auto& p = source[indices[i]];
            expand(out, {p[0] + mesh.origin[0], p[1] + mesh.origin[1], p[2] + mesh.origin[2]});
        } else if constexpr (Mode == 3) {
            expand(out, packed[g.indices[i]]);
        } else {
            expand(out, packed[i]);
        }
    }
    return out;
}
template<int Mode> Box bounds(const woby::Mesh& mesh, const std::vector<woby::Coordinate>& packed,
    size_t workers, size_t prefetch = 0) {
    const auto count = Mode == 4 ? packed.size() : woby::meshGeometry(mesh).indices.size();
    if (workers == 1) { return boundsRange<Mode>(mesh, packed, 0, count, prefetch); }
    std::vector<std::future<Box>> jobs;
    for (size_t i = 0; i < workers; ++i) {
        jobs.push_back(std::async(std::launch::async, [&, i] {
            return boundsRange<Mode>(mesh, packed, count * i / workers, count * (i + 1) / workers, prefetch);
        }));
    }
    auto result = emptyBox();
    for (auto& job : jobs) {
        const auto part = job.get();
        if (part[0][0] <= part[1][0]) { expand(result, part[0]); expand(result, part[1]); }
    }
    return result;
}
Json locality(const std::vector<uint32_t>& indices, size_t stride) {
    uint64_t same = 0, sameLine = 0, samePage = 0, increasing = 0, within16 = 0, far = 0;
    for (size_t i = 1; i < indices.size(); ++i) {
        const uint64_t a = indices[i - 1], b = indices[i];
        const auto distance = a > b ? a - b : b - a;
        same += a == b; sameLine += a * stride / 64 == b * stride / 64;
        samePage += a * stride / 4096 == b * stride / 4096;
        increasing += b > a; within16 += distance <= 16; far += distance * stride >= 1024 * 1024;
    }
    return {{"comparisons", indices.empty() ? 0 : indices.size() - 1}, {"stride", stride}, {"same_id", same},
        {"same_start_cache_line", sameLine}, {"same_start_4KiB_page", samePage}, {"increasing", increasing},
        {"within_16_ids", within16}, {"at_least_1MiB_apart", far}};
}
// Synthetic access controls reuse actual source coordinates but choose an index pattern.
// They measure useful coordinate loads, not physical DRAM traffic.
double access(const std::vector<woby::Coordinate>& source, std::span<const uint32_t> indices) {
    std::array<double, 8> sums{};
    for (size_t i = 0; i < indices.size(); ++i) {
        const auto& p = source[indices[i]];
        sums[i % sums.size()] += p[0] + p[1] + p[2];
    }
    return std::accumulate(sums.begin(), sums.end(), 0.0);
}
Json memorySweep(const woby::Mesh& mesh) {
    const auto& source = *woby::meshGeometry(mesh).sourcePositions;
    const auto& actual = mesh.sourceData->indices;
    const size_t reads = std::min(size_t{8 * 1024 * 1024}, actual.size());
    Json rows = Json::array();
    std::vector<uint32_t> order(reads);
    for (const size_t bytes : {size_t{16384}, size_t{524288}, size_t{8 * 1024 * 1024},
                              size_t{32 * 1024 * 1024}, size_t{128 * 1024 * 1024}, size_t{1024} * 1024 * 1024}) {
        const auto count = std::min(source.size(), std::max(size_t{1}, bytes / sizeof(woby::Coordinate)));
        for (int pattern = 0; pattern < 3; ++pattern) {
            uint64_t random = 0x8128;
            for (size_t i = 0; i < reads; ++i) {
                random ^= random << 13; random ^= random >> 7; random ^= random << 17;
                order[i] = static_cast<uint32_t>((pattern == 0 ? i : pattern == 1 ? actual[i] : random) % count);
            }
            for (int repeat = 0; repeat < 3; ++repeat) {
                const auto start = Clock::now();
                const auto check = access(source, order);
                std::atomic_signal_fence(std::memory_order_seq_cst);
                const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
                rows.push_back({{"working_set_bytes", count * sizeof(woby::Coordinate)}, {"reads", reads},
                    {"pattern", pattern == 0 ? "sequential" : pattern == 1 ? "actual_prefix_wrapped" : "random"},
                    {"repeat", repeat}, {"seconds", elapsed}, {"ns_per_read", elapsed * 1e9 / static_cast<double>(reads)},
                    {"useful_GB_per_second", static_cast<double>(reads * sizeof(woby::Coordinate)) / elapsed / 1e9}, {"checksum", check}});
            }
        }
    }
    return rows;
}
template<bool Blend> __m128d select(__m128d previous, __m128d value, __m128d mask) {
    if constexpr (Blend) { return _mm_blendv_pd(previous, value, mask); }
    else { return _mm_or_pd(_mm_and_pd(mask, value), _mm_andnot_pd(mask, previous)); }
}
template<bool Blend> Box simdRange(const woby::Mesh& mesh, size_t beginIndex, size_t endIndex) {
    const auto& g = woby::meshGeometry(mesh);
    const auto& source = *g.sourcePositions;
    const bool identity = g.positionIndexing == woby::PositionIndexing::identity;
    auto lowXY = _mm_set1_pd(INFINITY), highXY = _mm_set1_pd(-INFINITY);
    auto lowZ = _mm_set_sd(INFINITY), highZ = _mm_set_sd(-INFINITY);
    const auto originXY = _mm_loadu_pd(mesh.origin.data()), originZ = _mm_load_sd(mesh.origin.data()+2);
    for (size_t i = beginIndex; i < endIndex; ++i) {
        const auto render = g.indices[i];
        const auto& p = source[identity ? render : g.positionIndices[render]];
        const auto xy = _mm_add_pd(_mm_loadu_pd(p.data()), originXY);
        const auto z = _mm_add_sd(_mm_load_sd(p.data()+2), originZ);
        lowXY = select<Blend>(lowXY, xy, _mm_cmplt_pd(xy, lowXY));
        highXY = select<Blend>(highXY, xy, _mm_cmplt_pd(highXY, xy));
        lowZ = select<Blend>(lowZ, z, _mm_cmplt_sd(z, lowZ));
        highZ = select<Blend>(highZ, z, _mm_cmplt_sd(highZ, z));
    }
    Box out;
    _mm_storeu_pd(out[0].data(), lowXY); _mm_store_sd(out[0].data()+2, lowZ);
    _mm_storeu_pd(out[1].data(), highXY); _mm_store_sd(out[1].data()+2, highZ);
    return out;
}
template<bool Blend> Box simdBounds(const woby::Mesh& mesh, size_t workers) {
    const auto count = woby::meshGeometry(mesh).indices.size();
    if (workers == 1) { return simdRange<Blend>(mesh, 0, count); }
    std::vector<std::future<Box>> jobs;
    for (size_t i = 0; i < workers; ++i) {
        jobs.push_back(std::async(std::launch::async, [&, i] {
            return simdRange<Blend>(mesh, count*i/workers, count*(i+1)/workers);
        }));
    }
    auto out = emptyBox();
    for (auto& job : jobs) {
        const auto part = job.get();
        if (part[0][0] <= part[1][0]) { expand(out, part[0]); expand(out, part[1]); }
    }
    return out;
}
}
Json simdReplay(const woby::Mesh& mesh) {
    int cpu[4];
    __cpuid(cpu, 1);
    if (!(cpu[2] & (1 << 19))) { throw std::runtime_error("SSE4.1 replay requires SSE4.1 support"); }
    const auto& g = woby::meshGeometry(mesh);
    if (!g.sourcePositions || g.nodes.size() != 1 || g.nodes.front().indexOffset || g.nodes.front().indexCount != g.indices.size()) {
        throw std::runtime_error("SIMD replay needs one complete OBJ triangle group");
    }
    const auto reference = woby::originalMeshBounds(mesh, &g.nodes.front());
    const std::vector<woby::Coordinate> unused;
    Json rows = Json::array();
    const auto timeCase = [&](const char* name, int repeat, size_t workers, auto operation) {
        const auto phase = begin(name);
        const auto box = operation();
        const auto seconds = std::chrono::duration<double>(Clock::now()-phase.start).count();
        end(phase, {{"repeat", repeat}, {"workers", workers}, {"visits", g.indices.size()}});
        if (box != reference) { throw std::runtime_error(std::string("SIMD bounds mismatch: ") + name); }
        rows.push_back({{"name", name}, {"repeat", repeat}, {"workers", workers}, {"seconds", seconds}, {"bounds", box}});
    };
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (const size_t workers : {size_t{1}, size_t{16}}) {
            timeCase("simd_cpp_chain", repeat, workers, [&] { return bounds<1>(mesh, unused, workers); });
            timeCase("simd_sse2_select", repeat, workers, [&] { return simdBounds<false>(mesh, workers); });
            timeCase("simd_sse41_blend", repeat, workers, [&] { return simdBounds<true>(mesh, workers); });
        }
    }
    return {{"bounds", rows}, {"visits_per_case", g.indices.size()}};
}
Json deepReplay(const woby::Mesh& mesh) {
    const auto& g = woby::meshGeometry(mesh);
    if (!g.sourcePositions || !mesh.sourceData || mesh.sourceData->indices.size() != g.indices.size() || g.indices.empty()) {
        throw std::runtime_error("Replay needs OBJ triangles and source indices");
    }
    Json out, rows = Json::array();
    const auto allocate = begin("replay_expanded_positions");
    std::vector<woby::Coordinate> packed;
    packed.reserve(g.vertices.size());
    for (size_t i = 0; i < g.vertices.size(); ++i) { packed.push_back(woby::originalPosition(woby::meshPosition(mesh, i), mesh.origin)); }
    end(allocate, {{"bytes", packed.size() * sizeof(woby::Coordinate)}});
    const auto reference = woby::originalMeshBounds(mesh, &g.nodes.front());
    const auto timeCase = [&](const char* name, int repeat, size_t count, size_t workers, auto operation) {
        const auto phase = begin(name);
        const auto box = operation();
        const auto seconds = std::chrono::duration<double>(Clock::now() - phase.start).count();
        end(phase, {{"repeat", repeat}, {"workers", workers}, {"visits", count}});
        if (box != reference) { throw std::runtime_error(std::string("Bounds replay mismatch: ") + name); }
        rows.push_back({{"name", name}, {"repeat", repeat}, {"workers", workers}, {"visits", count},
                       {"seconds", seconds}, {"bounds", box}});
    };
    // Every generated triangle vertex is used. A complete triangle group is required
    // for the unique-vertex scan to have the same bounding box.
    if (g.nodes.size() != 1 || g.nodes.front().indexOffset || g.nodes.front().indexCount != g.indices.size()) {
        throw std::runtime_error("Replay needs one complete triangle group");
    }
    for (int repeat = 0; repeat < 3; ++repeat) {
        timeCase("replay_original_api", repeat, g.indices.size(), 1, [&] { return bounds<0>(mesh, packed, 1); });
        timeCase("replay_unchecked_chain", repeat, g.indices.size(), 1, [&] { return bounds<1>(mesh, packed, 1); });
        timeCase("replay_source_indices", repeat, g.indices.size(), 1, [&] { return bounds<2>(mesh, packed, 1); });
        timeCase("replay_expanded_indexed", repeat, g.indices.size(), 1, [&] { return bounds<3>(mesh, packed, 1); });
        timeCase("replay_unique_vertices", repeat, packed.size(), 1, [&] { return bounds<4>(mesh, packed, 1); });
        for (const size_t workers : {size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
            timeCase("replay_source_parallel", repeat, g.indices.size(), workers, [&] { return bounds<2>(mesh, packed, workers); });
            timeCase("replay_chain_parallel", repeat, g.indices.size(), workers, [&] { return bounds<1>(mesh, packed, workers); });
        }
        for (const size_t ahead : {size_t{16}, size_t{64}, size_t{256}}) {
            timeCase(("replay_source_prefetch_" + std::to_string(ahead)).c_str(), repeat, g.indices.size(), 1,
                     [&] { return bounds<2>(mesh, packed, 1, ahead); });
        }
    }
    out["bounds"] = rows;
    const auto properties = begin("replay_index_locality");
    out["source_index_locality"] = locality(mesh.sourceData->indices, sizeof(woby::Coordinate));
    out["render_index_locality"] = locality(g.indices, sizeof(woby::Vertex));
    out["render_to_source_locality"] = locality(g.positionIndices, sizeof(woby::Coordinate));
    end(properties);
    const auto sweep = begin("replay_memory_sweep");
    out["access_sweep"] = memorySweep(mesh);
    end(sweep);
    out["expanded_position_bytes"] = packed.size() * sizeof(woby::Coordinate);
    return out;
}
} // namespace obj_profile
