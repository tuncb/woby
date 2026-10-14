#include "spatial_sort.h"
#include "point_cloud.h"
#include "post_loading.h"
#include "surface_annotation.h"
#include <doctest/doctest.h>
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
    const auto mesh = testMesh();
    std::optional<woby::PostLoadData> serial;
    { ReservedWorkers reserved; serial = woby::preparePostLoadData(mesh); }
    const auto parallel = woby::preparePostLoadData(mesh);
    REQUIRE(serial); REQUIRE(parallel);
    CHECK(parallel->originalBounds == serial->originalBounds);
    CHECK(parallel->points.pointVertexIndices == serial->points.pointVertexIndices);
    CHECK(parallel->annotations->fingerprints == serial->annotations->fingerprints);
    CHECK(parallel->annotations->spatial->triangles == serial->annotations->spatial->triangles);
    std::atomic<unsigned> calls = 0;
    CHECK_FALSE(woby::preparePostLoadData(mesh, [&] { return calls.fetch_add(1) > 2000; }));
    CHECK(woby::analysisWorkerCount.load() == 0);
}
