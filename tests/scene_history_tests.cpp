#include "scene_history.h"
#include "scene_history_load.h"
#include "model_load.h"
#include "scene_lifecycle.h"
#include "ui_operations.h"
#include "automation_registry.h"
#include "control_scene.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <fstream>

namespace {

void writeHistoryModel(const std::filesystem::path& path, float width = 1, const char* partName = "part")
{
    std::ofstream stream(path);
    stream << "o " << partName << "\nv 0 0 0\nv " << width << " 0 0\nv 0 1 0\nf 1 2 3\n";
}

struct HistoryFixture {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / ("woby-history-" + woby::automationRandomHex(8));
    std::filesystem::path source = root / "original.obj";
    woby::UiState state;
    woby::SceneHistory history;
    woby::SceneDocument clean;

    HistoryFixture()
    {
        std::filesystem::create_directory(root);
        writeHistoryModel(source);
        state.files.push_back(woby::createUiFileState(source, woby::loadModelMesh(source), 0));
        woby::appendFolderTreeSceneNode(state, root, 0, 1);
        clean = woby::createSceneDocument(state);
        woby::resetSceneHistory(history, state);
    }

    ~HistoryFixture()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void translate(float x, uint64_t interaction = 0)
    {
        woby::setGroupTranslation(state.files[0].groupSettings[0], {x, 2.25f, -3.5f});
        woby::markSceneDirty(state);
        woby::recordSceneHistory(history, state, interaction);
    }

    void step(bool redo = false)
    {
        auto prepared = woby::loadSceneHistoryStep(history, state, clean, redo);
        REQUIRE(prepared);
        woby::commitSceneHistoryStep(history, state, std::move(*prepared), redo);
    }
};

} // namespace

TEST_CASE("analysis renaming is one undoable scene edit")
{
    HistoryFixture f;
    const auto id = woby::createComparison(f.state);
    const auto original = woby::findComparison(f.state, id)->name;
    woby::resetSceneHistory(f.history, f.state);
    woby::renameComparison(f.state, id, "Inspection result");
    REQUIRE(woby::recordSceneHistory(f.history, f.state));
    CHECK(f.history.snapshots.size() == 2);
    f.step();
    REQUIRE(woby::findComparison(f.state, id));
    CHECK(woby::findComparison(f.state, id)->name == original);
    f.step(true);
    CHECK(woby::findComparison(f.state, id)->name == "Inspection result");
    woby::renameComparison(f.state, id, "Inspection result");
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
}

TEST_CASE("surface mesh quality mode metric and limits undo and redo together")
{
    HistoryFixture f;
    const auto id = woby::createComparison(f.state);
    woby::resetSceneHistory(f.history, f.state);
    const auto original = woby::comparisonSettings(f.state, id);
    auto settings = original;
    settings.mode = woby::ComparisonMode::surfaceQuality;
    settings.quality.metric = woby::SurfaceQualityMetric::shape;
    settings.quality.onOriginal = true;
    settings.quality.maximumEnabled = true;
    settings.quality.maximumSize = 2;
    woby::setComparisonSettings(f.state, settings, id);
    REQUIRE(woby::recordSceneHistory(f.history, f.state));
    f.step();
    CHECK(woby::comparisonSettings(f.state, id) == original);
    f.step(true);
    CHECK(woby::comparisonSettings(f.state, id) == settings);
}

TEST_CASE("scene history idle and camera frames consume no edit notifications")
{
    HistoryFixture f;
    const auto revision = f.state.sceneEditRevision;
    for (int frame = 0; frame < 100; ++frame) {
        woby::orbitUiCamera(f.state, 1, 2);
        woby::panUiCamera(f.state, 1, 2, 800);
        woby::dollyUiCamera(f.state, 0.1f);
        woby::selectSceneObject(f.state, f.state.files[0].objectId);
        woby::setUiScale(f.state, 1.5f);
        woby::setViewerPaneVisible(f.state, false);
        woby::setScreenshotSettings(f.state, {});
        woby::updateSceneDirty(f.state, f.clean);
        CHECK_FALSE(woby::recordSceneHistory(f.history, f.state, 123));
    }
    CHECK(f.state.sceneEditRevision == revision);
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
    CHECK(f.history.snapshots.size() == 1);
    CHECK_FALSE(woby::canUndoScene(f.history));
}

TEST_CASE("scene history records scene operation notifications while already dirty")
{
    const std::vector<std::function<void(woby::UiState&)>> edits{
        [](auto& state) { woby::setShowGrid(state, true); },
        [](auto& state) { woby::setShowOrigin(state, true); },
        [](auto& state) { woby::setSceneUpAxis(state, woby::SceneUpAxis::y); },
        [](auto& state) { woby::setMasterVertexPointSize(state, 12); },
        [](auto& state) { woby::setSceneNodeSubtreeVisible(state, state.sceneNodes[0], false); },
        [](auto& state) { woby::setGroupVisible(state, state.files[0], state.files[0].groupSettings[0], true); },
        [](auto& state) { woby::setSceneNodeSubtreeRenderMode(state, state.sceneNodes[0], woby::UiRenderMode::triangles, true); },
        [](auto& state) {
            woby::selectSceneObject(state, state.files[0].objectId);
            woby::setSelectedObjectProperty(state, woby::UiObjectProperty::translationX, 3);
        },
        [](auto& state) { woby::setSelectedObjectProperty(state, woby::UiObjectProperty::opacity, 0.5f); },
        [](auto& state) { woby::resetSelectedObjectProperties(state, woby::UiPropertyGroup::transform); },
        [](auto& state) { woby::setSelectedObjectsVisible(state, false); },
        [](auto& state) { woby::applyInspectionPreset(state, woby::UiInspectionPreset::vertices); },
    };
    HistoryFixture f;
    for (size_t index = 0; index < edits.size(); ++index) {
        INFO("operation: ", index);
        const auto before = woby::createSceneDocument(f.state);
        edits[index](f.state);
        CHECK(woby::recordSceneHistory(f.history, f.state));
        CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
        const auto after = woby::createSceneDocument(f.state);
        CHECK(after != before);
        const auto camera = f.state.camera;
        f.step();
        CHECK(woby::sceneContentEqual(woby::createSceneDocument(f.state), before));
        CHECK(f.state.camera == camera);
        CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
        f.step(true);
        CHECK(woby::createSceneDocument(f.state) == after);
        CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
    }
    CHECK(f.history.cursor == edits.size());
}

TEST_CASE("scene history rejects notified no-ops without erasing redo or splitting a drag")
{
    HistoryFixture f;
    woby::setMasterVertexPointSize(f.state, 12);
    REQUIRE(woby::recordSceneHistory(f.history, f.state, 123));
    // An operation can notify even if clamping/batching leaves content unchanged.
    woby::markSceneDirty(f.state);
    CHECK(woby::recordSceneHistory(f.history, f.state, 123));
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state, 123));
    woby::setMasterVertexPointSize(f.state, 14);
    CHECK(woby::recordSceneHistory(f.history, f.state, 123));
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state)); // Release with no edit.
    CHECK(f.history.cursor == 1);
    f.step();
    woby::markSceneDirty(f.state);
    CHECK(woby::recordSceneHistory(f.history, f.state));
    CHECK(woby::canRedoScene(f.history));
    CHECK_FALSE(woby::canUndoScene(f.history));
    woby::updateSceneDirty(f.state, f.clean);
    CHECK_FALSE(f.state.isDirty);
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
    f.step(true);
    woby::setMasterVertexPointSize(f.state, 16);
    CHECK(woby::recordSceneHistory(f.history, f.state, 123));
    CHECK(f.history.cursor == 2);
}

TEST_CASE("scene history records automation struct edits and ignores automation reads and camera")
{
    using A = woby::ControlAction;
    for (const auto action : {A::visibility, A::render, A::transformSet, A::opacity, A::colorSet, A::vertexSize}) {
        INFO("action: ", static_cast<int>(action));
        HistoryFixture f;
        woby::ControlOperation command;
        command.action = action;
        command.objectId = f.state.files[0].groupSettings[0].objectId;
        command.target = "part";
        switch (action) {
        case A::visibility: command.visible = false; break;
        case A::render: command.triangles = true; break;
        case A::transformSet: command.translation = {3, 4, 5}; break;
        case A::opacity: command.value = 0.5f; break;
        case A::colorSet: command.rgb = {0.2f, 0.3f, 0.4f}; break;
        case A::vertexSize: command.scale = 2.0f; break;
        default: break;
        }
        const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
        (void)woby::applyControlSceneOperation(f.state, f.clean, command, format, 100, 600);
        REQUIRE(woby::recordSceneHistory(f.history, f.state));
        CHECK(f.history.cursor == 1);
        // Repeating the same operation must not create another action.
        (void)woby::applyControlSceneOperation(f.state, f.clean, command, format, 100, 600);
        (void)woby::recordSceneHistory(f.history, f.state);
        CHECK(f.history.cursor == 1);
        for (const auto read : {A::transformGet, A::sceneInfo, A::cameraDolly, A::pane}) {
            command.action = read;
            command.factor = 2.0f;
            command.visible = false;
            (void)woby::applyControlSceneOperation(f.state, f.clean, command, format, 100, 600);
            CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
        }
        f.step();
        CHECK(woby::sceneContentEqual(woby::createSceneDocument(f.state), f.clean));
    }
}

TEST_CASE("saved camera navigation preserves dirty baseline redo and coalesced scene history")
{
    HistoryFixture f;
    std::optional<std::filesystem::path> path;
    woby::orbitUiCamera(f.state, 30, -20);
    woby::rollUiCamera(f.state, 12);
    woby::panUiCamera(f.state, 8, -5, 800);
    woby::dollyUiCamera(f.state, 0.3f);
    woby::updateSceneDirty(f.state, f.clean);
    CHECK_FALSE(f.state.isDirty);
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
    REQUIRE_FALSE(woby::saveSceneState(f.state, path, f.clean, f.root / "view.woby", false));
    CHECK(woby::readSceneDocument(*path).camera == f.state.camera);

    f.translate(1);
    f.step();
    woby::orbitUiCamera(f.state, -20, 10);
    woby::rollUiCamera(f.state, -7);
    woby::panUiCamera(f.state, -4, 9, 800);
    woby::dollyUiCamera(f.state, -0.2f);
    const auto view = f.state.camera;
    woby::updateSceneDirty(f.state, f.clean);
    CHECK_FALSE(f.state.isDirty);
    CHECK_FALSE(woby::recordSceneHistory(f.history, f.state));
    // Even a notified scene no-op with a different view must retain redo.
    woby::notifySceneEdit(f.state);
    CHECK(woby::recordSceneHistory(f.history, f.state));
    CHECK(woby::canRedoScene(f.history));
    REQUIRE_FALSE(woby::saveSceneState(f.state, path, f.clean, *path, true));
    CHECK(woby::readSceneDocument(*path).camera == view);
    CHECK(woby::canRedoScene(f.history));
    f.step(true);
    CHECK(f.state.camera == view);
    CHECK(f.state.isDirty);
    f.step();
    CHECK(f.state.camera == view);
    CHECK_FALSE(f.state.isDirty);

    // Returning a scene drag to its start still coalesces away after navigation.
    f.translate(2, 123);
    woby::orbitUiCamera(f.state, 10, 2);
    woby::resetGroupTransform(f.state.files[0].groupSettings[0]);
    woby::markSceneDirty(f.state);
    woby::recordSceneHistory(f.history, f.state);
    CHECK_FALSE(woby::canUndoScene(f.history));
    for (const auto& snapshot : f.history.snapshots) { CHECK_FALSE(snapshot.document.camera); }
}

TEST_CASE("scene history restores exact transforms and appearance and branches after undo")
{
    HistoryFixture f;
    CHECK_FALSE(woby::canUndoScene(f.history));
    CHECK_FALSE(woby::canRedoScene(f.history));
    f.translate(1.444f);
    const auto transformed = woby::createSceneDocument(f.state);
    woby::setGroupColor(f.state.files[0].groupSettings[0], {0.12f, 0.34f, 0.56f, 1});
    woby::setFileOpacity(f.state.files[0].fileSettings, 0.375f);
    woby::markSceneDirty(f.state);
    woby::recordSceneHistory(f.history, f.state);
    const auto appearance = woby::createSceneDocument(f.state);
    f.step();
    CHECK(woby::createSceneDocument(f.state) == transformed);
    f.step();
    CHECK(woby::createSceneDocument(f.state) == f.clean);
    CHECK_FALSE(f.state.isDirty);
    f.step(true);
    f.step(true);
    CHECK(woby::createSceneDocument(f.state) == appearance);
    f.step();
    f.translate(9.25f);
    CHECK_FALSE(woby::canRedoScene(f.history));
    f.step();
    CHECK(woby::createSceneDocument(f.state) == transformed);
}

TEST_CASE("scene history coalesces drag frames including release and separates interactions")
{
    HistoryFixture f;
    for (int frame = 1; frame <= 50; ++frame) { f.translate(static_cast<float>(frame) / 10, 123); }
    f.translate(5.125f); // Final edit delivered in the release frame.
    CHECK(f.history.cursor == 1);
    const auto firstDrag = woby::createSceneDocument(f.state);
    f.translate(6, 456);
    f.translate(7, 456);
    woby::recordSceneHistory(f.history, f.state);
    CHECK(f.history.cursor == 2);
    f.step();
    CHECK(woby::createSceneDocument(f.state) == firstDrag);
    f.step();
    CHECK(woby::createSceneDocument(f.state) == f.clean);
    f.step(true);
    CHECK(woby::createSceneDocument(f.state) == firstDrag);
}

TEST_CASE("scene history ignores transient changes and drags that return to their start")
{
    HistoryFixture f;
    f.state.uiScale = 1.5f;
    f.state.viewerPaneVisible = false;
    f.state.propertiesPaneVisible = true;
    woby::setPropertiesPaneWidth(f.state, 560.0f, 300.0f, 900.0f);
    f.state.camera.distance = 42;
    woby::selectSceneObject(f.state, f.state.files[0].objectId);
    woby::recordSceneHistory(f.history, f.state);
    CHECK_FALSE(woby::canUndoScene(f.history));
    f.translate(1, 123);
    woby::resetGroupTransform(f.state.files[0].groupSettings[0]);
    woby::markSceneDirty(f.state);
    woby::recordSceneHistory(f.history, f.state);
    CHECK_FALSE(woby::canUndoScene(f.history));
    f.translate(2);
    f.step();
    CHECK(f.state.uiScale == 1.5f);
    CHECK_FALSE(f.state.viewerPaneVisible);
    CHECK(f.state.propertiesPaneVisible);
    CHECK(f.state.propertiesPaneWidth == 560.0f);
    CHECK(f.state.camera.distance == 42);
    CHECK(f.state.selectedSceneObjects == std::vector<woby::SceneObjectId>{f.state.files[0].objectId});
}

TEST_CASE("scene history explicit save boundary splits a continuing interaction at the clean state")
{
    HistoryFixture f;
    f.translate(1, 123);
    f.clean = woby::createSceneDocument(f.state);
    woby::clearSceneDirty(f.state);
    woby::finishSceneHistoryInteraction(f.history);
    // The same widget can remain active after a global Save shortcut.
    f.translate(2, 123);
    woby::recordSceneHistory(f.history, f.state);
    REQUIRE(f.history.cursor == 2);
    f.step();
    CHECK_FALSE(f.state.isDirty);
    CHECK(woby::createSceneDocument(f.state) == f.clean);
    f.step();
    CHECK(f.state.isDirty);
    f.step(true);
    CHECK_FALSE(f.state.isDirty);
}

TEST_CASE("scene history reloads removed geometry and restores folder identity and analysis references")
{
    HistoryFixture f;
    const auto fileId = f.state.files[0].objectId;
    const auto partId = f.state.files[0].groupSettings[0].objectId;
    const auto folderId = f.state.sceneNodes[0].objectId;
    const auto comparisonId = woby::createComparison(f.state);
    woby::setComparisonObjects(f.state, {partId}, woby::ComparisonSide::a, true, comparisonId);
    woby::recordSceneHistory(f.history, f.state);
    const auto withComparison = woby::createSceneDocument(f.state);
    REQUIRE(woby::removeFileFromState(f.state, 0));
    woby::recordSceneHistory(f.history, f.state);
    CHECK(woby::missingComparisonPartCount(f.state, comparisonId) == 1);
    f.step();
    CHECK(woby::sceneContentEqual(woby::createSceneDocument(f.state), withComparison));
    CHECK(f.state.files[0].objectId == fileId);
    CHECK(f.state.files[0].groupSettings[0].objectId == partId);
    CHECK(f.state.sceneNodes[0].objectId == folderId);
    CHECK(f.state.files[0].mesh.vertices[1].position[0] == 1);
    CHECK(f.state.files[0].mesh.indices == std::vector<uint32_t>{0, 1, 2});
    CHECK(woby::missingComparisonPartCount(f.state, comparisonId) == 0);
    CHECK(woby::comparisonContains(f.state, partId, woby::ComparisonSide::a, comparisonId));
    f.step(true);
    CHECK(f.state.files.empty());
    CHECK(woby::missingComparisonPartCount(f.state, comparisonId) == 1);
    f.step();
    woby::removeComparison(f.state, comparisonId);
    woby::recordSceneHistory(f.history, f.state);
    f.step();
    CHECK(woby::sceneContentEqual(woby::createSceneDocument(f.state), withComparison));
}

TEST_CASE("scene history undoes additions and membership while keeping object IDs monotonic")
{
    HistoryFixture f;
    writeHistoryModel(f.root / "new.obj");
    auto file = woby::createUiFileState(f.root / "new.obj", f.state.files[0].mesh, 1);
    f.state.files.push_back(std::move(file));
    woby::appendDefaultSceneNodesForFiles(f.state, 1);
    const auto addedId = f.state.files[1].objectId;
    woby::recordSceneHistory(f.history, f.state);
    const auto comparisonId = woby::createComparison(f.state);
    woby::recordSceneHistory(f.history, f.state);
    woby::setComparisonObjects(f.state, {addedId}, woby::ComparisonSide::b, true, comparisonId);
    woby::recordSceneHistory(f.history, f.state);
    f.step();
    CHECK(woby::comparisonPartCount(f.state, woby::ComparisonSide::b, comparisonId) == 0);
    f.step();
    CHECK(f.state.comparisons.empty());
    f.step();
    CHECK(f.state.files.size() == 1);
    f.step(true);
    CHECK(f.state.files[1].objectId == addedId);
    const auto newId = woby::createComparison(f.state);
    CHECK(newId > comparisonId);
}

TEST_CASE("scene history dirty state follows the latest saved document through undo redo and branching")
{
    HistoryFixture f;
    f.translate(1);
    f.translate(2);
    // Use the same persistence entry point as the app, including Save As.
    const auto path = std::filesystem::temp_directory_path()
        / ("woby-scene-history-" + woby::automationRandomHex(8) + ".woby");
    std::optional<std::filesystem::path> currentPath;
    REQUIRE_FALSE(woby::saveSceneState(f.state, currentPath, f.clean, path, true));
    CHECK_FALSE(f.state.isDirty);
    f.translate(3);
    f.step();
    CHECK_FALSE(f.state.isDirty);
    f.step();
    CHECK(f.state.isDirty);
    f.step(true);
    CHECK_FALSE(f.state.isDirty);
    f.step();
    f.translate(4);
    f.step();
    CHECK(f.state.isDirty);
    // Saving does not depend on the saved version still being in history.
    REQUIRE_FALSE(woby::saveSceneState(f.state, currentPath, f.clean, path, true));
    f.step(true);
    CHECK(f.state.isDirty);
    f.step();
    CHECK_FALSE(f.state.isDirty);
    std::filesystem::remove(path);
}

TEST_CASE("scene history retains all metadata-only actions beyond 100 until committed replacement")
{
    HistoryFixture f;
    constexpr size_t actionCount = 250;
    for (size_t action = 1; action <= actionCount; ++action) {
        f.translate(static_cast<float>(action));
    }
    CHECK(f.history.snapshots.size() == actionCount + 1);
    for (const auto& snapshot : f.history.snapshots) {
        REQUIRE(snapshot.content.files.size() == 1);
        CHECK(snapshot.content.files[0].mesh.vertices.empty());
        CHECK(snapshot.content.files[0].mesh.indices.empty());
        CHECK(snapshot.content.files[0].mesh.nodes.empty());
        CHECK(snapshot.content.files[0].path == f.source);
    }
    auto replacement = woby::prepareSceneReplacement(f.state, {}, woby::createSceneDocument(woby::UiState{}));
    CHECK(woby::canUndoScene(f.history)); // Failed/canceled open never commits this candidate.
    for (size_t action = 0; action < actionCount; ++action) { f.step(); }
    CHECK_FALSE(woby::canUndoScene(f.history));
    CHECK(woby::createSceneDocument(f.state) == f.clean);
    CHECK_FALSE(f.state.isDirty);
    for (size_t action = 1; action <= actionCount; ++action) {
        f.step(true);
        CHECK(f.state.files[0].groupSettings[0].translation[0] == static_cast<float>(action));
    }
    CHECK_FALSE(woby::canRedoScene(f.history));
    f.step(); // Leave both Undo and Redo available before scene replacement.
    f.state = std::move(replacement);
    CHECK_FALSE(woby::prepareSceneHistoryStep(f.history, f.state, f.clean, true));
    woby::recordSceneHistory(f.history, f.state);
    CHECK_FALSE(woby::canUndoScene(f.history));
    CHECK_FALSE(woby::canRedoScene(f.history));
    CHECK(f.history.snapshots.size() == 1);
}

TEST_CASE("scene history reload accepts changed source geometry and refreshes bounds")
{
    HistoryFixture f;
    const auto partId = f.state.files[0].groupSettings[0].objectId;
    const auto comparisonId = woby::createComparison(f.state);
    woby::setComparisonObjects(f.state, {partId}, woby::ComparisonSide::a, true, comparisonId);
    f.translate(1.444f);
    const auto beforeRemoval = woby::createSceneDocument(f.state);
    f.clean = beforeRemoval;
    REQUIRE(woby::removeFileFromState(f.state, 0));
    woby::recordSceneHistory(f.history, f.state);
    writeHistoryModel(f.source, 9);
    f.step();
    CHECK(f.state.files[0].mesh.bounds.max[0] == 9);
    CHECK(f.state.files[0].groupSettings[0].localBounds.max[0] == 9);
    CHECK(f.state.files[0].fileSettings.center[0] == 4.5f);
    CHECK(f.state.files[0].groupSettings[0].translation[0] == 1.444f);
    CHECK(woby::comparisonContains(f.state, partId, woby::ComparisonSide::a, comparisonId));
    CHECK(woby::sceneContentEqual(woby::createSceneDocument(f.state), beforeRemoval));
    CHECK_FALSE(f.state.isDirty); // Geometry is external to the saved document.
    const auto cursor = f.history.cursor;
    woby::recordSceneHistory(f.history, f.state);
    CHECK(f.history.cursor == cursor);
    CHECK(woby::canRedoScene(f.history));
}

TEST_CASE("scene history edits reuse loaded geometry even when its source disappears")
{
    HistoryFixture f;
    f.translate(3);
    std::filesystem::remove(f.source);
    f.step();
    CHECK(woby::createSceneDocument(f.state) == f.clean);
    f.step(true);
    CHECK(f.state.files[0].groupSettings[0].translation[0] == 3);
    CHECK(f.state.files[0].mesh.indices.size() == 3);
}

TEST_CASE("scene history consumes failed reloads without changing the scene or creating phantom edits")
{
    for (const bool redo : {false, true}) {
        for (const auto failure : {"missing", "invalid", "changed-parts", "missing-importer"}) {
            INFO("redo: ", redo, "; failure: ", failure);
            HistoryFixture f;
            if (std::string(failure) == "missing-importer") {
                f.state.files[0].importerId = "unavailable-history-test-importer";
                woby::markSceneDirty(f.state);
            }
            if (redo) {
                // Empty baseline followed by an addition, then undo that addition.
                woby::UiState empty;
                woby::resetSceneHistory(f.history, empty);
                woby::recordSceneHistory(f.history, f.state);
                f.step();
            } else {
                woby::recordSceneHistory(f.history, f.state);
                REQUIRE(woby::removeFileFromState(f.state, 0));
                woby::recordSceneHistory(f.history, f.state);
            }
            if (std::string(failure) == "missing") { std::filesystem::remove(f.source); }
            if (std::string(failure) == "invalid") { std::ofstream(f.source) << "not an OBJ model"; }
            if (std::string(failure) == "changed-parts") { writeHistoryModel(f.source, 2, "different-part"); }
            const auto before = woby::createSceneDocument(f.state);
            const auto cursor = f.history.cursor;
            CHECK_THROWS_WITH((void)woby::loadSceneHistoryStep(f.history, f.state, f.clean, redo),
                doctest::Contains(f.source.string()));
            CHECK(f.history.cursor == cursor); // Preparation has no side effects.
            CHECK(woby::createSceneDocument(f.state) == before);
            woby::skipSceneHistoryStep(f.history, f.state, redo);
            CHECK(f.history.cursor == (redo ? cursor + 1 : cursor - 1));
            const auto progressed = f.history.cursor;
            const auto size = f.history.snapshots.size();
            woby::recordSceneHistory(f.history, f.state);
            woby::recordSceneHistory(f.history, f.state);
            CHECK(f.history.cursor == progressed);
            CHECK(f.history.snapshots.size() == size);
            CHECK(woby::createSceneDocument(f.state) == before);
            if (redo) { CHECK_FALSE(woby::canRedoScene(f.history)); }
            else { CHECK(woby::canRedoScene(f.history)); }
            // New edits still branch from the actual unchanged scene.
            woby::setShowGrid(f.state, true);
            woby::recordSceneHistory(f.history, f.state);
            CHECK_FALSE(woby::canRedoScene(f.history));
            f.step();
            CHECK(woby::createSceneDocument(f.state) == before);
        }
    }
}

TEST_CASE("scene history preparation does not partly restore a batch when one source fails")
{
    HistoryFixture f;
    writeHistoryModel(f.root / "second.obj");
    f.state.files.push_back(woby::createUiFileState(f.root / "second.obj",
        woby::loadModelMesh(f.root / "second.obj"), 1));
    woby::appendDefaultSceneNodesForFiles(f.state, 1);
    woby::UiState empty;
    woby::resetSceneHistory(f.history, empty);
    woby::recordSceneHistory(f.history, f.state);
    f.step();
    std::filesystem::remove(f.root / "second.obj");
    CHECK_THROWS((void)woby::loadSceneHistoryStep(f.history, f.state, f.clean, true));
    CHECK(f.state.files.empty());
    woby::skipSceneHistoryStep(f.history, f.state, true);
    CHECK_FALSE(woby::canRedoScene(f.history));
    CHECK(f.state.files.empty());
}
