#include "automation_registry.h"
#include "background_load.h"
#include "command_line.h"
#include "control_scene.h"
#include "importer_host.h"
#include "model_load.h"
#include "scene_dimensions.h"
#include "scene_history.h"
#include "scene_pick.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <limits>

namespace {
struct LineFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-lines-" + woby::automationRandomHex(8));
    LineFixture() { std::filesystem::create_directory(root); woby::unloadImporters(); woby::loadImporter(WOBY_TEST_LINES_IMPORTER); }
    ~LineFixture() { woby::unloadImporters(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path file(const std::string& name) const {
        const auto path = root / (name + ".wline"); std::ofstream(path) << "fixture"; return path;
    }
    woby::UiState scene(const std::string& name = "mixed") const {
        auto loaded = woby::loadModelBatchCpu({file(name)}, 0, {}, {});
        if (loaded.files.size() != 1) { throw std::runtime_error(loaded.lastError); }
        woby::UiState state; state.files = std::move(loaded.files);
        woby::appendDefaultSceneNodesForFiles(state, 0); return state;
    }
};
const auto formatId = [](woby::SceneObjectId id) { return std::to_string(id); };
}

TEST_CASE("Line importer supports mixed and line-only hierarchies with owned geometry")
{
    LineFixture f;
    for (const auto* name : {"mixed", "lineonly"}) {
        auto state = f.scene(name);
        const auto& file = state.files[0]; const auto& mesh = file.mesh;
        const bool only = std::string(name) == "lineonly";
        CHECK_FALSE(woby::empty(mesh));
        CHECK(mesh.indices.size() == (only ? 0 : 6));
        CHECK(mesh.lineIndices == std::vector<uint32_t>{4,5,6,7});
        REQUIRE(mesh.nodes.size() == (only ? 2 : 3));
        const auto first = only ? 0u : 1u;
        CHECK(mesh.nodes[first].name == "rear-curve");
        CHECK(mesh.nodes[first].displayName == "Rear curve");
        CHECK(mesh.nodes[first].indexCount == 0);
        CHECK(mesh.nodes[first].lineIndexCount == 2);
        CHECK_FALSE(mesh.nodes[first].hasTexcoords);
        CHECK(file.groupSettings[first].color == std::array<float,4>{0,1,0,1});
        CHECK(file.groupSettings[first].localBounds.min[0] == -1.5f);
        CHECK(file.groupSettings[first].localBounds.max[0] == 1.5f);
        CHECK(file.groupSettings[first].localBounds.center[2] == -.25f);
        CHECK(woby::originalMeshBounds(mesh, &mesh.nodes[first])[0][2] == -.25);
        CHECK(woby::nodeCenter(mesh, mesh.nodes[first])[2] == -.25f);
        CHECK(woby::comparisonObjectParts(state, {file.objectId}).size() == (only ? 0 : 1));
        CHECK(woby::comparisonObjectParts(state, {file.groupSettings[first].objectId}).empty());
        CHECK_FALSE(woby::setObjectUvGrid(state, {file.groupSettings[first].objectId}, true, {}, {}));
        const auto details = woby::controlObjectDetails(state, file.groupSettings[first].objectId, formatId);
        CHECK(details.at("primitive") == "lines");
        CHECK(details.at("lineSegmentCount") == 1);
        CHECK(details.at("triangleCount") == 0);
        const auto tree = woby::controlSceneTree(state, formatId);
        CHECK(tree[0]["children"][0]["children"].back()["name"] == "Boundary curves");
    }
    CHECK(f.scene("absent").files[0].mesh.lineIndices.empty());
    const auto hidden = f.scene("hidden");
    CHECK_FALSE(hidden.files[0].groupSettings.back().visible);
}

TEST_CASE("Line importer rejects malformed buffers and releases errors and canceled imports")
{
    LineFixture f;
    for (const auto* name : {"invalid_index", "odd_count", "null_buffer", "short_lines", "failure"}) {
        CHECK_THROWS_AS((void)woby::loadModel(f.file(name)), std::runtime_error);
        REQUIRE(f.scene().files[0].mesh.lineIndices.size() == 4);
    }
    bool canceled = false;
    woby::ImportCallbacks callbacks;
    callbacks.progress = [&](float) { canceled = true; }; callbacks.canceled = [&] { return canceled; };
    CHECK(woby::importModel(f.file("canceled"), {}, callbacks).canceled);
    CHECK(f.scene().files[0].mesh.lineIndices.size() == 4);
}

TEST_CASE("Line buffer validation enforces pair partitions unique identities and limits")
{
    const WobyImportVertex vertices[] = {{{0,0,0},{},{}},{{1,1,0},{},{}},{{2,0,0},{},{}}};
    uint32_t indices[] = {0,1,1,2};
    WobyImportGroup groups[] = {{"First",0,2,0,{}},{"Second",2,2,0,{}}};
    WobyImportResult result{}; result.struct_size = sizeof(result);
    result.vertices = vertices; result.vertex_count = 3;
    WobyImportLines lines{sizeof(WobyImportLines),indices,4,groups,2};
    SUBCASE("valid line-only result") {
        const auto mesh = woby::copyImportedMesh(result, nullptr, &lines);
        CHECK(mesh.nodes.size() == 2); CHECK(mesh.indices.empty()); CHECK(mesh.bounds.max[0] == 2); return;
    }
    SUBCASE("implicit Lines group") {
        lines.groups = nullptr; lines.group_count = 0;
        const auto mesh = woby::copyImportedMesh(result, nullptr, &lines);
        CHECK(mesh.nodes.size() == 1); CHECK(mesh.nodes[0].name == "Lines"); CHECK(mesh.nodes[0].lineIndexCount == 4); return;
    }
    SUBCASE("odd group offset") { groups[1].index_offset = 1; }
    SUBCASE("odd group length") { groups[0].index_count = 1; }
    SUBCASE("empty group") { groups[0].index_count = 0; }
    SUBCASE("uncovered lines") { lines.group_count = 1; }
    SUBCASE("overlapping groups") { groups[1].index_offset = 0; }
    SUBCASE("duplicate identities") { groups[1].name = groups[0].name; }
    SUBCASE("null groups") { lines.groups = nullptr; }
    SUBCASE("invalid flags") { groups[0].flags = 8; }
    SUBCASE("invalid color") { groups[0].flags = WOBY_IMPORT_GROUP_HAS_COLOR; groups[0].color[0] = 2; }
    SUBCASE("too many groups") { lines.group_count = 100001; }
    SUBCASE("implicit triangle group also counts toward group limit") {
        const uint32_t triangle[] = {0,1,2};
        result.indices = triangle; result.index_count = 3; lines.group_count = 100000;
        CHECK_THROWS_AS((void)woby::copyImportedMesh(result, nullptr, &lines), std::runtime_error); return;
    }
    SUBCASE("invalid index") { indices[3] = 99; }
    SUBCASE("missing geometry") { lines.index_count = 0; lines.group_count = 0; }
    SUBCASE("hierarchy omits line groups") {
        const WobyImportHierarchyNode node{"First",WOBY_IMPORT_NO_PARENT,0};
        const WobyImportHierarchy hierarchy{sizeof(WobyImportHierarchy),&node,1};
        CHECK_THROWS_AS((void)woby::copyImportedMesh(result, &hierarchy, &lines), std::runtime_error); return;
    }
    SUBCASE("triangle and line group names share one namespace") {
        const uint32_t triangle[] = {0,1,2};
        const WobyImportGroup triangleGroup{"First",0,3,0,{}};
        result.indices = triangle; result.index_count = 3; result.groups = &triangleGroup; result.group_count = 1;
        CHECK_THROWS_AS((void)woby::copyImportedMesh(result, nullptr, &lines), std::runtime_error); return;
    }
    CHECK_THROWS_AS((void)woby::copyImportedMesh(result, nullptr, &lines), std::runtime_error);
}

TEST_CASE("Line appearance propagates to descendants and preserves child overrides in scenes views and history")
{
    LineFixture f; auto state = f.scene();
    const auto clean = woby::createSceneDocument(state);
    const auto parent = state.sceneNodes[0].children[0].objectId;
    const auto first = state.files[0].groupSettings[1].objectId;
    const auto second = state.files[0].groupSettings[2].objectId;
    woby::SceneHistory history; woby::resetSceneHistory(history, state);
    woby::selectSceneObject(state, parent);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::red, .7f);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::lineWidth, 8);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::lineDepthTest, 0);
    CHECK(state.files[0].groupSettings[0].lines == woby::LineStyle{});
    CHECK(state.files[0].groupSettings[1].lines == woby::LineStyle{8,false});
    CHECK(state.files[0].groupSettings[2].color[0] == .7f);
    woby::setSelectedObjectsVisible(state, false);
    CHECK_FALSE(state.files[0].groupSettings[1].visible);
    woby::setSelectedObjectsVisible(state, true);
    woby::selectSceneObject(state, second);
    CHECK_FALSE(woby::selectedObjectProperty(state, woby::UiObjectProperty::solidMesh).available);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::lineWidth, 3);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::lineDepthTest, 1);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::red, .2f);
    woby::setSelectedObjectsVisible(state, false);
    const auto view = woby::createView(state);
    const auto saved = woby::createSceneDocument(state);
    CHECK(saved.files[0].groups[1].lineGroup);
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false); REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK(woby::createSceneDocument(state) == clean);
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true); REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(woby::createSceneDocument(state) == saved);
    REQUIRE(woby::setObjectLineStyle(state, {parent}, 1.0f, true));
    woby::applyView(state, view);
    CHECK(woby::createSceneDocument(state) == saved);
    const auto path = f.root / "scene.woby";
    woby::writeSceneDocument(path, saved);
    auto loaded = woby::loadSceneCpu(path, {}, {});
    auto restored = woby::prepareSceneReplacement(state, std::move(loaded.files), loaded.document);
    CHECK(woby::createSceneDocument(restored) == saved);
    CHECK(restored.files[0].groupSettings[1].lines == woby::LineStyle{8,false});
    CHECK(restored.files[0].groupSettings[2].lines == woby::LineStyle{3,true});
    CHECK_FALSE(restored.files[0].groupSettings[2].visible);
    auto changed = saved.files[0]; changed.groups[1].lineGroup = false;
    CHECK_THROWS_AS(woby::applySceneFileRecord(restored.files[0], changed), std::runtime_error);
    woby::selectSceneObject(state, first);
    woby::resetSelectedObjectProperties(state, woby::UiPropertyGroup::appearance);
    CHECK(state.files[0].groupSettings[1].lines == woby::LineStyle{});
}

TEST_CASE("Line styles clamp at operation and scene load boundaries and ctl validates eligibility atomically")
{
    LineFixture f; auto state = f.scene();
    const auto clean = woby::createSceneDocument(state);
    const auto first = state.files[0].groupSettings[1].objectId;
    CHECK(woby::setObjectLineStyle(state, {first}, 100.0f, false));
    CHECK(state.files[0].groupSettings[1].lines == woby::LineStyle{12,false});
    CHECK_FALSE(woby::setObjectLineStyle(state, {first}, std::numeric_limits<float>::quiet_NaN(), true));
    CHECK(state.files[0].groupSettings[1].lines == woby::LineStyle{12,false});
    const auto method = *woby::findControlMethod("render.set");
    auto command = woby::parseControlOperation(method, {{"target",formatId(first)},{"lineWidth",4},{"lineDepthTest",true}});
    command.objectId = first;
    CHECK(woby::controlOperationParams(command)["lineWidth"] == 4);
    woby::applyControlSceneOperation(state, clean, command, formatId, 200, 800);
    CHECK(woby::controlObjectDetails(state, first, formatId).at("settings").at("lineWidth") == 4);
    command.objectId = state.files[0].groupSettings[0].objectId;
    command.solid = false;
    const auto before = woby::createSceneDocument(state);
    CHECK_THROWS_AS((void)woby::applyControlSceneOperation(state, clean, command, formatId, 200, 800), std::invalid_argument);
    CHECK(woby::createSceneDocument(state) == before);
    auto record = before.files[0]; record.groups[1].settings.lines.width = -1;
    woby::applySceneFileRecord(state.files[0], record);
    CHECK(state.files[0].groupSettings[1].lines.width == 1);
    const auto path = f.root / "clamp.woby";
    auto saved = before; saved.files[0].groups[1].settings.lines.width = 99;
    woby::writeSceneDocument(path, saved);
    CHECK(woby::readSceneDocument(path).files[0].groups[1].settings.lines.width == 12);
    auto lineOnly = f.scene("lineonly");
    CHECK_FALSE(woby::fileHasComparableParts(lineOnly.files[0]));
}

TEST_CASE("Imported line picking respects width depth transforms and selection bounds")
{
    woby::Mesh mesh;
    mesh.vertices = {{{-.8f,-.8f,.4f},{},{}},{{.8f,-.8f,.4f},{},{}},{{0,.8f,.4f},{},{}},
        {{-.6f,0,.7f},{},{}},{{.6f,0,.7f},{},{}}};
    mesh.indices = {0,1,2}; mesh.lineIndices = {3,4};
    mesh.nodes = {{"surface",0,3},{"line",0,0}}; mesh.nodes[1].lineIndexCount = 2;
    woby::finalizeMesh(mesh, true);
    woby::UiState state;
    state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(state,0);
    auto& groups = state.files[0].groupSettings;
    woby::ScenePickView view; bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    view.width = view.height = 100;
    auto pick = [&](float y) { return woby::pickSceneObject(woby::scenePickParts(state), view, {50,y}); };
    CHECK(pick(50) == groups[0].objectId);
    woby::setGroupLineStyle(groups[1], {12,false});
    CHECK(pick(50) == groups[1].objectId); CHECK(pick(55) == groups[1].objectId);
    woby::setGroupLineStyle(groups[1], {2,false});
    CHECK(pick(55) == groups[0].objectId);
    woby::selectSceneObject(state, groups[1].objectId);
    const auto parts = woby::scenePickParts(state);
    CHECK(woby::sceneSelectionLines(parts).size() == 24);
    const auto dimensions = woby::sceneDimensions(parts); REQUIRE(dimensions);
    CHECK(dimensions->lengths[0] == doctest::Approx(1.2));
    woby::setGroupTranslation(groups[1], {0,.4f,0});
    CHECK(pick(30) == groups[1].objectId); CHECK(pick(50) == groups[0].objectId);
    woby::setGroupVisible(state, state.files[0], groups[1], false);
    CHECK(pick(30) == groups[0].objectId);
}

TEST_CASE("Line picking clips near-plane crossings and skips degenerate and offscreen segments")
{
    woby::Mesh mesh;
    mesh.vertices = {{{-.8f,0,-.5f},{},{}},{{.8f,0,.5f},{},{}}};
    mesh.lineIndices = {0,1};
    woby::ScenePickPart part;
    part.objectId = 1; part.mesh = &mesh; part.lineIndexCount = 2;
    part.lineWidth = 4; part.edges = true; part.solid = false; part.edgeXray = false;
    bx::mtxIdentity(part.model.data());
    woby::ScenePickView view; bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    view.width = view.height = 100;
    auto pick = [&](float x) { return woby::pickSceneObject(std::span(&part, 1), view, {x,50}); };
    CHECK(pick(70) == 1); CHECK(pick(30) == woby::invalidSceneObjectId);
    mesh.lineIndices[1] = 0;
    CHECK(pick(10) == woby::invalidSceneObjectId);
    mesh.lineIndices[1] = 1;
    mesh.vertices[1].position[2] = -.5f;
    CHECK(pick(70) == woby::invalidSceneObjectId);
    mesh.vertices[0].position = {2,0,.5f}; mesh.vertices[1].position = {3,0,.5f};
    CHECK(pick(99) == woby::invalidSceneObjectId);
}
