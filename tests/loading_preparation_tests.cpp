#include "annotation_preparation.h"
#include "annotation_ui.h"
#include "background_load.h"
#include "model_load.h"
#include "surface_annotation.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <bx/math.h>
#include <atomic>
#include <chrono>
#include <fstream>

namespace {
woby::Mesh preparationMesh()
{
    woby::Mesh mesh;
    mesh.vertices = {{{-2, -1, 0}, {0, 0, 1}, {}}, {{4, -1, 0}, {0, 0, 1}, {}}, {{0, 3, 0}, {0, 0, 1}, {}}};
    for (size_t i = 0; i < 50000; ++i) { mesh.indices.insert(mesh.indices.end(), {0, 1, 2}); }
    mesh.nodes = {{"surface", 0, static_cast<uint32_t>(mesh.indices.size())}};
    woby::finalizeMesh(mesh, false);
    return mesh;
}
woby::UiState preparationScene()
{
    woby::UiState state;
    state.files.push_back(woby::createUiFileState({}, preparationMesh(), 0));
    woby::assignSceneObjectIds(state);
    woby::recalculateSceneBounds(state);
    woby::frameCameraToScene(state);
    return state;
}
struct PreparationDirectory {
    std::filesystem::path path;
    PreparationDirectory()
    {
        static std::atomic<unsigned> sequence = 0;
        path = std::filesystem::temp_directory_path() / ("woby-preparation-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(sequence++));
        std::filesystem::create_directory(path);
    }
    ~PreparationDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
};
}

TEST_CASE("loading reports ordered stages and measured within-stage progress")
{
    const PreparationDirectory directory;
    const auto path = directory.path / "mesh.obj";
    { std::ofstream file(path); file << "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
        for (size_t i = 0; i < 20000; ++i) { file << "f 1 2 3\n"; } }
    std::vector<woby::BackgroundLoadProgress> updates;
    auto result = woby::loadModelBatchCpu({path}, 0, [&](const auto& progress) { updates.push_back(progress); }, {});
    REQUIRE(result.files.size() == 1);
    float previous = 0;
    bool intermediateMesh = false, intermediateGroups = false;
    std::vector<woby::ModelLoadStage> stages;
    for (const auto& update : updates) {
        CHECK(update.currentPath == path);
        CHECK(update.currentFileFraction >= previous);
        CHECK(update.currentFileFraction <= 1);
        previous = update.currentFileFraction;
        if (stages.empty() || stages.back() != update.model.stage) { stages.push_back(update.model.stage); }
        const bool partial = update.model.completed > 0 && update.model.completed < update.model.total;
        intermediateMesh |= partial && update.model.stage == woby::ModelLoadStage::buildingMesh;
        intermediateGroups |= partial && update.model.stage == woby::ModelLoadStage::groups;
    }
    CHECK(stages == std::vector<woby::ModelLoadStage>{woby::ModelLoadStage::reading, woby::ModelLoadStage::triangulating,
        woby::ModelLoadStage::sourcePositions, woby::ModelLoadStage::buildingMesh, woby::ModelLoadStage::normals,
        woby::ModelLoadStage::bounds, woby::ModelLoadStage::groups, woby::ModelLoadStage::ready});
    CHECK(intermediateMesh); CHECK(intermediateGroups); CHECK(previous == 1);
    bool cancel = false;
    woby::ImportCallbacks callbacks;
    callbacks.canceled = [&] { return cancel; };
    callbacks.stageProgress = [&](const auto& update) { cancel |= update.stage == woby::ModelLoadStage::buildingMesh; };
    const auto canceled = woby::loadModel(path, {}, callbacks);
    CHECK(canceled.canceled); CHECK(canceled.mesh.vertices.empty());
}

TEST_CASE("group centers reuse bounds for populated empty and invalid ranges")
{
    auto mesh = preparationMesh();
    mesh.nodes = {{"a", 0, 3}, {"b", 3, 3}, {"empty", 0, 0}, {"past end", 150000, 0}};
    const auto groups = woby::createUiGroupStates(mesh, 2);
    REQUIRE(groups.size() == 4);
    for (size_t i = 0; i < groups.size(); ++i) {
        CHECK(groups[i].center == groups[i].localBounds.center);
        CHECK(groups[i].center == woby::nodeCenter(mesh, mesh.nodes[i]));
        CHECK(groups[i].color == woby::defaultGroupColor(i + 2));
    }
}

TEST_CASE("annotation cache preparation publishes only completed immutable results after mesh moves")
{
    auto state = preparationScene();
    auto& mesh = state.files[0].mesh;
    REQUIRE_FALSE(mesh.annotationCache);
    const auto fingerprint = woby::annotationFingerprint(mesh, 0, mesh.indices.size());
    const auto* vertices = mesh.vertices.data();
    woby::AnnotationPreparationRuntime runtime;
    woby::updateAnnotationPreparation(runtime, state);
    REQUIRE(runtime.job);
    CHECK_FALSE(mesh.annotationCache); // Even a fast worker cannot publish into the scene.
    state.files.reserve(10); // Move containing Mesh without moving its buffers.
    runtime.job->worker.join();
    woby::updateAnnotationPreparation(runtime, state);
    REQUIRE(state.files[0].mesh.annotationCache);
    CHECK(state.files[0].mesh.annotationCache->vertexData == vertices);
    CHECK(state.files[0].mesh.annotationCache->fingerprints[0] == fingerprint);
    CHECK(woby::annotationPreparationReady(state));
    CHECK_FALSE(runtime.job);
    // Copying geometry invalidates pointer-bound caches; it must be prepared anew.
    const auto copy = state.files[0].mesh;
    CHECK_FALSE(woby::annotationMeshCacheReady(copy));
}

TEST_CASE("annotation cache preparation cancellation releases borrowed geometry safely")
{
    auto state = preparationScene();
    woby::AnnotationPreparationRuntime runtime;
    woby::updateAnnotationPreparation(runtime, state);
    REQUIRE(runtime.job);
    woby::cancelAnnotationPreparation(runtime);
    CHECK_FALSE(runtime.job); CHECK_FALSE(state.files[0].mesh.annotationCache);
    state.files.clear();
    state = preparationScene();
    woby::updateAnnotationPreparation(runtime, state);
    REQUIRE(runtime.job);
    runtime.job->worker.join();
    woby::updateAnnotationPreparation(runtime, state);
    CHECK(woby::annotationPreparationReady(state));
    size_t checks = 0;
    const auto& mesh = state.files[0].mesh;
    CHECK_FALSE(woby::buildAnnotationMeshCache(mesh.vertices, mesh.indices, mesh.nodes, [&] { return ++checks >= 3; }));
    CHECK(checks == 3);
}

TEST_CASE("saved annotation target validation waits for background fingerprints")
{
    auto state = preparationScene();
    woby::UiAnnotation item;
    item.targetId = state.files[0].groupSettings[0].objectId;
    item.geometry.fingerprint = woby::annotationFingerprint(state.files[0].mesh, 0, state.files[0].mesh.indices.size());
    state.annotations.push_back(item);
    woby::validateAnnotationTargets(state);
    CHECK(state.annotations[0].targetPending); CHECK_FALSE(state.annotations[0].targetValid);
    SUBCASE("removing a pending source does not leave annotations waiting forever") {
        REQUIRE(woby::removeFileFromState(state, 0));
        CHECK_FALSE(state.annotations[0].targetPending);
        CHECK_FALSE(state.annotations[0].targetValid);
        CHECK(woby::annotationPreparationReady(state));
        return;
    }
    SUBCASE("publish and validate") {}
    woby::AnnotationPreparationRuntime runtime;
    woby::updateAnnotationPreparation(runtime, state);
    REQUIRE(runtime.job);
    runtime.job->worker.join();
    woby::updateAnnotationPreparation(runtime, state);
    CHECK_FALSE(state.annotations[0].targetPending); CHECK(state.annotations[0].targetValid);
    state.annotations[0].geometry.fingerprint = "changed";
    woby::validateAnnotationTargets(state);
    CHECK_FALSE(state.annotations[0].targetValid);
}

TEST_CASE("annotation gesture waits without projection and cancels stale pending input")
{
    auto state = preparationScene();
    woby::ScenePickView view;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    view.width = view.height = 200;
    woby::AnnotationInteraction interaction;
    interaction.tool = woby::AnnotationShape::line;
    CHECK(woby::beginAnnotationPointer(state, interaction, view, {80, 100}));
    CHECK(interaction.waitingForPreparation); CHECK_FALSE(interaction.dragging);
    CHECK(interaction.projection.triangles.empty());
    woby::moveAnnotationPointer(state, interaction, {120, 100});
    woby::endAnnotationPointer(state, interaction, true);
    CHECK(interaction.waitingReleased);
    CHECK(interaction.pointerEnd == woby::PickPoint{120, 100});
    SUBCASE("resume when ready") {
        woby::prepareAnnotationMeshCache(state.files[0].mesh);
        woby::resumeAnnotationPointer(state, interaction);
        CHECK_FALSE(interaction.waitingForPreparation);
        CHECK_FALSE(interaction.dragging);
        CHECK(interaction.error.empty());
        CHECK(state.annotations.size() == 1);
    }
    SUBCASE("changed scene cancels") {
        ++state.sceneGeneration;
        woby::resumeAnnotationPointer(state, interaction);
        CHECK_FALSE(interaction.waitingForPreparation);
        CHECK_FALSE(interaction.error.empty()); CHECK(state.annotations.empty());
    }
    SUBCASE("escape cancels") {
        woby::cancelAnnotationPointer(interaction);
        CHECK_FALSE(interaction.waitingForPreparation); CHECK_FALSE(interaction.tool);
    }
}
