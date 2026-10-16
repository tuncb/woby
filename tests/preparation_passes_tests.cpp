#include "model_mesh.h"
#include "point_cloud.h"
#include "scene_mesh_preparation.h"
#include "parallel_work.h"
#include "obj_mesh.h"
#include "simd_bounds.h"

#include <doctest/doctest.h>
#include <future>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <tuple>

namespace {
woby::Mesh indexedMesh(size_t count = 600003)
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    g.vertices = {{{-2, 0, 1}, {}, {}}, {{4, -3, 0}, {}, {}}, {{1, 5, 2}, {}, {}}};
    g.indices.resize(count);
    for (size_t i = 0; i < count; ++i) { g.indices[i] = static_cast<uint32_t>(i % 3); }
    g.nodes = {{"all", 0, static_cast<uint32_t>(count)}};
    woby::finalizeMesh(mesh, false);
    woby::publishMeshGeometry(mesh);
    return mesh;
}
}

TEST_CASE("Immutable preparation validation is shared and geometry edits invalidate it")
{
    auto mesh = indexedMesh();
    const auto snapshot = mesh;
    REQUIRE(mesh.preparationCache);
    CHECK(snapshot.preparationCache == mesh.preparationCache);
    size_t firstPolls = 0, secondPolls = 0;
    REQUIRE(woby::prepareSceneMesh(mesh, 0, [&] { ++firstPolls; return false; }, false, false));
    CHECK(mesh.preparationCache->indicesValidated.load());
    REQUIRE(woby::prepareSceneMesh(snapshot, 0, [&] { ++secondPolls; return false; }, false, false));
    CHECK(secondPolls < firstPolls);
    CHECK_FALSE(woby::prepareSceneMesh(snapshot, 0, [] { return true; }, false, false));
    SUBCASE("triangle") { woby::editMeshGeometry(mesh).indices.back() = 3; }
    SUBCASE("line") { woby::editMeshGeometry(mesh).lineIndices = {0, 3}; }
    SUBCASE("point") { woby::editMeshGeometry(mesh).pointIndices = {3}; }
    SUBCASE("vertex count") { woby::editMeshGeometry(mesh).vertices.pop_back(); }
    CHECK_FALSE(mesh.preparationCache);
    CHECK_THROWS_AS((void)woby::prepareSceneMesh(mesh, 0, {}, false, false), std::runtime_error);
    woby::publishMeshGeometry(mesh);
    CHECK(mesh.preparationCache != snapshot.preparationCache);
    CHECK_THROWS_AS((void)woby::prepareSceneMesh(mesh, 0, {}, false, false), std::runtime_error);
    CHECK(woby::prepareSceneMesh(snapshot, 0, {}, false, false).has_value());
}

TEST_CASE("Canceled index scans do not publish validation and concurrent readers are safe")
{
    const auto mesh = indexedMesh();
    size_t polls = 0;
    CHECK_FALSE(woby::prepareSceneMesh(mesh, 0, [&] { return ++polls == 4; }, false, false));
    CHECK_FALSE(mesh.preparationCache->indicesValidated.load());
    auto worker = std::async(std::launch::async, [mesh] { return woby::prepareSceneMesh(mesh, 0, {}, false, false); });
    REQUIRE(woby::prepareSceneMesh(mesh, 0, {}, false, false));
    REQUIRE(worker.get());
    CHECK(mesh.preparationCache->indicesValidated.load());
}

TEST_CASE("Bulk original bounds preserve storage precision and exclude unused source positions")
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    g.vertices.resize(3);
    const std::vector<woby::Coordinate> points{{-0.0, 1.0e-20, 0}, {2.0000000001, -3, 4}, {-1, 5, -6},
        {std::numeric_limits<double>::quiet_NaN(), 1.0e100, 1.0e100}};
    mesh.origin = {8000000, -9000000, 1000000};
    SUBCASE("floats") {
        for (size_t i = 0; i < g.vertices.size(); ++i) { g.vertices[i].position = woby::renderPosition(points[i]); }
    }
    SUBCASE("direct doubles") { g.precisePositions.assign(points.begin(), points.begin() + 3); }
    SUBCASE("identity source") { g.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(points); }
    SUBCASE("mapped source") {
        g.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(points);
        g.positionIndexing = woby::PositionIndexing::mapped;
        g.positionIndices = {2, 0, 1};
    }
    std::array<woby::Coordinate, 2> expected{};
    for (size_t i = 0; i < g.vertices.size(); ++i) {
        const auto p = woby::originalPosition(woby::meshPosition(mesh, i), mesh.origin);
        if (!i) { expected = {p, p}; }
        else { for (size_t k = 0; k < 3; ++k) {
            expected[0][k] = std::min(expected[0][k], p[k]);
            expected[1][k] = std::max(expected[1][k], p[k]);
        } }
    }
    const auto result = woby::prepareOriginalMeshBounds(mesh);
    REQUIRE(result);
    CHECK(*result == expected);
}

TEST_CASE("Bulk original bounds retain finite checks checked accesses and cancellation")
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    g.vertices.resize(65539);
    g.precisePositions.resize(g.vertices.size());
    SUBCASE("nonfinite first") { g.precisePositions.front()[0] = std::numeric_limits<double>::quiet_NaN(); }
    SUBCASE("nonfinite tail") { g.precisePositions.back()[2] = std::numeric_limits<double>::infinity(); }
    SUBCASE("original addition overflow") {
        mesh.origin[1] = std::numeric_limits<double>::max();
        g.precisePositions.back()[1] = std::numeric_limits<double>::max();
    }
    SUBCASE("short direct storage") {
        g.precisePositions.pop_back();
        CHECK_THROWS_AS((void)woby::prepareOriginalMeshBounds(mesh), std::out_of_range);
        return;
    }
    SUBCASE("invalid mapped source ID") {
        g.precisePositions.clear();
        g.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(3);
        g.positionIndexing = woby::PositionIndexing::mapped;
        g.positionIndices.assign(g.vertices.size(), 0);
        g.positionIndices.back() = 3;
        CHECK_THROWS_AS((void)woby::prepareOriginalMeshBounds(mesh), std::out_of_range);
        return;
    }
    SUBCASE("cancel during reduction") {
        size_t polls = 0;
        CHECK_FALSE(woby::prepareOriginalMeshBounds(mesh, [&] { return ++polls == 3; }));
        CHECK(polls == 3);
        return;
    }
    CHECK_THROWS_AS((void)woby::prepareOriginalMeshBounds(mesh), std::runtime_error);
}

TEST_CASE("Bulk source mapping matches serial output for identity and seam mappings")
{
    auto mesh = indexedMesh();
    auto& g = woby::editMeshGeometry(mesh);
    auto source = std::make_shared<woby::SourceMeshData>();
    source->positions = std::make_shared<const std::vector<woby::Coordinate>>(4);
    mesh.sourceData = source;
    SUBCASE("identity") {}
    SUBCASE("seams and permuted source IDs") {
        g.positionIndexing = woby::PositionIndexing::mapped;
        g.positionIndices = {3, 0, 3};
    }
    const auto serial = woby::mapSourceTriangleIndices(mesh, {}, false);
    const auto parallel = woby::mapSourceTriangleIndices(mesh);
    REQUIRE(serial);
    REQUIRE(parallel);
    CHECK(*parallel == *serial);
    std::vector<uint32_t> expected;
    for (const auto vertex : g.indices) {
        expected.push_back(g.positionIndexing == woby::PositionIndexing::identity ? vertex : g.positionIndices[vertex]);
    }
    CHECK(*parallel == expected);
    size_t polls = 0;
    CHECK_FALSE(woby::mapSourceTriangleIndices(mesh, [&] { return ++polls == 4; }));
    CHECK(polls == 4);
    CHECK(woby::analysisWorkerCount.load() == 0);
    REQUIRE(woby::mapSourceTriangleIndices(mesh));
}

TEST_CASE("Bulk source mapping rejects invalid vertices source IDs and short maps")
{
    auto mesh = indexedMesh();
    auto& g = woby::editMeshGeometry(mesh);
    auto source = std::make_shared<woby::SourceMeshData>();
    source->positions = std::make_shared<const std::vector<woby::Coordinate>>(3);
    mesh.sourceData = source;
    SUBCASE("render vertex") { g.indices.back() = 3; }
    SUBCASE("identity source too short") { source->positions = std::make_shared<const std::vector<woby::Coordinate>>(2); }
    SUBCASE("mapped source") { g.positionIndexing = woby::PositionIndexing::mapped; g.positionIndices = {0, 1, 3}; }
    SUBCASE("short map") {
        g.positionIndexing = woby::PositionIndexing::mapped; g.positionIndices = {0, 1};
        CHECK_THROWS_AS((void)woby::mapSourceTriangleIndices(mesh), std::out_of_range);
        return;
    }
    CHECK_THROWS_AS((void)woby::mapSourceTriangleIndices(mesh), std::runtime_error);
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Point bounds reuse needs a proven complete group and current validated bounds")
{
    auto mesh = indexedMesh(6);
    REQUIRE(woby::validatedMeshBounds(mesh));
    auto ranges = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
    REQUIRE(ranges);
    REQUIRE(ranges->nodeRanges[0].completeVertexSet);
    const auto reused = woby::points::buildCloud(mesh, ranges->pointVertexIndices, ranges->nodeRanges, {}, 2, 1);
    REQUIRE(reused);
    auto uncached = mesh;
    (void)woby::editMeshGeometry(uncached);
    CHECK_FALSE(woby::validatedMeshBounds(uncached));
    woby::publishMeshGeometry(uncached);
    CHECK_FALSE(woby::validatedMeshBounds(uncached));
    const auto scanned = woby::points::buildCloud(uncached, ranges->pointVertexIndices, ranges->nodeRanges, {}, 2, 1);
    REQUIRE(scanned);
    REQUIRE(reused->points.size() == scanned->points.size());
    for (size_t i = 0; i < reused->points.size(); ++i) {
        CHECK(reused->points[i].id == scanned->points[i].id);
        CHECK(reused->points[i].position == scanned->points[i].position);
    }
    auto& g = woby::editMeshGeometry(mesh);
    g.indices = {0, 1, 1, 0, 0, 1}; // A whole index range still omits one vertex.
    ranges = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
    REQUIRE(ranges);
    CHECK_FALSE(ranges->nodeRanges[0].completeVertexSet);
    const auto partial = woby::points::buildCloud(mesh, ranges->pointVertexIndices, ranges->nodeRanges, {}, 2, 1);
    REQUIRE(partial);
    CHECK(partial->nodes[partial->roots[0]].high[1] == 0);
    g.vertices[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS_AS((void)woby::points::buildCloud(mesh, ranges->pointVertexIndices, ranges->nodeRanges, {}, 2, 1), std::invalid_argument);
    CHECK_THROWS_AS((void)woby::calculateBounds(g.vertices), std::runtime_error);
}

TEST_CASE("Compact point membership retains first use at word boundaries and across groups")
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    g.vertices.resize(130);
    const std::vector<uint32_t> indices{129, 64, 63, 0, 64, 1, 129, 65, 63, 128, 127, 0};
    g.nodes = {{"first"}};
    SUBCASE("triangles") { g.indices = indices; g.nodes[0].indexCount = static_cast<uint32_t>(indices.size()); }
    SUBCASE("lines") { g.lineIndices = indices; g.nodes[0].lineIndexCount = static_cast<uint32_t>(indices.size()); }
    SUBCASE("points") { g.pointIndices = indices; g.nodes[0].pointIndexCount = static_cast<uint32_t>(indices.size()); }
    SUBCASE("overlapping groups and empty group") {
        g.pointIndices = indices;
        g.nodes[0].pointIndexCount = static_cast<uint32_t>(indices.size());
        g.nodes.push_back({"empty"});
        auto second = g.nodes[0]; second.name = "second";
        second.pointIndexOffset = 2; second.pointIndexCount -= 2;
        g.nodes.push_back(second);
    }
    std::vector<uint32_t> expected;
    std::vector<uint32_t> counts;
    for (const auto& node : g.nodes) {
        const auto first = expected.size();
        std::vector<bool> visited(g.vertices.size());
        for (const auto vertex : woby::meshNodeIndices(mesh, node)) {
            if (!visited[vertex]) { visited[vertex] = true; expected.push_back(vertex); }
        }
        counts.push_back(static_cast<uint32_t>(expected.size() - first));
    }
    const auto prepared = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
    REQUIRE(prepared);
    CHECK(prepared->pointVertexIndices == expected);
    uint32_t offset = 0;
    for (size_t i = 0; i < counts.size(); ++i) {
        CHECK(prepared->nodeRanges[i].pointIndexOffset == offset);
        CHECK(prepared->nodeRanges[i].pointIndexCount == counts[i]);
        CHECK_FALSE(prepared->nodeRanges[i].completeVertexSet);
        offset += counts[i];
    }
    const auto omitted = woby::prepareSceneMesh(mesh, 0, {}, false, false);
    REQUIRE(omitted);
    CHECK(omitted->pointVertexIndices.empty());
}

TEST_CASE("Complete membership validates trailing indices and cancellation before skipping duplicates")
{
    auto mesh = indexedMesh();
    const auto prepared = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
    REQUIRE(prepared);
    CHECK(prepared->pointVertexIndices == std::vector<uint32_t>{0, 1, 2});
    CHECK(prepared->nodeRanges[0].completeVertexSet);
    SUBCASE("invalid tail") {
        woby::editMeshGeometry(mesh).indices.back() = 3;
        CHECK_THROWS_AS((void)woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false), std::runtime_error);
    }
    SUBCASE("invalid primitive range") {
        woby::editMeshGeometry(mesh).nodes[0].indexCount += 3;
        CHECK_THROWS_AS((void)woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false), std::runtime_error);
    }
    SUBCASE("cancel after cached validation") {
        size_t calls = 0;
        CHECK_FALSE(woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, [&] { return ++calls == 3; }, false));
        CHECK(calls == 3);
    }
}

TEST_CASE("Ordered membership prefix falls back without losing visited bits or first-use order")
{
    for (const uint32_t prefix : {0u, 1u, 63u, 64u, 65u, 128u}) {
        woby::Mesh mesh;
        auto& g = woby::editMeshGeometry(mesh);
        g.vertices.resize(130);
        for (uint32_t i = 0; i < prefix; ++i) { g.pointIndices.push_back(i); g.pointIndices.push_back(i); }
        for (uint32_t i = 130; i-- > prefix;) { g.pointIndices.push_back(i); }
        for (uint32_t i = 0; i < 130; ++i) { g.pointIndices.push_back(i); }
        g.nodes = {{"points"}}; g.nodes[0].pointIndexCount = static_cast<uint32_t>(g.pointIndices.size());
        std::vector<uint32_t> expected;
        for (uint32_t i = 0; i < prefix; ++i) { expected.push_back(i); }
        for (uint32_t i = 130; i-- > prefix;) { expected.push_back(i); }
        const auto result = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
        REQUIRE(result); CHECK(result->pointVertexIndices == expected);
        CHECK(result->nodeRanges[0].completeVertexSet);
        // A proven prefix can also be the complete membership of a partial group.
        g.nodes[0].pointIndexCount = prefix * 2;
        const auto partial = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
        REQUIRE(partial);
        expected.resize(prefix);
        CHECK(partial->pointVertexIndices == expected);
        CHECK_FALSE(partial->nodeRanges[0].completeVertexSet);
    }
}

TEST_CASE("Parallel point gathering preserves complete cloud output and stable color IDs")
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    constexpr uint32_t count = 196613;
    g.vertices.resize(count);
    g.vertexColors.resize(count);
    g.pointIndices.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        // Repeated Morton keys and reverse membership expose unstable ties and
        // accidental use of the spatial offset as a color or original ID.
        g.vertices[i].position = {float(i % 97), float(i % 31), float(i % 13)};
        g.vertexColors[i] = 0xff000000u | i;
        g.pointIndices[i] = count - i - 1;
    }
    g.nodes = {{"first"}, {"empty"}, {"overlap"}};
    g.nodes[0].pointIndexCount = count;
    g.nodes[2].pointIndexOffset = 17; g.nodes[2].pointIndexCount = count - 31;
    SUBCASE("RGB") {}
    SUBCASE("without colors") { g.vertexColors.clear(); }
    const auto prepared = woby::prepareSceneMesh(mesh, woby::gpuMeshPoints, {}, false);
    REQUIRE(prepared);
    std::optional<woby::points::Cloud> serial;
    {
        struct ReservedHelpers {
            unsigned previous = woby::analysisWorkerCount.exchange(woby::analysisWorkerLimit());
            ~ReservedHelpers() { woby::analysisWorkerCount.store(previous); }
        } reserved;
        serial = woby::points::buildCloud(mesh, prepared->pointVertexIndices, prepared->nodeRanges, {});
    }
    const auto parallel = woby::points::buildCloud(mesh, prepared->pointVertexIndices, prepared->nodeRanges, {});
    REQUIRE(serial); REQUIRE(parallel);
    CHECK(woby::analysisWorkerCount.load() == 0);
    CHECK(parallel->sourceVertices == serial->sourceVertices);
    CHECK(parallel->groups == serial->groups);
    CHECK(parallel->colors == serial->colors);
    CHECK(parallel->roots == serial->roots);
    REQUIRE(parallel->points.size() == serial->points.size());
    REQUIRE(parallel->proxies.size() == serial->proxies.size());
    for (const auto vectors : {std::pair{&parallel->points, &serial->points}, std::pair{&parallel->proxies, &serial->proxies}}) {
        for (size_t i = 0; i < vectors.first->size(); ++i) {
            const auto& a = (*vectors.first)[i]; const auto& b = (*vectors.second)[i];
            CHECK(a.id == b.id); CHECK(a.position == b.position);
            const auto vertex = parallel->sourceVertices[a.id - 1];
            CHECK(a.position == g.vertices[vertex].position);
            if (!g.vertexColors.empty()) { CHECK(parallel->colors[a.id - 1] == g.vertexColors[vertex]); }
        }
    }
    REQUIRE(parallel->nodes.size() == serial->nodes.size());
    for (size_t i = 0; i < parallel->nodes.size(); ++i) {
        const auto& a = parallel->nodes[i]; const auto& b = serial->nodes[i];
        CHECK(a.low == b.low); CHECK(a.high == b.high); CHECK(a.extrema == b.extrema);
        CHECK(std::tie(a.begin, a.count, a.left, a.right, a.proxyBegin, a.proxyCount, a.group)
            == std::tie(b.begin, b.count, b.left, b.right, b.proxyBegin, b.proxyCount, b.group));
    }
    REQUIRE(parallel->navigationCuts.size() == serial->navigationCuts.size());
    for (size_t level = 0; level < parallel->navigationCuts.size(); ++level) {
        const auto& a = parallel->navigationCuts[level]; const auto& b = serial->navigationCuts[level];
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i) { CHECK(a[i].node == b[i].node); CHECK(a[i].full == b[i].full); }
    }
}

TEST_CASE("Parallel point gathering joins workers on invalid input cancellation and callback exceptions")
{
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    constexpr uint32_t count = 196613;
    g.vertices.resize(count); g.vertexColors.resize(count, 0xffabcdef);
    std::vector<uint32_t> sources(count);
    std::iota(sources.begin(), sources.end(), 0u);
    woby::GpuNodeRange range; range.pointIndexCount = count;
    SUBCASE("invalid color source in final batch") {
        sources.back() = count;
        CHECK_THROWS_AS((void)woby::points::buildCloud(mesh, sources, {&range, 1}, {}), std::invalid_argument);
    }
    SUBCASE("invalid uncolored source") {
        g.vertexColors.clear(); sources.back() = count;
        CHECK_THROWS_AS((void)woby::points::buildCloud(mesh, sources, {&range, 1}, {}), std::invalid_argument);
    }
    SUBCASE("canceled parallel color pass") {
        size_t calls = 0;
        CHECK_FALSE(woby::points::buildCloud(mesh, sources, {&range, 1}, [&] { return ++calls == 5; }));
        CHECK(calls == 5);
    }
    SUBCASE("callback throws on a worker") {
        const auto caller = std::this_thread::get_id();
        if (woby::analysisWorkerLimit()) {
            CHECK_THROWS_WITH((void)woby::points::buildCloud(mesh, sources, {&range, 1}, [&] {
                if (std::this_thread::get_id() != caller) { throw std::runtime_error("point callback failed"); }
                return false;
            }), "point callback failed");
        }
    }
    CHECK(woby::analysisWorkerCount.load() == 0);
    sources.back() = count - 1;
    REQUIRE(woby::points::buildCloud(mesh, sources, {&range, 1}, {}));
}

TEST_CASE("Point key sort and gather passes leave helpers for concurrent annotations")
{
    const auto limit = woby::analysisWorkerLimit();
    if (limit <= 3) { return; }
    woby::Mesh mesh;
    auto& g = woby::editMeshGeometry(mesh);
    constexpr uint32_t count = 8 * 65536 + 7;
    g.vertices.resize(count); g.vertexColors.resize(count, 0xffabcdef);
    std::vector<uint32_t> sources(count);
    std::iota(sources.begin(), sources.end(), 0u);
    woby::GpuNodeRange range; range.pointIndexCount = count;
    unsigned peak = 0;
    {
        // The post-load coordinator and fingerprint job occupy two helpers.
        struct ReservedHelpers {
            unsigned previous = woby::analysisWorkerCount.exchange(2);
            ~ReservedHelpers() { woby::analysisWorkerCount.store(previous); }
        } reserved;
        const auto cloud = woby::points::buildCloud(mesh, sources, {&range, 1}, [&] {
            // buildCloud serializes these callbacks even when workers call them.
            peak = std::max(peak, woby::analysisWorkerCount.load());
            return false;
        });
        REQUIRE(cloud);
        CHECK(cloud->points.size() == count);
        CHECK(woby::analysisWorkerCount.load() == 2);
    }
    CHECK(peak <= 2 + (limit - 2) / 2);
    CHECK(woby::analysisWorkerCount.load() == 0);
}

TEST_CASE("Preparation pass benchmark" * doctest::skip())
{
    woby::Mesh mesh;
#ifdef _WIN32
    wchar_t* model = nullptr;
    size_t modelLength = 0;
    REQUIRE(_wdupenv_s(&model, &modelLength, L"WOBY_PREPARATION_BENCHMARK_MODEL") == 0);
    const std::unique_ptr<wchar_t, decltype(&std::free)> modelOwner(model, &std::free);
#else
    const auto* model = std::getenv("WOBY_PREPARATION_BENCHMARK_MODEL");
#endif
    if (model) {
        woby::ObjPrototypeOptions options; options.deferPreparation = true;
        mesh = woby::loadObjMeshPrototype(std::filesystem::path(model), {}, options);
    } else {
        mesh = indexedMesh(24000003);
        woby::captureSourceMesh(mesh, woby::SourceProvenance::importerVertices);
        woby::publishMeshGeometry(mesh);
    }
    const auto& g = woby::meshGeometry(mesh);
    const auto canceled = [] { return false; };
    std::cout << "vertices=" << g.vertices.size() << ",corners=" << g.indices.size() << '\n';
    std::cout << "round,pass,milliseconds\n";
    const auto measure = [&](size_t round, const char* name, const auto& work) {
        const auto start = std::chrono::steady_clock::now();
        auto result = work();
        const auto end = std::chrono::steady_clock::now();
        std::cout << round << ',' << name << ',' << std::chrono::duration<double, std::milli>(end-start).count() << '\n';
        return result;
    };
    // Include allocation and page faults. Rotate modes to reduce order bias.
    for (size_t round = 0; round < 4; ++round) {
        mesh.preparationCache->indicesValidated.store(false);
        REQUIRE(measure(round, "prepare_initial", [&] { return woby::prepareSceneMesh(mesh, 0, canceled, false, false); }));
        REQUIRE(measure(round, "prepare_cached", [&] { return woby::prepareSceneMesh(mesh, 0, canceled, false, false); }));
        std::vector<uint32_t> expected;
        for (size_t mode = 0; mode < 3; ++mode) {
            const auto selected = (round + mode) % 3;
            const auto name = selected == 0 ? "source_push_back" : selected == 1 ? "source_bulk_serial" : "source_bulk_parallel";
            auto output = measure(round, name, [&] {
                if (selected) { return *woby::mapSourceTriangleIndices(mesh, canceled, selected == 2); }
                std::vector<uint32_t> indices;
                indices.reserve(g.indices.size());
                for (size_t i = 0; i < g.indices.size(); ++i) {
                    if (i % 65536 == 0 && canceled()) { throw std::runtime_error("Canceled benchmark."); }
                    const auto vertex = g.indices[i];
                    if (vertex >= g.vertices.size()) { throw std::runtime_error("Invalid source triangle vertex."); }
                    const auto position = g.positionIndexing == woby::PositionIndexing::identity ? vertex : g.positionIndices.at(vertex);
                    if (position >= woby::sourcePositions(*mesh.sourceData).size()) { throw std::runtime_error("Invalid source triangle position."); }
                    indices.push_back(position);
                }
                return indices;
            });
            if (mode == 0) { expected = std::move(output); }
            else { CHECK(output == expected); }
        }
        const auto expectedBounds = measure(round, "original_accessor", [&] {
            auto reduction = woby::doubleBoundsReduction({});
            for (size_t i = 0; i < g.vertices.size(); ++i) {
                if (i % 65536 == 0 && canceled()) { throw std::runtime_error("Canceled benchmark."); }
                const auto p = woby::originalPosition(woby::meshPosition(mesh, i), mesh.origin);
                if (!woby::finiteCoordinate(p)) { throw std::runtime_error("Non-finite original coordinate."); }
                if (!i) { reduction = woby::doubleBoundsReduction(p); }
                else { woby::expand(reduction, p); }
            }
            return woby::boundsResult(reduction);
        });
        const auto bounds = measure(round, "original_bulk", [&] { return woby::prepareOriginalMeshBounds(mesh, canceled); });
        REQUIRE(bounds);
        CHECK(*bounds == expectedBounds);
    }
}
