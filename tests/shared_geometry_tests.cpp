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
    CHECK(meshGeometry(copy).precisePositions.data() == meshGeometry(mesh).precisePositions.data());
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
    data.precisePositions[0] = {-0.0, 1.0e-20, 0};
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
