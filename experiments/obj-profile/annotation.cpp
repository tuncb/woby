// Complete annotation preparation, with output validation outside the timer.
#include "obj_mesh.h"
#include "post_loading.h"
#include "surface_annotation.h"
#include "point_cloud.h"
#include "profile.h"
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
struct Digest { uint64_t value = 14695981039346656037ull; };
void add(Digest& digest, uint64_t value)
{
    for (unsigned byte = 0; byte < 8; ++byte) {
        digest.value = (digest.value ^ (value & 255)) * 1099511628211ull;
        value >>= 8;
    }
}
void add(Digest& digest, float value) { add(digest, uint64_t{std::bit_cast<uint32_t>(value)}); }
void add(Digest& digest, double value) { add(digest, std::bit_cast<uint64_t>(value)); }
template <typename T, size_t N> void add(Digest& digest, const std::array<T, N>& values)
{
    for (const auto& value : values) { add(digest, value); }
}
obj_profile::Json describe(const woby::MeshAnnotationCache& cache)
{
    Digest blocks, bounds, triangles, tree;
    for (const auto& block : cache.blocks) {
        add(blocks, block.begin); add(blocks, block.end);
        add(blocks, block.minimum); add(blocks, block.maximum);
    }
    for (const auto& box : cache.localBounds) {
        add(bounds, uint64_t{box.has_value()});
        if (box) { add(bounds, *box); }
    }
    for (const auto offset : cache.spatial->triangles) { add(triangles, uint64_t{offset}); }
    for (const auto& node : cache.spatial->tree) {
        add(tree, node.minimum); add(tree, node.maximum);
        add(tree, node.begin); add(tree, node.end); add(tree, node.left); add(tree, node.right);
        add(tree, node.indexBegin); add(tree, node.indexEnd);
    }
    return {{"vertices", cache.vertexCount}, {"corners", cache.indexCount},
        {"blocks", cache.blocks.size()}, {"triangles", cache.spatial->triangles.size()},
        {"tree_nodes", cache.spatial->tree.size()}, {"fingerprints", cache.fingerprints},
        {"block_digest", blocks.value}, {"precise_bounds_digest", bounds.value},
        {"triangle_order_digest", triangles.value}, {"tree_digest", tree.value}};
}
obj_profile::Json describe(const woby::SceneMeshPreparation& prepared)
{
    Digest membership, ranges, points, proxies, nodes, colors, cuts;
    const auto* cloud = prepared.pointCloud.get();
    const auto& sources = cloud ? cloud->sourceVertices : prepared.pointVertexIndices;
    for (const auto value : sources) { add(membership, uint64_t{value}); }
    for (const auto& range : prepared.nodeRanges) {
        for (const auto value : {range.triangleIndexOffset, range.triangleIndexCount,
            range.lineIndexOffset, range.lineIndexCount, range.pointIndexOffset, range.pointIndexCount}) {
            add(ranges, uint64_t{value});
        }
        add(ranges, uint64_t{range.completeVertexSet});
    }
    if (cloud) {
        for (const auto& point : cloud->points) { add(points, point.position); add(points, uint64_t{point.id}); }
        for (const auto& point : cloud->proxies) { add(proxies, point.position); add(proxies, uint64_t{point.id}); }
        for (const auto& node : cloud->nodes) {
            add(nodes, node.low); add(nodes, node.high);
            for (const auto value : {node.begin, node.count, node.left, node.right,
                node.proxyBegin, node.proxyCount, node.group}) { add(nodes, uint64_t{value}); }
            for (const auto value : node.extrema) { add(nodes, uint64_t{value}); }
        }
        for (const auto value : cloud->roots) { add(nodes, uint64_t{value}); }
        for (const auto& group : cloud->groups) {
            add(ranges, uint64_t{group.firstId}); add(ranges, uint64_t{group.endId});
            add(ranges, group.model); add(ranges, group.color); add(ranges, uint64_t{group.enabled});
        }
        for (const auto value : cloud->colors) { add(colors, uint64_t{value}); }
        for (const auto& cut : cloud->navigationCuts) {
            add(cuts, cut.size());
            for (const auto& node : cut) { add(cuts, uint64_t{node.node}); add(cuts, uint64_t{node.full}); }
        }
    }
    return {{"source_count", sources.size()}, {"membership_digest", membership.value},
        {"ranges_digest", ranges.value}, {"points_digest", points.value}, {"proxies_digest", proxies.value},
        {"nodes_digest", nodes.value}, {"colors_digest", colors.value}, {"cuts_digest", cuts.value},
        {"point_count", cloud ? cloud->points.size() : 0}, {"node_count", cloud ? cloud->nodes.size() : 0},
        {"proxy_count", cloud ? cloud->proxies.size() : 0}, {"upload_bytes", prepared.uploadBytes}};
}
int run(const std::filesystem::path& path, size_t rounds, std::wstring_view mode)
{
    woby::ObjPrototypeOptions options; options.deferPreparation = true;
    auto mesh = woby::loadObjMeshPrototype(path, {}, options);
    obj_profile::Json rows = obj_profile::Json::array(), expected;
    for (size_t round = 0; round < rounds; ++round) {
        const auto before = obj_profile::counters();
        const auto start = obj_profile::Clock::now();
        std::optional<woby::PostLoadData> prepared;
        std::optional<woby::SceneMeshPreparation> points;
        std::shared_ptr<const woby::MeshAnnotationCache> cache;
        if (mode == L"post-load") {
            prepared = woby::preparePostLoadData(mesh);
            if (prepared) { cache = prepared->annotations; }
        } else if (mode == L"points" || mode == L"membership") {
            points = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, mode == L"points");
        } else { cache = woby::buildAnnotationMeshCache(mesh); }
        const auto elapsed = std::chrono::duration<double, std::milli>(obj_profile::Clock::now() - start).count();
        const auto after = obj_profile::counters();
        if (!cache && !points) { throw std::runtime_error("Preparation produced no output"); }
        auto output = cache ? describe(*cache) : describe(*points);
        if (prepared) { output["points"] = describe(prepared->points); }
        if (round && output != expected) { throw std::runtime_error("Preparation output changed between runs"); }
        expected = output;
        rows.push_back({{"round", round + 1}, {"wall_ms", elapsed},
            {"cpu_ms", double(after.user + after.kernel - before.user - before.kernel) / 10000.0},
            {"peak_commit_bytes", after.peakCommit}, {"output", output}});
    }
    auto report = obj_profile::result();
    report["mode"] = mode == L"post-load" ? "post-load" : mode == L"points" ? "points"
        : mode == L"membership" ? "membership" : "annotation";
    report["records"] = std::move(rows);
    std::cout << report.dump() << '\n';
    return 0;
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc < 2 || argc > 4) { throw std::invalid_argument("Usage: woby_annotation_benchmark OBJ [rounds] [annotation|post-load|points|membership]"); }
        const auto rounds = argc > 2 ? std::stoull(argv[2]) : 3;
        if (!rounds || rounds > 20) { throw std::invalid_argument("Rounds must be between 1 and 20"); }
        const std::wstring_view mode = argc > 3 ? argv[3] : L"annotation";
        if (mode != L"annotation" && mode != L"post-load" && mode != L"points" && mode != L"membership") {
            throw std::invalid_argument("Invalid benchmark mode");
        }
        return run(argv[1], static_cast<size_t>(rounds), mode);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
