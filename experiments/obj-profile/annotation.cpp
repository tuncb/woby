// Complete annotation preparation, with output validation outside the timer.
#include "obj_mesh.h"
#include "post_loading.h"
#include "surface_annotation.h"
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
int run(const std::filesystem::path& path, size_t rounds, bool postLoad)
{
    woby::ObjPrototypeOptions options; options.deferPreparation = true;
    auto mesh = woby::loadObjMeshPrototype(path, {}, options);
    obj_profile::Json rows = obj_profile::Json::array(), expected;
    for (size_t round = 0; round < rounds; ++round) {
        const auto before = obj_profile::counters();
        const auto start = obj_profile::Clock::now();
        std::optional<woby::PostLoadData> prepared;
        std::shared_ptr<const woby::MeshAnnotationCache> cache;
        if (postLoad) {
            prepared = woby::preparePostLoadData(mesh);
            if (prepared) { cache = prepared->annotations; }
        } else { cache = woby::buildAnnotationMeshCache(mesh); }
        const auto elapsed = std::chrono::duration<double, std::milli>(obj_profile::Clock::now() - start).count();
        const auto after = obj_profile::counters();
        if (!cache) { throw std::runtime_error("Annotation preparation produced no cache"); }
        const auto output = describe(*cache);
        if (round && output != expected) { throw std::runtime_error("Annotation output changed between runs"); }
        expected = output;
        rows.push_back({{"round", round + 1}, {"wall_ms", elapsed},
            {"cpu_ms", double(after.user + after.kernel - before.user - before.kernel) / 10000.0},
            {"peak_commit_bytes", after.peakCommit}, {"output", output}});
    }
    auto report = obj_profile::result();
    report["mode"] = postLoad ? "post-load" : "annotation";
    report["records"] = std::move(rows);
    std::cout << report.dump() << '\n';
    return 0;
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc < 2 || argc > 4) { throw std::invalid_argument("Usage: woby_annotation_benchmark OBJ [rounds] [annotation|post-load]"); }
        const auto rounds = argc > 2 ? std::stoull(argv[2]) : 3;
        if (!rounds || rounds > 20) { throw std::invalid_argument("Rounds must be between 1 and 20"); }
        const std::wstring_view mode = argc > 3 ? argv[3] : L"annotation";
        if (mode != L"annotation" && mode != L"post-load") { throw std::invalid_argument("Invalid benchmark mode"); }
        return run(argv[1], static_cast<size_t>(rounds), mode == L"post-load");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
