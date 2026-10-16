// Replay the annotation block pass on the same immutable OBJ geometry.
#include "obj_mesh.h"
#include "surface_annotation.h"
#include "hash_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
struct NodeWork { size_t first = 0, last = 0; };
struct Output {
    std::vector<woby::MeshAnnotationBlock> blocks;
    std::vector<std::array<woby::Coordinate, 2>> precise;
    std::vector<std::string> hashes;
};
void combine(uint64_t& hash, uint64_t value) {
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
}
bool finite(const auto& p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }
auto emptyPrecise() {
    std::array<woby::Coordinate, 2> box;
    box[0].fill(std::numeric_limits<double>::infinity());
    box[1].fill(-std::numeric_limits<double>::infinity());
    return box;
}
template<int Mode> Output replay(const woby::Mesh& mesh, size_t workers) {
    const auto& geometry = woby::meshGeometry(mesh);
    const auto& vertices = geometry.vertices;
    const auto& indices = geometry.indices;
    const auto& source = *geometry.sourcePositions;
    const bool identity = geometry.positionIndexing == woby::PositionIndexing::identity;
    Output output;
    std::vector<NodeWork> work;
    for (const auto& node : geometry.nodes) {
        NodeWork range{output.blocks.size(), 0};
        const auto end = std::min(indices.size(), size_t{node.indexOffset} + node.indexCount);
        for (size_t begin = node.indexOffset; begin < end; begin += 768) {
            woby::MeshAnnotationBlock block;
            block.begin = begin; block.end = std::min(end, begin + 768);
            block.minimum.fill(std::numeric_limits<float>::infinity());
            block.maximum.fill(-std::numeric_limits<float>::infinity());
            output.blocks.push_back(block);
        }
        range.last = output.blocks.size(); work.push_back(range);
    }
    output.precise.assign(output.blocks.size(), emptyPrecise());
    const auto blockWork = [&](size_t first, size_t last, [[maybe_unused]] uint64_t& hash) {
        for (size_t b = first; b < last; ++b) {
            auto& block = output.blocks[b];
            auto& exact = output.precise[b];
            for (size_t i = block.begin; i < block.end; ++i) {
                const auto index = indices[i];
                if constexpr (Mode < 4) {
                    if constexpr (Mode == 0) { woby::hashCombine(hash, index); }
                    else { combine(hash, index); }
                }
                if (index >= vertices.size()) { continue; }
                const auto& p = vertices[index].position;
                if constexpr (Mode < 4) {
                    if constexpr (Mode == 0) { woby::hashArray3(hash, p); }
                    else { for (const auto value : p) { combine(hash, std::bit_cast<uint32_t>(value)); } }
                }
                if constexpr (Mode != 3) {
                    if (!finite(p)) { continue; }
                    const auto position = [&] {
                        if constexpr (Mode == 2 || Mode == 5) { return source[identity ? index : geometry.positionIndices[index]]; }
                        else { return woby::meshPosition(mesh, index); }
                    }();
                    const auto validPrecise = [&] {
                        if constexpr (Mode == 0 || Mode == 1 || Mode == 4) { return woby::finiteCoordinate(position); }
                        else { return finite(position); }
                    }();
                    if (validPrecise) {
                        for (size_t axis = 0; axis < 3; ++axis) {
                            exact[0][axis] = std::min(exact[0][axis], position[axis]);
                            exact[1][axis] = std::max(exact[1][axis], position[axis]);
                        }
                    }
                    for (size_t axis = 0; axis < 3; ++axis) {
                        block.minimum[axis] = std::min(block.minimum[axis], p[axis]);
                        block.maximum[axis] = std::max(block.maximum[axis], p[axis]);
                    }
                }
            }
        }
    };
    if constexpr (Mode < 4) {
        for (size_t n = 0; n < work.size(); ++n) {
            uint64_t hash = 1469598103934665603ull;
            combine(hash, geometry.nodes[n].indexCount);
            blockWork(work[n].first, work[n].last, hash);
            output.hashes.push_back(std::to_string(hash));
        }
    } else {
        const auto execute = [&](size_t worker) {
            uint64_t unused = 0;
            blockWork(output.blocks.size() * worker / workers, output.blocks.size() * (worker + 1) / workers, unused);
        };
        std::vector<std::jthread> jobs;
        for (size_t i = 1; i < workers; ++i) { jobs.emplace_back([&, i] { execute(i); }); }
        execute(0);
    }
    return output;
}
bool equalBounds(const Output& output, const woby::MeshAnnotationCache& reference, const woby::Mesh& mesh) {
    if (output.blocks.size() != reference.blocks.size()) { return false; }
    for (size_t b = 0; b < output.blocks.size(); ++b) {
        const auto& a = output.blocks[b]; const auto& e = reference.blocks[b];
        if (a.begin != e.begin || a.end != e.end || a.minimum != e.minimum || a.maximum != e.maximum) { return false; }
    }
    for (size_t n = 0; n < woby::meshGeometry(mesh).nodes.size(); ++n) {
        auto combined = emptyPrecise();
        const auto& node = woby::meshGeometry(mesh).nodes[n];
        for (size_t b = 0; b < output.blocks.size(); ++b) {
            if (output.blocks[b].begin < node.indexOffset || output.blocks[b].end > size_t{node.indexOffset} + node.indexCount) { continue; }
            for (size_t axis = 0; axis < 3; ++axis) {
                combined[0][axis] = std::min(combined[0][axis], output.precise[b][0][axis]);
                combined[1][axis] = std::max(combined[1][axis], output.precise[b][1][axis]);
            }
        }
        if (!reference.localBounds[n] || combined != *reference.localBounds[n]) { return false; }
    }
    return true;
}
template<int Mode> void measure(Json& rows, const woby::Mesh& mesh, const woby::MeshAnnotationCache& reference,
    const char* name, size_t workers, size_t round) {
    const auto start = Clock::now();
    const auto output = replay<Mode>(mesh, workers);
    const auto milliseconds = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    const bool validHash = Mode >= 4 || output.hashes == reference.fingerprints;
    const bool validBounds = Mode == 3 || equalBounds(output, reference, mesh);
    if (!validHash || !validBounds) { throw std::runtime_error(std::string("Replay differs: ") + name); }
    rows.push_back({{"name", name}, {"round", round}, {"workers", workers}, {"wall_ms", milliseconds},
        {"hash_validated", Mode < 4}, {"bounds_validated", Mode != 3}});
}
int run(const std::filesystem::path& path, size_t rounds) {
    woby::ObjPrototypeOptions options; options.deferPreparation = true;
    auto mesh = woby::loadObjMeshPrototype(path, {}, options);
    const auto start = Clock::now();
    const auto reference = woby::buildAnnotationMeshCache(mesh);
    if (!reference) { throw std::runtime_error("The replay requires an OBJ triangle mesh"); }
    Json rows = Json::array();
    const auto baseline = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    for (size_t round = 1; round <= rounds; ++round) {
        // Rotate the long combined kernels to limit order bias.
        for (size_t ordinal = 0; ordinal < 3; ++ordinal) {
            switch ((ordinal + round - 1) % 3) {
            case 0: measure<0>(rows, mesh, *reference, "combined_current_calls", 1, round); break;
            case 1: measure<1>(rows, mesh, *reference, "combined_inline_hash", 1, round); break;
            case 2: measure<2>(rows, mesh, *reference, "combined_inline_hash_direct_position", 1, round); break;
            }
        }
        measure<3>(rows, mesh, *reference, "hash_inline_only", 1, round);
        measure<4>(rows, mesh, *reference, "bounds_api_only", 1, round);
        measure<5>(rows, mesh, *reference, "bounds_direct_only", 1, round);
        measure<5>(rows, mesh, *reference, "bounds_direct_parallel", 8, round);
    }
    std::cout << Json{{"method", "Owned immutable geometry; one production cache reference; isolated passes; validation and result destruction outside timers"},
        {"reference_cache_ms", baseline}, {"vertices", woby::meshGeometry(mesh).vertices.size()},
        {"corners", woby::meshGeometry(mesh).indices.size()}, {"groups", woby::meshGeometry(mesh).nodes.size()},
        {"blocks", reference->blocks.size()}, {"fingerprints", reference->fingerprints}, {"records", rows}}.dump() << '\n';
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2 || argc > 3) { throw std::invalid_argument("Usage: woby_loading_audit_kernels OBJ [rounds]"); }
        const auto rounds = argc == 3 ? std::stoull(argv[2]) : 3;
        if (!rounds || rounds > 10) { throw std::invalid_argument("Rounds must be between one and ten"); }
        return run(std::filesystem::path(argv[1]), static_cast<size_t>(rounds));
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
