#include "ui_operations.h"
#include "scene_history.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <fstream>
#include <limits>

namespace {
struct ViewDirectory {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-views-" + woby::automationRandomHex(8));

    ViewDirectory() { std::filesystem::create_directory(root); }
    ~ViewDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
};

woby::UiFileState viewFile(const std::filesystem::path& root = ".")
{
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"part", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    return woby::createUiFileState(root / "same.obj", std::move(mesh), 0);
}

woby::UiState viewState(const std::filesystem::path& root = ".")
{
    woby::UiState state;
    state.files.push_back(viewFile(root));
    state.files.push_back(viewFile(root));
    woby::appendFolderTreeSceneNode(state, root, 0, 2);
    const auto comparison = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].groupSettings[0].objectId}, woby::ComparisonSide::a, true, comparison);
    woby::setComparisonObjects(state, {state.files[1].groupSettings[0].objectId}, woby::ComparisonSide::b, true, comparison);
    woby::clearSceneDirty(state);
    return state;
}

void step(woby::SceneHistory& history, woby::UiState& state, const woby::SceneDocument& clean, bool redo = false)
{
    auto prepared = woby::prepareSceneHistoryStep(history, state, clean, redo);
    REQUIRE(prepared);
    woby::commitSceneHistoryStep(history, state, std::move(*prepared), redo);
}
}

TEST_CASE("views capture independent display checkpoints and restore selection dimensions and camera")
{
    auto state = viewState();
    auto& file = state.files[0];
    auto& group = file.groupSettings[0];
    woby::setGroupColor(group, {.1f, .2f, .3f, .4f});
    woby::setGroupTranslation(group, {2, 3, 4});
    woby::setGroupRotationDegrees(group, {10, 20, 30});
    woby::setGroupScale(group, 2);
    woby::setGroupOpacity(group, .5f);
    group.showTriangles = true;
    group.showVertices = true;
    woby::setGroupVertexSizeScale(group, 3);
    woby::setFileTranslation(file.fileSettings, {5, 6, 7});
    woby::setFileVertexSizeScale(file, 2);
    woby::setSceneNodeTranslation(state.sceneNodes[0].settings, {4, 5, 6});
    woby::setShowDimensions(state, true);
    woby::setShowGrid(state, true);
    woby::setShowOrigin(state, true);
    woby::setSceneUpAxis(state, woby::SceneUpAxis::y);
    woby::setMasterVertexPointSize(state, 12);
    state.camera.target = {7, 8, 9};
    state.camera.distance = 42;
    state.selectedSceneObjects = {group.objectId, state.files[1].objectId};
    state.comparisons[0].settings.mode = woby::ComparisonMode::surfaceQuality;
    state.comparisons[0].settings.colorRange = 2;
    state.comparisons[0].translation = {8, 9, 10};
    const auto original = woby::createSceneDocument(state);
    const auto selection = state.selectedSceneObjects;
    const auto id = woby::createView(state);
    woby::setGroupColor(group, {1, 1, 1, 1});
    woby::setGroupVisible(state, file, group, false);
    woby::resetGroupTransform(group);
    woby::resetFileTransform(file.fileSettings);
    woby::resetSceneNodeTransform(state.sceneNodes[0].settings);
    woby::setShowDimensions(state, false);
    woby::setShowGrid(state, false);
    state.camera.distance = 3;
    state.selectedSceneObjects.clear();
    state.comparisons[0].settings.enabled = false;
    state.comparisons[0].a[0].enabled = false;
    state.comparisons[0].translation = {};
    const auto second = woby::createView(state);
    REQUIRE(woby::findView(state, id)->scene.camera.distance == 42);
    woby::applyView(state, id);
    auto restored = woby::createSceneDocument(state);
    restored.views.clear();
    CHECK(restored == original);
    CHECK(state.selectedSceneObjects == selection);
    CHECK(state.camera.distance == 42);
    CHECK(state.files[0].mesh.vertices.size() == 3);
    woby::applyView(state, second);
    CHECK_FALSE(state.files[0].groupSettings[0].visible);
    CHECK(state.camera.distance == 3);
    CHECK(state.selectedSceneObjects.empty());
}

TEST_CASE("view names updates deletion and no-op operations are independent scene edits")
{
    auto state = viewState();
    const auto first = woby::createView(state), second = woby::createView(state);
    CHECK(woby::findView(state, first)->name == "View 1");
    CHECK(woby::findView(state, second)->name == "View 2");
    woby::renameView(state, first, "  Detail \"A\"  ");
    CHECK(woby::findView(state, first)->name == "Detail \"A\"");
    auto revision = state.sceneEditRevision;
    woby::renameView(state, first, "Detail \"A\"");
    woby::updateView(state, first);
    woby::applyView(state, first);
    CHECK(state.sceneEditRevision == revision);
    const auto clean = woby::createSceneDocument(state);
    state.camera.distance += 4;
    woby::updateSceneDirty(state, clean);
    CHECK_FALSE(state.isDirty);
    woby::updateView(state, first);
    woby::updateSceneDirty(state, clean);
    CHECK(state.isDirty);
    CHECK(woby::findView(state, first)->scene.camera == state.camera);
    CHECK(woby::findView(state, second)->scene.camera != state.camera);
    woby::removeView(state, first);
    CHECK_FALSE(woby::findView(state, first));
    CHECK(state.views.size() == 1);
    revision = state.sceneEditRevision;
    woby::removeView(state, first);
    woby::renameView(state, first, "missing");
    woby::updateView(state, first);
    woby::applyView(state, first);
    CHECK(state.sceneEditRevision == revision);
}

TEST_CASE("views persist with document references across fresh IDs and duplicate object names")
{
    const ViewDirectory directory;
    auto state = viewState(directory.root);
    state.files[0].groupSettings[0].color = {.2f, .4f, .6f, 1};
    state.files[1].groupSettings[0].color = {.8f, .6f, .4f, 1};
    state.selectedSceneObjects = {state.files[1].groupSettings[0].objectId, state.files[0].objectId};
    woby::createView(state);
    woby::renameView(state, state.views[0].id, "Detail \"one\"\nsecond line");
    state.camera.distance = 73;
    woby::createView(state);
    const auto document = woby::createSceneDocument(state);
    const auto path = directory.root / "roundtrip.woby";
    woby::writeSceneDocument(path, document);
    const auto read = woby::readSceneDocument(path);
    CHECK(read.views == document.views);
    auto loaded = woby::prepareSceneReplacement(state, state.files, read);
    REQUIRE(loaded.views.size() == 2);
    CHECK(loaded.views[0].id != state.views[0].id);
    CHECK(loaded.files[0].objectId != state.files[0].objectId);
    CHECK(woby::sceneViewRecords(loaded) == document.views);
    loaded.files[0].groupSettings[0].color = {1, 1, 1, 1};
    loaded.files[1].groupSettings[0].color = {0, 0, 0, 1};
    woby::applyView(loaded, loaded.views[0].id);
    CHECK(loaded.files[0].groupSettings[0].color == state.files[0].groupSettings[0].color);
    CHECK(loaded.files[1].groupSettings[0].color == state.files[1].groupSettings[0].color);
    CHECK(loaded.selectedSceneObjects == std::vector<woby::SceneObjectId>{loaded.files[1].groupSettings[0].objectId, loaded.files[0].objectId});
    const auto empty = woby::prepareSceneReplacement(loaded, {}, {});
    CHECK(empty.views.empty());
    CHECK(empty.nextViewId == loaded.nextViewId);
}

TEST_CASE("removed references are pruned from every view without affecting new or surviving objects")
{
    auto state = viewState();
    const auto removedFile = state.files[0].objectId;
    const auto removedGroup = state.files[0].groupSettings[0].objectId;
    const auto comparison = state.comparisons[0].objectId;
    state.selectedSceneObjects = {removedGroup};
    woby::createView(state);
    woby::createView(state);
    REQUIRE(woby::removeFileFromState(state, 0));
    woby::removeComparison(state, comparison);
    for (const auto& view : state.views) {
        for (const auto& object : view.objects) {
            CHECK(object.objectId != removedFile);
            CHECK(object.objectId != removedGroup);
            CHECK(object.objectId != comparison);
        }
    }
    state.files.push_back(viewFile());
    woby::appendDefaultSceneNodesForFiles(state, 1);
    state.files.back().groupSettings[0].color = {.7f, .8f, .9f, 1};
    woby::applyView(state, state.views[0].id);
    CHECK(state.selectedSceneObjects.empty());
    CHECK(state.files.back().groupSettings[0].color == std::array<float, 4>{.7f, .8f, .9f, 1});
    REQUIRE(woby::removeFileFromState(state, 0));
    for (const auto& view : state.views) { CHECK(view.objects.empty()); }
    CHECK(state.views.size() == 2);
}

TEST_CASE("view loading drops invalid and duplicate references and normalizes display values")
{
    auto state = viewState();
    woby::createView(state);
    auto document = woby::createSceneDocument(state);
    auto& objects = document.views[0].objects;
    auto invalid = objects[0];
    invalid.index = 1000;
    objects.push_back(invalid);
    objects.push_back(objects[0]);
    for (auto& object : objects) {
        object.settings.appearance.scale = 10000;
        object.settings.appearance.opacity = -10;
        object.settings.appearance.color = {-1, 2, .5f, 1};
    }
    document.views[0].scene.masterVertexPointSize = std::numeric_limits<float>::quiet_NaN();
    auto loaded = woby::prepareSceneReplacement(state, state.files, document);
    CHECK(loaded.views[0].objects.size() == state.views[0].objects.size());
    CHECK(loaded.views[0].scene.masterVertexPointSize == woby::defaultMasterVertexPointSize);
    woby::applyView(loaded, loaded.views[0].id);
    CHECK(loaded.files[0].groupSettings[0].scale == woby::maxGroupScale);
    CHECK(loaded.files[0].groupSettings[0].opacity == 0);
    CHECK(loaded.files[0].groupSettings[0].color == std::array<float, 4>{0, 1, .5f, 1});
}

TEST_CASE("applying views restores camera and selection through undo redo without recording ordinary navigation")
{
    auto state = viewState();
    state.camera.distance = 12;
    state.selectedSceneObjects = {state.files[0].objectId};
    const auto id = woby::createView(state);
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    state.camera.distance = 24;
    state.selectedSceneObjects = {state.files[1].objectId};
    CHECK_FALSE(woby::recordSceneHistory(history, state));
    woby::applyView(state, id);
    REQUIRE(woby::recordSceneHistory(history, state));
    REQUIRE(history.snapshots.size() == 2);
    woby::updateSceneDirty(state, clean);
    CHECK_FALSE(state.isDirty); // Applying a camera-only checkpoint is navigation.
    step(history, state, clean);
    CHECK(state.camera.distance == 24);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{state.files[1].objectId});
    step(history, state, clean, true);
    CHECK(state.camera.distance == 12);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{state.files[0].objectId});
    state.camera.distance = 36;
    woby::setShowGrid(state, true);
    woby::recordSceneHistory(history, state);
    step(history, state, clean);
    CHECK(state.camera.distance == 36); // Ordinary edit undo preserves navigation.
    step(history, state, clean);
    CHECK(state.camera.distance == 24); // Apply View undo uses its actual entry camera.
}

TEST_CASE("view CRUD and reference pruning are undoable with scene content")
{
    auto state = viewState();
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    const auto id = woby::createView(state);
    woby::recordSceneHistory(history, state);
    woby::renameView(state, id, "Named");
    woby::recordSceneHistory(history, state);
    state.camera.distance = 37;
    woby::updateView(state, id);
    woby::recordSceneHistory(history, state);
    woby::removeView(state, id);
    woby::recordSceneHistory(history, state);
    step(history, state, clean);
    REQUIRE(woby::findView(state, id));
    CHECK(woby::findView(state, id)->scene.camera.distance == 37);
    step(history, state, clean);
    CHECK(woby::findView(state, id)->scene.camera.distance != 37);
    step(history, state, clean);
    CHECK(woby::findView(state, id)->name == "View 1");
    step(history, state, clean);
    CHECK(state.views.empty());
    step(history, state, clean, true);
    const auto comparison = state.comparisons[0].objectId;
    const auto count = state.views[0].objects.size();
    woby::removeComparison(state, comparison);
    woby::recordSceneHistory(history, state);
    CHECK(state.views[0].objects.size() == count - 1);
    step(history, state, clean);
    CHECK(state.views[0].objects.size() == count);
    REQUIRE(woby::findComparison(state, comparison));
}

TEST_CASE("legacy scenes have no views and malformed view camera or child tables are rejected")
{
    const ViewDirectory directory;
    const auto path = directory.root / "malformed.woby";
    { std::ofstream out(path); out << "version = 6\n"; }
    CHECK(woby::readSceneDocument(path).views.empty());
    { std::ofstream out(path); out << "version = 7\n[[views.objects]]\nindex = 0\n"; }
    CHECK_THROWS((void)woby::readSceneDocument(path));
    { std::ofstream out(path); out << "version = 7\n[[views]]\n[views.camera]\ndistance = nan\n"; }
    CHECK_THROWS((void)woby::readSceneDocument(path));
}

TEST_CASE("view application undoes appearance and navigation together without merging adjacent edits")
{
    auto state = viewState();
    state.camera.distance = 10;
    const auto id = woby::createView(state);
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    woby::setShowGrid(state, true);
    state.camera.distance = 20;
    woby::recordSceneHistory(history, state, 123);
    woby::applyView(state, id);
    woby::recordSceneHistory(history, state, 123);
    REQUIRE(history.snapshots.size() == 3);
    CHECK_FALSE(state.showGrid);
    step(history, state, clean);
    CHECK(state.showGrid);
    CHECK(state.camera.distance == 20);
    step(history, state, clean, true);
    CHECK_FALSE(state.showGrid);
    CHECK(state.camera.distance == 10);
}

TEST_CASE("undo file removal restores checkpoint references and pruned folders with their identities")
{
    auto state = viewState();
    state.selectedSceneObjects = {state.files[0].objectId};
    const auto id = woby::createView(state);
    const auto savedObjects = state.views[0].objects;
    const auto savedParts = state.views[0].parts;
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    auto reloaded = state.files;
    REQUIRE(woby::removeFileFromState(state, 0));
    woby::recordSceneHistory(history, state);
    REQUIRE(state.views[0].objects.size() < savedObjects.size());
    auto restored = woby::prepareSceneHistoryStep(history, state, clean, false, std::move(reloaded));
    REQUIRE(restored);
    woby::commitSceneHistoryStep(history, state, std::move(*restored), false);
    REQUIRE(woby::findView(state, id));
    CHECK(state.views[0].objects == savedObjects);
    CHECK(state.views[0].parts == savedParts);
    woby::applyView(state, id);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{state.files[0].objectId});
}
