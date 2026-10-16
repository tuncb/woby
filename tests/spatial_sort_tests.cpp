#include "spatial_sort.h"
#include "point_cloud.h"
#include "post_loading.h"
#include "surface_annotation.h"
#include <doctest/doctest.h>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

namespace {
using Key = std::pair<uint32_t, uint32_t>;
struct ReservedWorkers {
    unsigned count = woby::analysisWorkerLimit();
    ReservedWorkers() { woby::analysisWorkerCount.fetch_add(count); }
    ~ReservedWorkers() { woby::analysisWorkerCount.fetch_sub(count); }
};
std::vector<Key> testKeys(size_t count)
{
    std::mt19937 random(173);
    std::vector<Key> keys(count);
    for (size_t i = 0; i < count; ++i) {
        const auto value = static_cast<uint32_t>(random());
        keys[i] = {i % 3 == 0 ? value : value % 17, static_cast<uint32_t>(i)};
    }
    return keys;
}
woby::Mesh testMesh()
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    constexpr uint32_t count = 300001;
    g.vertices.resize(193);
    for (uint32_t i = 0; i < g.vertices.size(); ++i) {
        g.vertices[i].position = {float(i % 7), float(i % 11), float(i % 13)};
    }
    g.indices.resize(count * 3);
    for (uint32_t i = 0; i < count; ++i) {
        const auto vertex = (i * 37) % 191;
        // Repeated centers cross both key-generation and radix chunk boundaries.
        g.indices[i * 3] = vertex; g.indices[i * 3 + 1] = vertex + 1; g.indices[i * 3 + 2] = vertex + 2;
    }
    g.nodes = {{"first", 0, 450000}, {"second", 450000, count * 3 - 450000}};
    mesh.bounds = woby::calculateBounds(g.vertices);
    woby::publishMeshGeometry(mesh);
    return mesh;
}
}

TEST_CASE("Spatial radix sorting matches stable ordering for full width keys and ties")
{
    for (const size_t count : {size_t{0}, size_t{1}, size_t{257}, woby::spatialKeyBatchSize * 2 + 17}) {
        auto input = testKeys(count);
        if (count > 2) { input[0].first = UINT32_MAX; input[1].first = 0x80000000u; }
        auto expected = input;
        std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        auto parallel = input;
        woby::stableSpatialRadixSort(parallel, [](const auto& key) { return key.first; }, [] {});
        CHECK(parallel == expected);
        ReservedWorkers reserved;
        woby::stableSpatialRadixSort(input, [](const auto& key) { return key.first; }, [] {});
        CHECK(input == expected);
    }
    std::vector<Key> equal(woby::spatialKeyBatchSize * 2 + 3);
    for (size_t i = 0; i < equal.size(); ++i) { equal[i] = {UINT32_MAX, static_cast<uint32_t>(i)}; }
    const auto expected = equal;
    woby::stableSpatialRadixSort(equal, [](const auto& key) { return key.first; }, [] {});
    CHECK(equal == expected);
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Spatial jobs join on failure cancellation and concurrent nested preparation")
{
    auto keys = testKeys(woby::spatialKeyBatchSize * 3 + 17);
    std::atomic<unsigned> calls = 0;
    CHECK_THROWS_WITH(woby::stableSpatialRadixSort(keys, [](const auto& key) { return key.first; }, [&] {
        if (calls.fetch_add(1) >= 12) { throw std::runtime_error("Canceled test job."); }
    }), "Canceled test job.");
    CHECK(woby::analysisWorkerCount.load() == 0);
    std::atomic<bool> exceeded = false;
    std::array<std::vector<Key>, 2> outputs{keys, keys};
    const auto check = [&] {
        if (woby::analysisWorkerCount.load() > woby::analysisWorkerLimit()) { exceeded = true; }
    };
    woby::parallelWorkBatches(2, 1, check, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            woby::stableSpatialRadixSort(outputs[i], [](const auto& key) { return key.first; }, check);
        }
    }, 2);
    CHECK_FALSE(exceeded.load());
    CHECK(outputs[0] == outputs[1]);
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Large spatial partitions preserve ties constant digits and skewed full width keys")
{
    constexpr size_t count = (size_t{1} << 20) + 37;
    std::vector<Key> input(count);
    std::mt19937 random(179);
    uint32_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i % 3 == 0) { previous = static_cast<uint32_t>(random()); }
        input[i] = {previous, static_cast<uint32_t>(i)};
    }
    SUBCASE("all digits vary") {}
    SUBCASE("equal keys recur across every input chunk") {
        for (size_t i = 0; i < count; ++i) { input[i].first = static_cast<uint32_t>((i * 997) % 4093) * 2654435761u; }
    }
    SUBCASE("low digit is constant") { for (auto& key : input) { key.first |= 1023u; } }
    SUBCASE("middle digit is constant") { for (auto& key : input) { key.first &= ~(511u << 10); } }
    SUBCASE("both local digits are constant") { for (auto& key : input) { key.first |= (1u << 19) - 1; } }
    SUBCASE("all keys are equal") { for (auto& key : input) { key.first = UINT32_MAX; } }
    SUBCASE("one large partition with constant high bits") {
        for (auto& key : input) { key.first = 0xabc00000u | (key.first & ((1u << 19) - 1)); }
    }
    SUBCASE("one large partition and a distant outlier") {
        for (auto& key : input) { key.first &= (1u << 19) - 1; }
        input.back().first = UINT32_MAX;
    }
    auto expected = input;
    std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const unsigned cap : {0u, 3u}) {
        auto actual = input;
        std::atomic<bool> exceeded = false;
        woby::stableSpatialRadixSort(actual, [](const auto& key) { return key.first; }, [&] {
            if (woby::analysisWorkerCount.load() > std::min(cap, woby::analysisWorkerLimit())) { exceeded = true; }
        }, cap);
        CHECK(actual == expected);
        CHECK_FALSE(exceeded.load());
        CHECK(woby::analysisWorkerCount.load() == 0);
    }
}

TEST_CASE("Large spatial sort joins workers after cancellation in each part of the sort")
{
    constexpr size_t count = (size_t{1} << 20) + 19;
    std::vector<Key> input(count);
    std::mt19937 random(181);
    for (size_t i = 0; i < count; ++i) { input[i] = {static_cast<uint32_t>(random()), static_cast<uint32_t>(i)}; }
    auto sorted = input;
    std::atomic<size_t> completedChecks = 0;
    woby::stableSpatialRadixSort(sorted, [](const auto& key) { return key.first; }, [&] { ++completedChecks; }, 3);
    for (const size_t after : {size_t{1}, completedChecks.load() / 5,
        completedChecks.load() * 2 / 5, completedChecks.load() * 3 / 5, completedChecks.load() * 4 / 5}) {
        auto actual = input;
        std::atomic<size_t> calls = 0;
        CHECK_THROWS_WITH(woby::stableSpatialRadixSort(actual, [](const auto& key) { return key.first; }, [&] {
            if (++calls >= after) { throw std::runtime_error("Partition sort canceled"); }
        }, 3), "Partition sort canceled");
        CHECK(woby::analysisWorkerCount.load() == 0);
    }
    // A throwing key accessor must also leave no helpers running.
    auto actual = input;
    std::atomic<size_t> reads = 0;
    CHECK_THROWS_WITH(woby::stableSpatialRadixSort(actual, [&](const auto& key) {
        if (++reads == count + 100) { throw std::runtime_error("Key access failed"); }
        return key.first;
    }, [] {}, 3), "Key access failed");
    CHECK(woby::analysisWorkerCount.load() == 0);
    actual = input;
    woby::stableSpatialRadixSort(actual, [](const auto& key) { return key.first; }, [] {}, 3);
    CHECK(actual == sorted);
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Per-job helper limits keep the shared budget and preserve complete ordered output")
{
    for (const unsigned cap : {0u, 1u, 3u}) {
        std::atomic<bool> exceeded = false;
        const auto check = [&] {
            if (woby::analysisWorkerCount.load() > std::min(cap, woby::analysisWorkerLimit())) { exceeded = true; }
        };
        std::vector<size_t> result(300003, SIZE_MAX);
        woby::parallelWorkBatches(result.size(), 1024, check, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) { result[i] = i; }
        }, 4096, cap);
        for (size_t i = 0; i < result.size(); ++i) { REQUIRE(result[i] == i); }
        auto keys = testKeys(result.size()), expected = keys;
        std::stable_sort(expected.begin(), expected.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        woby::stableSpatialRadixSort(keys, [](const auto& key) { return key.first; }, check, cap);
        CHECK(keys == expected);
        CHECK_FALSE(exceeded.load());
        CHECK(woby::analysisWorkerCount.load() == 0);
        std::atomic<size_t> calls = 0;
        CHECK_THROWS_WITH(woby::parallelWorkBatches(result.size(), 1024, [&] {
            if (++calls > 8) { throw std::runtime_error("Limited job canceled"); }
        }, [](size_t, size_t) {}, 4096, cap), "Limited job canceled");
        CHECK(woby::analysisWorkerCount.load() == 0);
    }
}

TEST_CASE("Annotation spatial preparation preserves fingerprints ties and invalid triangle filtering")
{
    auto mesh = testMesh();
    SUBCASE("invalid indices and nonfinite positions") {
        auto& g = woby::editMeshGeometry(mesh);
        g.indices[0] = UINT32_MAX;
        g.indices[65536 * 3 + 1] = UINT32_MAX;
        g.vertices[191].position[0] = std::numeric_limits<float>::quiet_NaN();
        woby::publishMeshGeometry(mesh);
    }
    SUBCASE("nonmonotonic group order with overlapping ranges") {
        auto& g = woby::editMeshGeometry(mesh);
        std::swap(g.nodes[0], g.nodes[1]);
        g.nodes.push_back({"overlap", 3, 1536});
        woby::publishMeshGeometry(mesh);
    }
    const auto& g = woby::meshGeometry(mesh);
    std::shared_ptr<const woby::MeshAnnotationCache> serial;
    { ReservedWorkers reserved; serial = woby::buildAnnotationMeshCache(mesh); }
    const auto parallel = woby::buildAnnotationMeshCache(mesh);
    REQUIRE(serial); REQUIRE(parallel);
    CHECK(parallel->spatial->triangles == serial->spatial->triangles);
    CHECK(parallel->fingerprints == serial->fingerprints);
    REQUIRE(parallel->fingerprints.size() == g.nodes.size());
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        CHECK(parallel->fingerprints[i] == woby::annotationFingerprint(mesh, g.nodes[i].indexOffset, g.nodes[i].indexCount));
    }
    for (const auto offset : parallel->spatial->triangles) {
        for (size_t k = 0; k < 3; ++k) {
            REQUIRE(g.indices[offset + k] < g.vertices.size());
            for (const auto value : g.vertices[g.indices[offset + k]].position) { REQUIRE(std::isfinite(value)); }
        }
    }
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Annotation bounds match scalar access for every coordinate storage and irregular group")
{
    auto mesh = testMesh();
    auto& geometry = woby::editMeshGeometry(mesh);
    std::vector<woby::Coordinate> positions;
    for (const auto& vertex : geometry.vertices) {
        positions.push_back({double{vertex.position[0]} + 1.e-10,
            double{vertex.position[1]} - 1.e-10, double{vertex.position[2]} + 1.e-12});
    }
    positions[0][0] = std::numeric_limits<double>::quiet_NaN();
    positions[1][1] = std::numeric_limits<double>::infinity();
    SUBCASE("render positions") {}
    SUBCASE("direct precise positions") { geometry.precisePositions = positions; }
    SUBCASE("identity source positions") {
        geometry.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(positions);
    }
    SUBCASE("mapped source positions") {
        geometry.positionIndexing = woby::PositionIndexing::mapped;
        for (size_t i = 0; i < positions.size(); ++i) {
            geometry.positionIndices.push_back(static_cast<uint32_t>(positions.size() - i - 1));
        }
        geometry.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(positions);
    }
    geometry.vertices[2].position = {-0.0f, +0.0f, -0.0f};
    geometry.vertices[3].position[0] = std::numeric_limits<float>::quiet_NaN();
    geometry.vertices[4].position[2] = std::numeric_limits<float>::infinity();
    geometry.indices[2] = UINT32_MAX;
    geometry.nodes = {{"tail", 400003, 600001}, {"empty", 0, 0}, {"first", 1, 450001},
        {"overlap", 700, 1502}, {"past end", 1000000, 3}, {"partial", 900000, 2}};
    // Include a whole rejected block before valid blocks, plus a signed-zero tie.
    std::fill_n(geometry.indices.begin() + 100000, 768, 3);
    geometry.nodes.push_back({"rejected", 100000, 768});
    geometry.indices[110000] = 2; geometry.indices[110001] = 2; geometry.indices[110002] = 2;
    geometry.nodes.push_back({"zero", 110000, 3});
    woby::publishMeshGeometry(mesh);
    const auto cache = woby::buildAnnotationMeshCache(mesh);
    REQUIRE(cache);
    const auto sameBits = [](const auto& actual, const auto& expected) {
        CHECK(std::bit_cast<std::array<std::byte, sizeof(actual)>>(actual)
            == std::bit_cast<std::array<std::byte, sizeof(expected)>>(expected));
    };
    size_t output = 0;
    for (size_t n = 0; n < geometry.nodes.size(); ++n) {
        const auto& group = geometry.nodes[n];
        CHECK(cache->fingerprints[n] == woby::annotationFingerprint(mesh, group.indexOffset, group.indexCount));
        std::array<woby::Coordinate, 2> exact;
        exact[0].fill(std::numeric_limits<double>::infinity());
        exact[1].fill(-std::numeric_limits<double>::infinity());
        const auto end = std::min(geometry.indices.size(), size_t{group.indexOffset} + group.indexCount);
        for (size_t begin = group.indexOffset; begin < end; begin += 768) {
            woby::MeshAnnotationBlock block;
            block.begin = begin; block.end = std::min(end, begin + 768);
            block.minimum.fill(std::numeric_limits<float>::infinity());
            block.maximum.fill(-std::numeric_limits<float>::infinity());
            for (size_t i = begin; i < block.end; ++i) {
                const auto index = geometry.indices[i];
                if (index >= geometry.vertices.size()) { continue; }
                const auto& p = geometry.vertices[index].position;
                if (!woby::finitePosition(p)) { continue; }
                const auto precise = woby::meshPosition(mesh, index);
                for (size_t axis = 0; axis < 3; ++axis) {
                    block.minimum[axis] = std::min(block.minimum[axis], p[axis]);
                    block.maximum[axis] = std::max(block.maximum[axis], p[axis]);
                    if (woby::finiteCoordinate(precise)) {
                        exact[0][axis] = std::min(exact[0][axis], precise[axis]);
                        exact[1][axis] = std::max(exact[1][axis], precise[axis]);
                    }
                }
            }
            if (!std::isfinite(block.minimum[0])) { continue; }
            REQUIRE(output < cache->blocks.size());
            CHECK(cache->blocks[output].begin == block.begin); CHECK(cache->blocks[output].end == block.end);
            sameBits(cache->blocks[output].minimum, block.minimum);
            sameBits(cache->blocks[output].maximum, block.maximum);
            ++output;
        }
        REQUIRE(cache->localBounds[n].has_value() == std::isfinite(exact[0][0]));
        if (cache->localBounds[n]) { sameBits(*cache->localBounds[n], exact); }
    }
    CHECK(output == cache->blocks.size());
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Annotation spatial tree matches a scalar scan and is identical without helpers")
{
    auto mesh = testMesh();
    const auto parallel = woby::buildAnnotationMeshCache(mesh);
    std::shared_ptr<const woby::MeshAnnotationCache> serial;
    { ReservedWorkers reserved; serial = woby::buildAnnotationMeshCache(mesh); }
    REQUIRE(parallel); REQUIRE(serial);
    const auto& g = woby::meshGeometry(mesh);
    const auto& spatial = *parallel->spatial;
    REQUIRE(spatial.tree.size() == serial->spatial->tree.size());
    CHECK(parallel->localBounds == serial->localBounds);
    CHECK(spatial.triangles == serial->spatial->triangles);
    for (size_t n = 0; n < spatial.tree.size(); ++n) {
        const auto& a = spatial.tree[n]; const auto& b = serial->spatial->tree[n];
        CHECK(a.minimum == b.minimum); CHECK(a.maximum == b.maximum);
        CHECK(a.begin == b.begin); CHECK(a.end == b.end);
        CHECK(a.left == b.left); CHECK(a.right == b.right);
        CHECK(a.indexBegin == b.indexBegin); CHECK(a.indexEnd == b.indexEnd);
        if (a.left || a.right) {
            REQUIRE(a.left < spatial.tree.size()); REQUIRE(a.right < spatial.tree.size());
            CHECK(a.left == n + 1);
            CHECK(spatial.tree[a.left].begin == a.begin);
            CHECK(spatial.tree[a.left].end == a.begin + (a.end - a.begin) / 2);
            CHECK(spatial.tree[a.right].begin == spatial.tree[a.left].end);
            CHECK(spatial.tree[a.right].end == a.end);
        }
        std::array<float, 3> low, high;
        low.fill(std::numeric_limits<float>::infinity()); high.fill(-std::numeric_limits<float>::infinity());
        size_t first = SIZE_MAX, last = 0;
        for (size_t i = a.begin; i < a.end; ++i) {
            const auto offset = spatial.triangles[i];
            first = std::min(first, size_t{offset}); last = std::max(last, size_t{offset} + 3);
            for (size_t k = 0; k < 3; ++k) {
                const auto& p = g.vertices[g.indices[offset + k]].position;
                for (size_t axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], p[axis]); high[axis] = std::max(high[axis], p[axis]);
                }
            }
        }
        CHECK(a.minimum == low); CHECK(a.maximum == high);
        CHECK(a.indexBegin == first); CHECK(a.indexEnd == last);
    }
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Annotation preparation joins all workers on cancellation and callback failure")
{
    auto mesh = testMesh();
    size_t completedChecks = 0;
    REQUIRE(woby::buildAnnotationMeshCache(mesh, [&] { ++completedChecks; return false; }));
    for (const size_t after : {size_t{1}, completedChecks / 8, completedChecks / 3, completedChecks * 2 / 3}) {
        size_t calls = 0;
        try {
            const auto result = woby::buildAnnotationMeshCache(mesh, [&] { return ++calls >= after; });
            CHECK_FALSE(result);
        }
        catch (const std::runtime_error&) { CHECK(calls >= after); }
        CHECK(woby::analysisWorkerCount.load() == 0);
        calls = 0;
        CHECK_THROWS_WITH((void)woby::buildAnnotationMeshCache(mesh, [&] {
            if (++calls == after) { throw std::runtime_error("Callback failed"); }
            return false;
        }), "Callback failed");
        CHECK(woby::analysisWorkerCount.load() == 0);
    }
}

TEST_CASE("Annotation tree partition boundaries keep the original preorder layout")
{
    for (const uint32_t count : {1u, 127u, 128u, 129u, 255u, 256u, 257u, 383u, 384u, 385u,
        511u, 512u, 513u, 65535u, 65536u, 65537u, 131071u, 131072u, 131073u}) {
        auto mesh = testMesh();
        auto& g = woby::editMeshGeometry(mesh);
        g.indices.resize(count * 3); g.nodes = {{"surface", 0, count * 3}};
        woby::publishMeshGeometry(mesh);
        const auto cache = woby::buildAnnotationMeshCache(mesh);
        REQUIRE(cache);
        const auto& tree = cache->spatial->tree;
        size_t cursor = 0;
        const auto visit = [&](const auto& self, size_t begin, size_t end) -> size_t {
            const auto index = cursor++;
            REQUIRE(index < tree.size());
            CHECK(tree[index].begin == begin); CHECK(tree[index].end == end);
            if (end - begin <= 128) {
                CHECK(tree[index].left == 0); CHECK(tree[index].right == 0);
            } else {
                const auto middle = begin + (end - begin) / 2;
                CHECK(tree[index].left == self(self, begin, middle));
                CHECK(tree[index].right == self(self, middle, end));
            }
            return index;
        };
        CHECK(visit(visit, 0, count) == 0);
        CHECK(cursor == tree.size());
    }
}

TEST_CASE("Annotation direct coordinate access rejects incomplete externally supplied storage")
{
    auto g = std::make_shared<woby::MeshGeometry>();
    g->vertices.resize(3); g->vertices[1].position = {1, 0, 0}; g->vertices[2].position = {0, 1, 0};
    g->indices = {0, 1, 2}; g->nodes = {{"surface", 0, 3}};
    SUBCASE("precise array too short") { g->precisePositions.resize(1); }
    SUBCASE("identity source array too short") {
        g->sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(1);
    }
    SUBCASE("mapped source index outside array") {
        g->sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(3);
        g->positionIndexing = woby::PositionIndexing::mapped; g->positionIndices = {0, 5, 2};
    }
    SUBCASE("position map too short") {
        g->sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(3);
        g->positionIndexing = woby::PositionIndexing::mapped; g->positionIndices = {0};
    }
    woby::Mesh mesh; mesh.storage = g;
    CHECK_THROWS_AS((void)woby::buildAnnotationMeshCache(mesh), std::out_of_range);
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Point key preparation retains source IDs positions groups and equal key order")
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    constexpr uint32_t count = 300003;
    g.vertices.resize(count); g.pointIndices.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        g.pointIndices[i] = count - 1 - i;
        g.vertices[i].position = {float(i % 7), float(i % 11), float(i % 13)};
    }
    g.nodes = {{"first"}, {"second"}};
    g.nodes[0].pointIndexCount = 150001;
    g.nodes[1].pointIndexOffset = 150001; g.nodes[1].pointIndexCount = count - 150001;
    mesh.bounds = woby::calculateBounds(g.vertices);
    woby::publishMeshGeometry(mesh);
    woby::points::Cloud serial;
    { ReservedWorkers reserved; serial = woby::points::buildCloud(mesh); }
    const auto parallel = woby::points::buildCloud(mesh);
    CHECK(parallel.sourceVertices == serial.sourceVertices);
    CHECK(parallel.groups == serial.groups);
    CHECK(parallel.roots == serial.roots);
    REQUIRE(parallel.points.size() == serial.points.size());
    for (size_t i = 0; i < parallel.points.size(); ++i) {
        CHECK(parallel.points[i].id == serial.points[i].id);
        CHECK(parallel.points[i].position == serial.points[i].position);
        CHECK(parallel.points[i].position == g.vertices[parallel.sourceVertices[parallel.points[i].id - 1]].position);
    }
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Overlapping post load preparation agrees with serial results and joins canceled work")
{
    auto mesh = testMesh();
    woby::captureSourceMesh(mesh, woby::SourceProvenance::importerVertices);
    auto source = std::make_shared<woby::SourceMeshData>(*mesh.sourceData);
    source->indices.clear();
    source->indicesPending = true;
    mesh.sourceData = source;
    std::optional<woby::PostLoadData> serial;
    { ReservedWorkers reserved; serial = woby::preparePostLoadData(mesh); }
    unsigned peakWorkers = 0;
    const auto parallel = woby::preparePostLoadData(mesh, [&] {
        peakWorkers = std::max(peakWorkers, woby::analysisWorkerCount.load());
        return false;
    });
    REQUIRE(serial); REQUIRE(parallel);
    REQUIRE(serial->source); REQUIRE(parallel->source);
    CHECK_FALSE(parallel->source->indicesPending);
    CHECK(parallel->source->indices == serial->source->indices);
    CHECK(parallel->source->indices == woby::meshGeometry(mesh).indices);
    CHECK(parallel->source->positions == source->positions);
    CHECK(source->indices.empty());
    CHECK(source->indicesPending);
    CHECK(peakWorkers <= woby::analysisWorkerLimit());
    CHECK(parallel->originalBounds == serial->originalBounds);
    REQUIRE(parallel->groups.size() == serial->groups.size());
    for (size_t i = 0; i < parallel->groups.size(); ++i) {
        CHECK(parallel->groups[i].local.min == serial->groups[i].local.min);
        CHECK(parallel->groups[i].local.max == serial->groups[i].local.max);
        CHECK(parallel->groups[i].original == serial->groups[i].original);
    }
    CHECK(parallel->points.pointVertexIndices == serial->points.pointVertexIndices);
    CHECK(parallel->annotations->fingerprints == serial->annotations->fingerprints);
    CHECK(parallel->annotations->spatial->triangles == serial->annotations->spatial->triangles);
    std::atomic<unsigned> calls = 0;
    CHECK_FALSE(woby::preparePostLoadData(mesh, [&] { return calls.fetch_add(1) > 2000; }));
    CHECK(woby::analysisWorkerCount.load() == 0);
}
