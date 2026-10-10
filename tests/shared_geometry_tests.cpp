#include "model_mesh.h"
#include "obj_mesh.h"
#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "annotation_preparation.h"
#include "surface_annotation.h"
#include "scene_history.h"
#include "ui_operations.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <cmath>
#include <type_traits>

namespace {
woby::Mesh triangle()
{
    return woby::loadObjMeshText("o part\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
}
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby-sharing-" + woby::automationRandomHex(8));
    woby::UiState state;
    Fixture()
    {
        std::filesystem::create_directory(root);
        state.files.push_back(woby::createUiFileState(root / "model.obj", triangle(), 0));
        woby::appendFolderTreeSceneNode(state, root, 0, 1);
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
}

TEST_CASE("published geometry shares arrays and isolates later edits")
{
    auto mesh = triangle();
    REQUIRE(mesh.storage);
    static_assert(std::is_const_v<std::remove_reference_t<decltype(meshGeometry(mesh))>>);
    auto copy = mesh;
    CHECK(copy.storage == mesh.storage);
    CHECK(meshGeometry(copy).vertices.data() == meshGeometry(mesh).vertices.data());
    CHECK(meshGeometry(copy).indices.data() == meshGeometry(mesh).indices.data());
    CHECK(meshGeometry(copy).sourcePositions == meshGeometry(mesh).sourcePositions);
    CHECK(meshGeometry(copy).nodes.data() == meshGeometry(mesh).nodes.data());
    const auto revision = copy.contentRevision;
    editMeshGeometry(copy).vertices[0].position[0] = 7;
    CHECK(copy.contentRevision != revision);
    CHECK(meshGeometry(mesh).vertices[0].position[0] == 0);
    CHECK(meshGeometry(copy).vertices[0].position[0] == 7);
    woby::publishMeshGeometry(copy);
    CHECK(copy.storage != mesh.storage);
    CHECK_FALSE(copy.draft);
}

TEST_CASE("unfinished geometry copies do not share mutable buffers")
{
    woby::Mesh mesh;
    auto& geometry = editMeshGeometry(mesh);
    geometry.vertices.resize(1);
    auto copy = mesh;
    geometry.vertices[0].position[0] = 3;
    CHECK(meshGeometry(copy).vertices[0].position[0] == 0);
    CHECK(meshGeometry(copy).vertices.data() != meshGeometry(mesh).vertices.data());
}

TEST_CASE("shared geometry keeps mixed primitives hierarchy batches and exact positions")
{
    auto mesh = triangle();
    auto& data = editMeshGeometry(mesh);
    woby::editMeshPositions(mesh)[0] = {-0.0, 1.0e-20, 0};
    data.lineIndices = {0, 1};
    data.pointIndices = {2};
    data.hierarchy = {{"root", UINT32_MAX, UINT32_MAX}, {"part", 0, 0}};
    data.analysisVertexBatches = {4, 4, 4};
    woby::publishMeshGeometry(mesh);
    const auto copy = mesh;
    const auto& original = meshGeometry(mesh);
    const auto& shared = meshGeometry(copy);
    CHECK(shared.lineIndices.data() == original.lineIndices.data());
    CHECK(shared.pointIndices.data() == original.pointIndices.data());
    CHECK(shared.hierarchy.data() == original.hierarchy.data());
    CHECK(shared.analysisVertexBatches.data() == original.analysisVertexBatches.data());
    CHECK(std::signbit(woby::meshPosition(copy, 0)[0]));
    CHECK(woby::meshPosition(copy, 0)[1] == 1.0e-20);
    const auto owner = mesh.storage;
    woby::publishMeshGeometry(mesh);
    CHECK(mesh.storage == owner);
}

TEST_CASE("comparison inputs and source results retain the geometry owner")
{
    Fixture f;
    const auto id = woby::createComparison(f.state);
    woby::setComparisonObjects(f.state, {f.state.files[0].groupSettings[0].objectId}, woby::ComparisonSide::a, true, id);
    const auto owner = f.state.files[0].mesh.storage;
    const auto snapshot = woby::snapshotComparisonInputs(f.state, id);
    REQUIRE(snapshot.files.size() == 1);
    CHECK(snapshot.files[0].mesh.storage == owner);
    auto input = woby::comparisonWorldMesh(snapshot, woby::ComparisonSide::a);
    const auto result = woby::computeComparisonStages(input, {}, woby::comparisonSource);
    CHECK(result.original.source.storage == input.storage);
    const auto expected = meshGeometry(result.original.source).indices;
    input = {};
    f.state.files.clear();
    CHECK(meshGeometry(snapshot.files[0].mesh).indices == expected);
    CHECK(meshGeometry(result.original.source).indices == expected);
}

TEST_CASE("annotation caches share geometry without an ownership cycle")
{
    std::weak_ptr<const woby::MeshGeometry> geometry;
    std::weak_ptr<const woby::MeshAnnotationCache> cache;
    {
        auto mesh = triangle();
        geometry = mesh.storage;
        woby::prepareAnnotationMeshCache(mesh);
        REQUIRE(mesh.annotationCache);
        cache = mesh.annotationCache;
        REQUIRE(mesh.annotationCache->geometry);
        CHECK(mesh.annotationCache->geometry == mesh.storage);
        const auto snapshot = std::make_shared<const woby::Mesh>(mesh);
        editMeshGeometry(mesh).vertices[0].position[0] = 2;
        CHECK_FALSE(mesh.annotationCache);
        CHECK(meshGeometry(*snapshot).vertices[0].position[0] == 0);
        CHECK(woby::annotationMeshCacheReady(*snapshot));
    }
    CHECK(geometry.expired());
    CHECK(cache.expired());
}

TEST_CASE("history preparation shares live geometry but history does not retain it")
{
    Fixture f;
    woby::SceneHistory history;
    const auto clean = woby::createSceneDocument(f.state);
    woby::resetSceneHistory(history, f.state);
    f.state.files[0].groupSettings[0].translation[0] = 2;
    woby::markSceneDirty(f.state);
    REQUIRE(woby::recordSceneHistory(history, f.state));
    std::weak_ptr<const woby::MeshGeometry> geometry = f.state.files[0].mesh.storage;
    {
        auto prepared = woby::prepareSceneHistoryStep(history, f.state, clean, false);
        REQUIRE(prepared);
        CHECK(prepared->files[0].mesh.storage == f.state.files[0].mesh.storage);
        for (const auto& step : history.snapshots) {
            CHECK_FALSE(step.content.files[0].mesh.storage);
            CHECK(meshGeometry(step.content.files[0].mesh).vertices.empty());
        }
    }
    f.state.files.clear();
    CHECK(geometry.expired());
}

TEST_CASE("annotation preparation owns removed input and rejects a late replacement result")
{
    Fixture f;
    woby::AnnotationPreparationRuntime runtime;
    const std::weak_ptr<const woby::MeshGeometry> oldStorage = f.state.files[0].mesh.storage;
    woby::updateAnnotationPreparation(runtime, f.state);
    REQUIRE(runtime.job);
    // Replace geometry without joining the worker. Keep the object identity to
    // exercise the geometry revision guard rather than only the file ID guard.
    f.state.files[0].mesh = triangle();
    const auto replacement = f.state.files[0].mesh.storage;
    REQUIRE(replacement != oldStorage.lock());
    REQUIRE_FALSE(oldStorage.expired());
    for (size_t i = 0; i < 100; ++i) { f.state.files.emplace_back(); }
    runtime.job->worker.join();
    woby::updateAnnotationPreparation(runtime, f.state);
    CHECK_FALSE(f.state.files[0].mesh.annotationCache);
    CHECK(oldStorage.expired());
    REQUIRE(runtime.job);
    runtime.job->worker.join();
    woby::updateAnnotationPreparation(runtime, f.state);
    REQUIRE(f.state.files[0].mesh.annotationCache);
    CHECK(f.state.files[0].mesh.annotationCache->geometry == replacement);
    CHECK_FALSE(runtime.job);
}

TEST_CASE("source coordinates preserve seams distinct equal positions and unused points")
{
    auto mesh = woby::loadObjMeshText("v -0 0 0\nv 1 0 0\nv 0 1 0\nv 1 0 0\nv 9 8 7\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nf 3/3 1/1 2/2\nf 3/1 4/2 1/3\nl 4 2\np 1\n");
    const auto& g = meshGeometry(mesh);
    REQUIRE(g.sourcePositions);
    CHECK(g.sourcePositions == mesh.sourceData->positions);
    CHECK(g.precisePositions.empty());
    CHECK(g.positionIndexing == woby::PositionIndexing::mapped);
    REQUIRE(g.positionIndices.size() == g.vertices.size());
    CHECK(g.sourcePositions->size() == 5);
    for (size_t i = 0; i < g.indices.size(); ++i) {
        CHECK(g.positionIndices[g.indices[i]] == mesh.sourceData->indices[i]);
    }
    CHECK(g.positionIndices[g.lineIndices[0]] == 3);
    CHECK(g.positionIndices[g.lineIndices[1]] == 1);
    CHECK(g.positionIndices[g.pointIndices[0]] == 0);
    auto copy = mesh;
    const auto source = mesh.sourceData;
    woby::rebaseMesh(copy, {1, 2, 3});
    woby::publishMeshGeometry(copy);
    CHECK(meshGeometry(copy).sourcePositions != g.sourcePositions);
    CHECK(meshGeometry(copy).sourcePositions == copy.sourceData->positions);
    CHECK(woby::sourcePositions(*copy.sourceData)[4] == woby::Coordinate{8, 6, 4});
    CHECK(woby::sourcePositions(*source)[4] == woby::Coordinate{9, 8, 7});
    for (size_t i = 0; i < g.vertices.size(); ++i) {
        CHECK(woby::originalPosition(woby::meshPosition(copy, i), copy.origin) == woby::meshPosition(mesh, i));
    }
    const auto expected = woby::meshPosition(copy, 0);
    copy.sourceData.reset();
    mesh = {};
    CHECK(woby::meshPosition(copy, 0) == expected);
    CHECK_THROWS_AS((void)woby::meshPosition(copy, meshGeometry(copy).vertices.size()), std::out_of_range);
    woby::rebaseMesh(copy, {});
    CHECK(woby::meshPosition(copy, 0) == woby::Coordinate{0, 1, 0});
}

TEST_CASE("identity source storage needs no map and arbitrary edits detach")
{
    auto mesh = triangle();
    const auto& g = meshGeometry(mesh);
    CHECK(g.positionIndexing == woby::PositionIndexing::identity);
    CHECK(g.positionIndices.empty());
    CHECK(g.precisePositions.empty());
    CHECK(g.sourcePositions == mesh.sourceData->positions);
    auto copy = mesh;
    woby::editMeshPositions(copy)[0] = {-0.0, 1e-20, 0};
    woby::publishMeshGeometry(copy);
    CHECK_FALSE(copy.sourceData);
    CHECK_FALSE(meshGeometry(copy).sourcePositions);
    CHECK(std::signbit(woby::meshPosition(copy, 0)[0]));
    CHECK(woby::meshPosition(copy, 0)[1] == 1e-20);
    CHECK(woby::meshPosition(mesh, 0) == woby::Coordinate{});
    auto data = *mesh.sourceData;
    woby::editSourcePositions(data)[0] = {4, 5, 6};
    woby::publishSourcePositions(data);
    CHECK(woby::sourcePositions(*mesh.sourceData)[0] == woby::Coordinate{});
    CHECK(woby::sourcePositions(data)[0] == woby::Coordinate{4, 5, 6});
}

TEST_CASE("publication rejects incomplete and invalid source position mappings")
{
    auto mesh = triangle();
    auto& g = editMeshGeometry(mesh);
    SUBCASE("missing source") { g.sourcePositions.reset(); g.positionIndices = {0, 1, 2}; }
    SUBCASE("identity too short") { g.vertices.resize(4); }
    SUBCASE("identity map conflict") { g.positionIndices = {0, 1, 2}; }
    SUBCASE("mapped too short") { g.positionIndexing = woby::PositionIndexing::mapped; g.positionIndices = {0}; }
    SUBCASE("mapped invalid ID") { g.positionIndexing = woby::PositionIndexing::mapped; g.positionIndices = {0, 1, 3}; }
    SUBCASE("mixed direct storage") { g.precisePositions.resize(3); }
    CHECK_THROWS_AS(woby::publishMeshGeometry(mesh), std::invalid_argument);
    CHECK(mesh.draft != nullptr);
}
