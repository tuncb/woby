#include "ui_operations.h"
#include "scene_history.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <limits>

namespace {
struct BulkFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-bulk-" + woby::automationRandomHex(8));
    woby::UiState state;
    BulkFixture()
    {
        std::filesystem::create_directory(root);
        for (int i = 0; i < 3; ++i) {
            woby::Mesh mesh;
            auto& geometry = woby::editMeshGeometry(mesh);
            geometry.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
            geometry.indices = {0, 1, 2};
            geometry.nodes = {{"surface", 0, 3}};
            mesh.bounds = woby::calculateBounds(geometry.vertices);
            state.files.push_back(woby::createUiFileState(root / (std::to_string(i) + ".obj"), std::move(mesh), 0));
        }
        woby::appendFolderTreeSceneNode(state, root, 0, 2);
        woby::appendDefaultSceneNodesForFiles(state, 2);
    }
    ~BulkFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    woby::SceneObjectId analysis()
    {
        const auto id = woby::createComparison(state);
        woby::setComparisonObjects(state, {state.files[0].objectId, state.files[1].objectId}, woby::ComparisonSide::a, true, id);
        woby::setComparisonObjects(state, {state.files[2].objectId}, woby::ComparisonSide::b, true, id);
        return id;
    }
    woby::SceneObjectId annotation()
    {
        woby::UiAnnotation item;
        item.targetId = state.files[0].groupSettings[0].objectId;
        item.targetName = "surface";
        item.settings.note = "Keep this note";
        state.annotations.push_back(item);
        woby::assignSceneObjectIds(state);
        return state.annotations.back().objectId;
    }
};

void undo(woby::SceneHistory& history, woby::UiState& state, const woby::SceneDocument& clean, bool redo = false)
{
    auto restored = woby::prepareSceneHistoryStep(history, state, clean, redo);
    REQUIRE(restored);
    woby::commitSceneHistoryStep(history, state, std::move(*restored), redo);
}
}

TEST_CASE("bulk list selection supports visible ranges toggles context clicks and stale anchors")
{
    std::vector<uint64_t> selected;
    uint64_t anchor = 0;
    const std::array<uint64_t, 4> rows{7, 2, 9, 4};
    woby::selectListItem(selected, anchor, 2, rows);
    woby::selectListItem(selected, anchor, 4, rows, false, true);
    CHECK(selected == std::vector<uint64_t>{2, 9, 4});
    CHECK(anchor == 2);
    woby::selectListItem(selected, anchor, 7, rows, true, true);
    CHECK(selected == std::vector<uint64_t>{2, 9, 4, 7});
    woby::selectListItem(selected, anchor, 9, rows, true);
    CHECK(selected == std::vector<uint64_t>{2, 4, 7});
    woby::selectListItem(selected, anchor, 9, rows, false, false, true);
    woby::selectListItem(selected, anchor, 9, rows, false, false, true);
    CHECK(selected == std::vector<uint64_t>{2, 4, 7, 9});
    anchor = 555;
    woby::selectListItem(selected, anchor, 4, rows, false, true);
    CHECK(selected == std::vector<uint64_t>{4});
    woby::selectListItem(selected, anchor, 555, rows);
    CHECK(selected == std::vector<uint64_t>{4});
}

TEST_CASE("bulk visibility handles mixed kinds and overlapping parents with one undo and no stale edits")
{
    BulkFixture f;
    const auto analysis = f.analysis(), annotation = f.annotation();
    auto& s = f.state;
    s.selectedSceneObjects = {s.sceneNodes[0].objectId, s.files[0].objectId, s.files[0].groupSettings[0].objectId, analysis, annotation};
    const auto selection = s.selectedSceneObjects;
    const auto clean = woby::createSceneDocument(s);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, s);
    CHECK(woby::setSceneSelectionVisible(s, false));
    CHECK(s.selectedSceneObjects == selection);
    CHECK_FALSE(s.files[0].fileSettings.visible);
    CHECK_FALSE(s.files[1].groupSettings[0].visible);
    CHECK(s.files[2].fileSettings.visible);
    CHECK_FALSE(woby::findComparison(s, analysis)->settings.enabled);
    CHECK_FALSE(woby::findAnnotation(s, annotation)->settings.visible);
    woby::recordSceneHistory(history, s);
    CHECK(history.snapshots.size() == 2);
    const auto hidden = woby::createSceneDocument(s);
    CHECK_FALSE(woby::setSceneSelectionVisible(s, false));
    undo(history, s, clean);
    CHECK(woby::createSceneDocument(s) == clean);
    undo(history, s, clean, true);
    CHECK(woby::createSceneDocument(s) == hidden);
    s.selectedSceneObjects.push_back(999999);
    CHECK_FALSE(woby::setSceneSelectionVisible(s, true));
    CHECK(woby::createSceneDocument(s) == hidden);
}

TEST_CASE("bulk analysis duplication snapshots IDs and deletion preserves sources through undo")
{
    BulkFixture f;
    auto& s = f.state;
    const auto a = f.analysis(), b = f.analysis();
    s.selectedSceneObjects = {a, b, a};
    const auto clean = woby::createSceneDocument(s);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, s);
    REQUIRE(woby::duplicateSceneSelection(s));
    REQUIRE(s.comparisons.size() == 4);
    REQUIRE(s.selectedSceneObjects.size() == 2);
    CHECK(woby::findComparison(s, s.selectedSceneObjects[0])->a == woby::findComparison(s, a)->a);
    CHECK(woby::findComparison(s, s.selectedSceneObjects[1])->b == woby::findComparison(s, b)->b);
    woby::recordSceneHistory(history, s);
    CHECK(history.snapshots.size() == 2);
    undo(history, s, clean);
    CHECK(s.comparisons.size() == 2);
    s.selectedSceneObjects = {a, b};
    CHECK(woby::deleteSceneSelection(s));
    CHECK(s.comparisons.empty());
    CHECK(s.files.size() == 3);
    woby::recordSceneHistory(history, s);
    undo(history, s, clean);
    CHECK(woby::createSceneDocument(s) == clean);
    s.selectedSceneObjects = {a, s.files[0].objectId};
    CHECK_FALSE(woby::duplicateSceneSelection(s));
    CHECK_FALSE(woby::deleteSceneSelection(s));
    CHECK(woby::createSceneDocument(s) == clean);
}

TEST_CASE("bulk annotation style and copies retain notes targets and independent settings")
{
    BulkFixture f;
    auto& s = f.state;
    const auto a = f.annotation(), b = f.annotation();
    s.selectedSceneObjects = {a, b};
    const auto target = woby::findAnnotation(s, a)->targetId;
    CHECK(woby::setSelectedAnnotationStyle(s, true, 6.0f, std::array<float, 4>{1, 0, 0, 1}));
    for (auto id : {a, b}) {
        const auto* item = woby::findAnnotation(s, id);
        CHECK(item->settings.locked);
        CHECK(item->settings.width == 6);
        CHECK(item->settings.note == "Keep this note");
        CHECK(item->targetId == target);
    }
    const auto before = woby::createSceneDocument(s);
    CHECK_FALSE(woby::setSelectedAnnotationStyle(s, false, std::numeric_limits<float>::quiet_NaN()));
    CHECK(woby::createSceneDocument(s) == before);
    REQUIRE(woby::duplicateSceneSelection(s));
    REQUIRE(s.selectedSceneObjects.size() == 2);
    CHECK(woby::setSelectedAnnotationStyle(s, false));
    CHECK(woby::findAnnotation(s, a)->settings.locked);
    CHECK(woby::deleteSceneSelection(s));
    CHECK(woby::createSceneDocument(s) == before);
}

TEST_CASE("bulk input selections preserve the inspector and act only on their analysis side")
{
    BulkFixture f;
    auto& s = f.state;
    const auto id = f.analysis();
    const auto other = f.analysis();
    woby::selectSceneObject(s, id);
    const std::array rows{s.files[0].objectId, s.files[1].objectId};
    const auto clean = woby::createSceneDocument(s);
    const auto revision = s.sceneEditRevision;
    woby::selectAnalysisInputRows(s, id, woby::ComparisonSide::a, rows);
    CHECK(s.selectedSceneObjects == std::vector<woby::SceneObjectId>{id});
    CHECK(woby::createSceneDocument(s) == clean);
    CHECK(s.sceneEditRevision == revision);
    CHECK(woby::applySelectedAnalysisInputs(s, id, woby::ComparisonSide::a, woby::UiInputAction::disable));
    CHECK(woby::enabledComparisonPartCount(s, woby::ComparisonSide::a, id) == 0);
    CHECK(woby::enabledComparisonPartCount(s, woby::ComparisonSide::b, id) == 1);
    CHECK(woby::enabledComparisonPartCount(s, woby::ComparisonSide::a, other) == 2);
    CHECK(woby::applySelectedAnalysisInputs(s, id, woby::ComparisonSide::a, woby::UiInputAction::enable));
    woby::selectAnalysisInputRow(s, id, woby::ComparisonSide::a, rows[0], rows);
    CHECK(woby::applySelectedAnalysisInputs(s, id, woby::ComparisonSide::a, woby::UiInputAction::isolate));
    CHECK(woby::enabledComparisonPartCount(s, woby::ComparisonSide::a, id) == 1);
    woby::selectAnalysisInputRows(s, id, woby::ComparisonSide::a, rows);
    CHECK(woby::applySelectedAnalysisInputs(s, id, woby::ComparisonSide::a, woby::UiInputAction::remove));
    CHECK(woby::findComparison(s, id)->a.empty());
    CHECK(woby::findComparison(s, id)->inputSelection[0].ids.empty());
}

TEST_CASE("bulk detector requests are transient and saved settings change only selected checks")
{
    BulkFixture f;
    auto& s = f.state;
    const auto id = f.analysis();
    using C = woby::DiagnosticCategory;
    const std::array<uint64_t, 2> rows{static_cast<uint64_t>(C::holes) + 1, static_cast<uint64_t>(C::selfIntersections) + 1};
    const auto clean = woby::createSceneDocument(s);
    const auto revision = s.sceneEditRevision;
    woby::selectDetectorRows(s, id, rows);
    REQUIRE(woby::applySelectedDetectors(s, id, woby::UiDetectorAction::run));
    CHECK(woby::createSceneDocument(s) == clean);
    CHECK(s.sceneEditRevision == revision);
    auto* item = woby::findComparison(s, id);
    CHECK(item->detectorRequests[static_cast<size_t>(C::holes)].revision == 1);
    CHECK(item->detectorRequests[0].revision == 0);
    CHECK(item->intersectionRequestRevision == 1);
    CHECK(woby::applySelectedDetectors(s, id, woby::UiDetectorAction::cancel));
    CHECK(item->cancelIntersections);
    CHECK(item->detectorRequests[static_cast<size_t>(C::holes)].cancel);
    CHECK(woby::applySelectedDetectors(s, id, woby::UiDetectorAction::show));
    CHECK(item->settings.topologyInspection.showHoles);
    CHECK(item->settings.intersections.show);
    CHECK(item->settings.showBoundaries == clean.comparisons[0].settings.showBoundaries);
    CHECK(woby::applySelectedDetectors(s, id, woby::UiDetectorAction::automaticOn));
    CHECK(woby::diagnosticAutoUpdate(item->settings, C::holes));
    CHECK(woby::diagnosticAutoUpdate(item->settings, C::selfIntersections));
    const auto before = woby::createSceneDocument(s);
    item->detectorSelection.ids.push_back(999);
    CHECK_FALSE(woby::applySelectedDetectors(s, id, woby::UiDetectorAction::hide));
    CHECK(woby::createSceneDocument(s) == before);
}

TEST_CASE("bulk view selection does not restore scenes and batch CRUD is undoable")
{
    BulkFixture f;
    auto& s = f.state;
    const auto first = woby::createView(s);
    woby::setShowGrid(s, true);
    const auto second = woby::createView(s);
    const std::array ids{first, second};
    const auto clean = woby::createSceneDocument(s);
    woby::selectViewRows(s, ids);
    CHECK(s.showGrid);
    CHECK(s.activeViewId == second);
    CHECK(woby::createSceneDocument(s) == clean);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, s);
    REQUIRE(woby::duplicateSelectedViews(s));
    REQUIRE(s.views.size() == 4);
    CHECK(s.viewSelection.ids.size() == 2);
    woby::recordSceneHistory(history, s);
    CHECK(history.snapshots.size() == 2);
    undo(history, s, clean);
    CHECK(s.views.size() == 2);
    woby::selectViewRows(s, ids);
    REQUIRE(woby::deleteSelectedViews(s));
    CHECK(s.views.empty());
    CHECK(s.activeViewId == 0);
    CHECK(s.showGrid);
    woby::recordSceneHistory(history, s);
    undo(history, s, clean);
    CHECK(woby::createSceneDocument(s) == clean);
}

TEST_CASE("bulk file removal expands folders deduplicates and rejects part or stale selections")
{
    BulkFixture f;
    auto& s = f.state;
    const auto analysis = f.analysis();
    const auto first = s.files[0].objectId, second = s.files[1].objectId, survivor = s.files[2].objectId;
    const auto part = s.files[0].groupSettings[0].objectId;
    s.selectedSceneObjects = {s.sceneNodes[0].objectId, first};
    CHECK(woby::selectedRemovalFiles(s) == std::vector<woby::SceneObjectId>{first, second});
    s.selectedSceneObjects = {first, part};
    CHECK(woby::selectedRemovalFiles(s) == std::vector<woby::SceneObjectId>{first});
    s.selectedSceneObjects = {part};
    CHECK(woby::selectedRemovalFiles(s).empty());
    s.selectedSceneObjects = {first, s.files[1].groupSettings[0].objectId};
    CHECK(woby::selectedRemovalFiles(s).empty());
    s.selectedSceneObjects = {s.sceneNodes[0].objectId, first, second, part, s.files[1].groupSettings[0].objectId};
    CHECK(woby::selectedRemovalFiles(s) == std::vector<woby::SceneObjectId>{first, second});
    const auto before = woby::createSceneDocument(s);
    const std::array bad{first, woby::SceneObjectId{999999}};
    CHECK_FALSE(woby::removeFilesFromState(s, bad));
    CHECK(woby::createSceneDocument(s) == before);
    const std::array ids{first, second, first};
    REQUIRE(woby::removeFilesFromState(s, ids));
    REQUIRE(s.files.size() == 1);
    CHECK(s.files[0].objectId == survivor);
    CHECK(woby::missingComparisonPartCount(s, analysis) == 2);
    CHECK(s.selectedSceneObjects.empty());
    REQUIRE(s.sceneNodes.size() == 1);
    CHECK(s.sceneNodes[0].fileIndex == 0);
}

TEST_CASE("bulk scene edits persist while every list selection remains transient")
{
    BulkFixture f;
    auto& s = f.state;
    const auto analysis = f.analysis();
    const auto section = woby::createCrossSection(s, {}, {s.files[0].groupSettings[0].objectId, s.files[1].groupSettings[0].objectId});
    s.selectedSceneObjects = {section};
    REQUIRE(woby::setSelectedAnnotationStyle(s, true, 7.0f));
    REQUIRE(woby::duplicateSceneSelection(s));
    REQUIRE(woby::setSceneSelectionVisible(s, false));
    const auto view = woby::createView(s);
    woby::selectViewRows(s, std::array{view});
    woby::selectAnalysisInputRows(s, analysis, woby::ComparisonSide::a, std::array{s.files[0].objectId});
    woby::selectDetectorRows(s, analysis, std::array<uint64_t, 1>{1});
    REQUIRE(woby::applySelectedAnalysisInputs(s, analysis, woby::ComparisonSide::a, woby::UiInputAction::disable));
    REQUIRE(woby::applySelectedDetectors(s, analysis, woby::UiDetectorAction::show));
    const auto document = woby::createSceneDocument(s);
    const auto path = f.root / "bulk.woby";
    woby::writeSceneDocument(path, document);
    auto restored = woby::prepareSceneReplacement(s, s.files, woby::readSceneDocument(path));
    CHECK(woby::createSceneDocument(restored) == document);
    CHECK(restored.selectedSceneObjects.empty());
    CHECK(restored.viewSelection.ids.empty());
    CHECK(restored.sceneSelectionAnchor == 0);
    REQUIRE(restored.comparisons.size() == 1);
    CHECK(restored.comparisons[0].inputSelection[0].ids.empty());
    CHECK(restored.comparisons[0].detectorSelection.ids.empty());
    REQUIRE(restored.annotations.size() == 2);
    CHECK(restored.annotations[0].settings.locked);
    CHECK(restored.annotations[0].settings.width == 7);
    CHECK_FALSE(restored.annotations[1].settings.visible);
}

TEST_CASE("bulk removal remaps nonadjacent files and restores all references in one undo")
{
    BulkFixture f;
    auto& s = f.state;
    const auto analysis = f.analysis();
    woby::createView(s);
    const auto originalFiles = s.files;
    const auto clean = woby::createSceneDocument(s);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, s);
    const std::array ids{s.files[0].objectId, s.files[2].objectId};
    REQUIRE(woby::removeFilesFromState(s, ids));
    REQUIRE(s.files.size() == 1);
    CHECK(s.files[0].objectId == originalFiles[1].objectId);
    REQUIRE(s.sceneNodes.size() == 1);
    REQUIRE(s.sceneNodes[0].children.size() == 1);
    CHECK(s.sceneNodes[0].children[0].fileIndex == 0);
    CHECK(woby::missingComparisonPartCount(s, analysis) == 2);
    woby::recordSceneHistory(history, s);
    CHECK(history.snapshots.size() == 2);
    const auto removed = woby::createSceneDocument(s);
    auto restored = woby::prepareSceneHistoryStep(history, s, clean, false, originalFiles);
    REQUIRE(restored);
    woby::commitSceneHistoryStep(history, s, std::move(*restored), false);
    CHECK(woby::sceneContentEqual(woby::createSceneDocument(s), clean));
    CHECK(woby::createSceneDocument(s).camera == removed.camera); // Ordinary undo retains navigation.
    CHECK(woby::missingComparisonPartCount(s, analysis) == 0);
    undo(history, s, clean, true);
    CHECK(woby::createSceneDocument(s) == removed);
}

TEST_CASE("bulk annotation color rejects cross sections without partial edits")
{
    BulkFixture f;
    const auto surface = f.annotation();
    const auto section = woby::createCrossSection(f.state, {}, {f.state.files[0].groupSettings[0].objectId, f.state.files[1].groupSettings[0].objectId});
    f.state.selectedSceneObjects = {surface, section};
    const auto before = woby::createSceneDocument(f.state);
    CHECK_FALSE(woby::setSelectedAnnotationStyle(f.state, true, 5.0f, std::array<float, 4>{1, 0, 0, 1}));
    CHECK(woby::createSceneDocument(f.state) == before);
    CHECK(woby::setSelectedAnnotationStyle(f.state, true, 10.0f));
    CHECK(woby::findAnnotation(f.state, surface)->settings.width == 10);
    CHECK(woby::findAnnotation(f.state, section)->settings.width == 8);
}
