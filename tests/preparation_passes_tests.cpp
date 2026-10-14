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
