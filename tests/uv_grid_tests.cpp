#include "ui_operations.h"
#include "scene_history.h"
#include "control_scene.h"
#include "automation_registry.h"
#include "command_line.h"
#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "uv_analysis.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <limits>

namespace {
struct UvFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-uv-" + woby::automationRandomHex(8));
    woby::UiState state;
    UvFixture()
    {
        std::filesystem::create_directory(root);
        woby::Mesh mesh;
        mesh.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
        mesh.indices = {0, 1, 2, 0, 1, 2, 0, 1, 2};
        mesh.nodes = {{"UV A", 0, 3}, {"UV B", 3, 3}, {"No UV", 6, 3}};
        mesh.nodes[0].hasTexcoords = mesh.nodes[1].hasTexcoords = true;
        mesh.bounds = woby::calculateBounds(mesh.vertices);
        state.files.push_back(woby::createUiFileState(root / "mesh.obj", std::move(mesh), 0));
        woby::appendFolderTreeSceneNode(state, root, 0, 1);
        woby::clearSceneDirty(state);
    }
    ~UvFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
}

TEST_CASE("UV edits expand parents skip missing UVs and preserve per-child overrides")
{
    UvFixture f;
    auto& state = f.state;
    auto& parts = state.files[0].groupSettings;
    woby::selectSceneObject(state, state.sceneNodes[0].objectId);
    REQUIRE(woby::selectedObjectProperty(state, woby::UiObjectProperty::uvGrid).available);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvGrid, 1);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvDensityU, 17);
    CHECK(parts[0].uvGrid == woby::UvGridSettings{true, 17, 10});
    CHECK(parts[1].uvGrid == parts[0].uvGrid);
    CHECK(parts[2].uvGrid == woby::UvGridSettings{});
    woby::selectSceneObject(state, parts[1].objectId);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvDensityU, 4);
    woby::selectSceneObject(state, state.files[0].objectId);
    CHECK(woby::selectedObjectProperty(state, woby::UiObjectProperty::uvDensityU).mixed);
    const auto saved = woby::createSceneDocument(state);
    woby::applySceneFileRecord(state.files[0], saved.files[0]);
    CHECK(parts[0].uvGrid.densityU == 17);
    CHECK(parts[1].uvGrid.densityU == 4);
    woby::selectSceneObject(state, parts[2].objectId);
    CHECK_FALSE(woby::selectedObjectProperty(state, woby::UiObjectProperty::uvGrid).available);
    const auto before = woby::createSceneDocument(state);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvGrid, 1);
    CHECK(woby::createSceneDocument(state) == before);
    woby::selectSceneObject(state, parts[0].objectId);
    woby::resetSelectedObjectProperties(state, woby::UiPropertyGroup::appearance);
    CHECK(parts[0].uvGrid == woby::UvGridSettings{});
    CHECK(parts[1].uvGrid.densityU == 4);
}

TEST_CASE("UV density validation and no-op notifications are consistent")
{
    UvFixture f;
    auto& state = f.state;
    REQUIRE(woby::setObjectUvGrid(state, {}, true, -2.0f, 1e9f));
    CHECK(state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, .1f, 1000});
    const auto revision = state.sceneEditRevision;
    REQUIRE(woby::setObjectUvGrid(state, {}, true, .1f, 1000.0f));
    CHECK(state.sceneEditRevision == revision);
    CHECK_FALSE(woby::setObjectUvGrid(state, {}, false, std::numeric_limits<float>::infinity(), {}));
    CHECK(state.sceneEditRevision == revision);
    CHECK(woby::normalizedUvGrid({true, std::numeric_limits<float>::quiet_NaN(), -1}) == woby::UvGridSettings{true, 10, .1f});
}

TEST_CASE("UV settings round trip through scenes saved views and history")
{
    UvFixture f;
    auto& state = f.state;
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    REQUIRE(woby::setObjectUvGrid(state, {}, true, 7.0f, 13.0f));
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK_FALSE(state.files[0].groupSettings[0].uvGrid.enabled);
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, 7, 13});
    const auto view = woby::createView(state);
    REQUIRE(woby::setObjectUvGrid(state, {}, false, 20.0f, 30.0f));
    woby::applyView(state, view);
    CHECK(state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, 7, 13});
    const auto document = woby::createSceneDocument(state);
    const auto path = f.root / "uv.woby";
    woby::writeSceneDocument(path, document);
    auto restored = woby::readSceneDocument(path);
    for (auto& file : restored.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
    CHECK(restored == document);
    std::ofstream(path) << "version = 6\n[[files]]\npath = \"mesh.obj\"\n[[files.groups]]\nname = \"UV A\"\n";
    CHECK(woby::readSceneDocument(path).files[0].groups[0].settings.uvGrid == woby::UvGridSettings{});
    std::ofstream(path) << "version = 6\n[[files]]\npath = \"mesh.obj\"\n[[files.groups]]\nname = \"UV A\"\n"
        "uv_grid = true\nuv_density_u = -4\nuv_density_v = 100000\n";
    CHECK(woby::readSceneDocument(path).files[0].groups[0].settings.uvGrid == woby::UvGridSettings{true, .1f, 1000});
}

TEST_CASE("UV control commands parse report settings and reject unavailable targets atomically")
{
    UvFixture f;
    std::vector<std::string> words = {"woby", "ctl", "--instance", "uv", "render", "set", "scene",
        "--uv-grid", "true", "--uv-density-u", "5", "--uv-density-v", "9"};
    std::vector<char*> argv;
    for (auto& word : words) { argv.push_back(word.data()); }
    const auto parsed = woby::parseCommandLine(static_cast<int>(argv.size()), argv.data());
    const auto& method = woby::controlMethod(woby::ControlAction::render);
    const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
    const auto clean = woby::createSceneDocument(f.state);
    auto command = woby::parseControlOperation(method, {{"target", "scene"}, {"uvGrid", true}, {"uvDensityU", 5}, {"uvDensityV", 9}});
    CHECK(parsed.control.command == woby::ControlCommand::operation);
    CHECK(parsed.control.operation.uvGrid == true);
    CHECK(parsed.control.operation.uvDensityU == 5);
    CHECK(parsed.control.operation.uvDensityV == 9);
    CHECK(woby::controlOperationParams(command)["uvDensityV"] == 9);
    (void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800);
    CHECK(f.state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, 5, 9});
    CHECK_FALSE(f.state.files[0].groupSettings[2].uvGrid.enabled);
    command.objectId = f.state.files[0].groupSettings[2].objectId;
    command.target = format(command.objectId);
    command.solid = false;
    const auto before = woby::createSceneDocument(f.state);
    CHECK_THROWS_AS((void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800), std::invalid_argument);
    CHECK(woby::createSceneDocument(f.state) == before);
    CHECK_THROWS(woby::parseControlOperation(method, {{"target", "scene"}, {"uvDensityU", "nan"}}));
}

TEST_CASE("UV layout preserves seams tile offsets and proportions while omitting unavailable parts")
{
    UvFixture f;
    auto source = f.state.files[0].mesh;
    source.vertices[0].texcoord = {-1, 0};
    source.vertices[1].texcoord = {3, 0};
    source.vertices[2].texcoord = {-1, 2};
    // A seam has coincident source positions but a distinct UV coordinate.
    source.vertices.push_back(source.vertices[0]);
    source.vertices.back().texcoord = {1, 1};
    source.indices[3] = 3;
    const auto result = woby::uvLayoutMesh(source, woby::SceneUpAxis::y);
    REQUIRE(result.nodes.size() == 2);
    REQUIRE(result.indices.size() == 6);
    REQUIRE(result.vertices.size() == 4);
    CHECK(result.nodes[1].indexOffset == 3);
    CHECK(result.nodes[0].hasTexcoords);
    CHECK(result.vertices[0].texcoord == std::array<float, 2>{-1, 0});
    CHECK(result.vertices[0].position != result.vertices[3].position);
    CHECK(result.vertices[0].position[1] > result.vertices[2].position[1]);
    CHECK(result.bounds.max[0] - result.bounds.min[0] == doctest::Approx(1));
    CHECK(result.bounds.max[1] - result.bounds.min[1] == doctest::Approx(.5));
    CHECK(result.bounds.min[2] == result.bounds.max[2]);
    CHECK(source.vertices[0].position == source.vertices[3].position);
    CHECK(result.precisePositions.size() == result.vertices.size());
    for (auto& node : source.nodes) { node.hasTexcoords = false; }
    CHECK(woby::uvLayoutMesh(source, woby::SceneUpAxis::y).indices.empty());
    source.nodes[0].hasTexcoords = true;
    for (auto& vertex : source.vertices) { vertex.texcoord = {0, 0}; }
    const auto constant = woby::uvLayoutMesh(source, woby::SceneUpAxis::y);
    REQUIRE(constant.indices.size() == 3);
    CHECK(woby::finitePosition(constant.bounds.min));
    CHECK(woby::finitePosition(constant.bounds.max));
}

TEST_CASE("UV analyses keep source appearance independent and use only source computation")
{
    UvFixture f;
    auto& state = f.state;
    const auto fileId = state.files[0].objectId;
    const auto before = woby::createSceneDocument(state).files;
    woby::selectSceneObject(state, fileId);
    REQUIRE(woby::compareSceneSelection(state, woby::AnalysisType::uv));
    const auto id = state.comparisons.back().objectId;
    CHECK(state.comparisons.back().b.empty());
    CHECK(woby::createSceneDocument(state).files == before);
    auto settings = woby::comparisonSettings(state, id);
    CHECK(settings.type == woby::AnalysisType::uv);
    CHECK(woby::effectiveComparisonSettings(state, id).mode == woby::ComparisonMode::original);
    CHECK(woby::requestedComparisonStages(settings, true, true) == woby::comparisonSource);
    for (size_t i = 0; i < woby::diagnosticCategoryCount; ++i) {
        CHECK_FALSE(woby::diagnosticAutoUpdate(settings, static_cast<woby::DiagnosticCategory>(i)));
    }
    const auto layout = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    REQUIRE(layout.nodes.size() == 2);
    const auto bounds = woby::comparisonDisplayBounds(state, id);
    REQUIRE(bounds);
    for (size_t k = 0; k < 3; ++k) {
        CHECK(bounds->min[k] == doctest::Approx(layout.bounds.min[k] + state.comparisons.back().translation[k]));
        CHECK(bounds->max[k] == doctest::Approx(layout.bounds.max[k] + state.comparisons.back().translation[k]));
    }
    const auto signature = woby::comparisonGeometrySignature(state, id);
    settings.uvGrid.densityU = -1;
    settings.uvGrid.densityV = 1e9f;
    woby::setComparisonSettings(state, settings, id);
    CHECK(woby::comparisonSettings(state, id).uvGrid == woby::UvGridSettings{true, .1f, 1000});
    CHECK(woby::comparisonGeometrySignature(state, id) == signature);
    settings.uvView = woby::UvView::surface;
    woby::setComparisonSettings(state, settings, id);
    CHECK(woby::comparisonGeometrySignature(state, id) != signature);
    const auto surface = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    REQUIRE(surface.nodes.size() == 3);
    CHECK(surface.nodes[0].hasTexcoords);
    CHECK_FALSE(surface.nodes[2].hasTexcoords);
    CHECK(surface.vertices[1].texcoord == state.files[0].mesh.vertices[1].texcoord);
    woby::setComparisonObjects(state, {fileId}, woby::ComparisonSide::b, true, id);
    woby::swapComparisonGroups(state, id);
    CHECK(state.comparisons.back().b.empty());
    CHECK(woby::createSceneDocument(state).files == before);
}

TEST_CASE("UV analysis type and view survive persistence saved views and undo redo")
{
    UvFixture f;
    auto& state = f.state;
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    const auto id = woby::createComparison(state, woby::AnalysisType::uv);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK(state.comparisons.empty());
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(woby::comparisonSettings(state, id).type == woby::AnalysisType::uv);
    const auto view = woby::createView(state);
    auto settings = woby::comparisonSettings(state, id);
    settings.uvView = woby::UvView::surface;
    settings.uvGrid = {false, 7, 13};
    woby::setComparisonSettings(state, settings, id);
    woby::applyView(state, view);
    CHECK(woby::comparisonSettings(state, id).uvView == woby::UvView::layout);
    CHECK(woby::comparisonSettings(state, id).uvGrid == woby::UvGridSettings{true, 10, 10});
    woby::setComparisonSettings(state, settings, id);
    const auto document = woby::createSceneDocument(state);
    const auto path = f.root / "uv-analysis.woby";
    woby::writeSceneDocument(path, document);
    auto restored = woby::readSceneDocument(path);
    for (auto& file : restored.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
    CHECK(restored == document);
    std::ofstream(path) << "version = 17\n[[analyses]]\nname = \"Legacy\"\nanalysis_enabled = true\nanalysis_mode = \"distance\"\n";
    const auto legacy = woby::readSceneDocument(path);
    REQUIRE(legacy.comparisons.size() == 1);
    CHECK(legacy.comparisons[0].settings.type == woby::AnalysisType::mesh);
}

TEST_CASE("UV layouts obey the scene up axis and refresh geometry and framing bounds")
{
    UvFixture f;
    auto& state = f.state;
    woby::setFileTranslation(state.files[0].fileSettings, {3, 4, 5});
    const auto layoutId = woby::createComparison(state, woby::AnalysisType::uv);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, layoutId);
    woby::setComparisonTranslation(state, layoutId, {-7, 8, -9});
    const auto surfaceId = woby::duplicateComparison(state, layoutId);
    auto surfaceSettings = woby::comparisonSettings(state, surfaceId);
    surfaceSettings.uvView = woby::UvView::surface;
    woby::setComparisonSettings(state, surfaceSettings, surfaceId);
    const auto meshId = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, meshId);
    const auto surfaceSignature = woby::comparisonGeometrySignature(state, surfaceId);
    const auto meshSignature = woby::comparisonGeometrySignature(state, meshId);
    auto layoutSignature = woby::comparisonGeometrySignature(state, layoutId);
    REQUIRE(surfaceSignature != 0);
    REQUIRE(meshSignature != 0);
    for (const auto up : {woby::SceneUpAxis::y, woby::SceneUpAxis::z, woby::SceneUpAxis::y}) {
        woby::setSceneUpAxis(state, up);
        CHECK(woby::comparisonGeometrySignature(state, layoutId) != layoutSignature);
        layoutSignature = woby::comparisonGeometrySignature(state, layoutId);
        CHECK(woby::comparisonGeometrySignature(state, surfaceId) == surfaceSignature);
        CHECK(woby::comparisonGeometrySignature(state, meshId) == meshSignature);
        auto refreshed = state;
        woby::recalculateSceneBounds(refreshed);
        CHECK(state.sceneBounds.min == refreshed.sceneBounds.min);
        CHECK(state.sceneBounds.max == refreshed.sceneBounds.max);
        const auto layout = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, layoutId);
        const bool yUp = up == woby::SceneUpAxis::y;
        const std::array<std::array<float, 3>, 3> positions = yUp
            ? std::array<std::array<float, 3>, 3>{{{3, 5, 5}, {4, 5, 5}, {3, 4, 5}}}
            : std::array<std::array<float, 3>, 3>{{{3, 4.5f, 5.5f}, {4, 4.5f, 5.5f}, {3, 4.5f, 4.5f}}};
        REQUIRE(layout.vertices.size() == 6);
        for (size_t i = 0; i < layout.vertices.size(); ++i) {
            CHECK(layout.vertices[i].position == positions[i % 3]);
            CHECK(layout.vertices[i].normal == (yUp ? std::array<float, 3>{0, 0, 1}
                : std::array<float, 3>{0, -1, 0}));
            CHECK(layout.vertices[i].texcoord == state.files[0].mesh.vertices[i % 3].texcoord);
            for (size_t k = 0; k < 3; ++k) {
                CHECK(layout.precisePositions[i][k] == layout.vertices[i].position[k]);
            }
        }
        const auto display = woby::comparisonDisplayBounds(state, layoutId);
        REQUIRE(display);
        woby::selectSceneObject(state, layoutId);
        const auto selected = woby::selectedSceneBounds(state);
        REQUIRE(selected);
        for (size_t k = 0; k < 3; ++k) {
            CHECK(display->min[k] == layout.bounds.min[k] + state.comparisons[0].translation[k]);
            CHECK(display->max[k] == layout.bounds.max[k] + state.comparisons[0].translation[k]);
            CHECK(selected->min[k] == display->min[k]);
            CHECK(selected->max[k] == display->max[k]);
            CHECK(state.sceneBounds.min[k] <= display->min[k]);
            CHECK(state.sceneBounds.max[k] >= display->max[k]);
        }
        CHECK(state.camera.target == state.sceneBounds.center);
        const auto revision = state.sceneEditRevision;
        woby::setSceneUpAxis(state, up);
        CHECK(state.sceneEditRevision == revision);
        CHECK(woby::comparisonGeometrySignature(state, layoutId) == layoutSignature);
    }
}

TEST_CASE("UV layout orientation survives scene reload saved views and undo redo")
{
    for (const auto up : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        UvFixture f;
        auto& state = f.state;
        const auto opposite = up == woby::SceneUpAxis::y ? woby::SceneUpAxis::z : woby::SceneUpAxis::y;
        woby::setSceneUpAxis(state, opposite);
        const auto id = woby::createComparison(state, woby::AnalysisType::uv);
        woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
        const auto clean = woby::createSceneDocument(state);
        woby::SceneHistory history;
        woby::resetSceneHistory(history, state);
        woby::setSceneUpAxis(state, up);
        REQUIRE(woby::recordSceneHistory(history, state));
        auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
        REQUIRE(undo);
        woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
        CHECK(state.upAxis == opposite);
        auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
        REQUIRE(redo);
        woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
        CHECK(state.upAxis == up);
        const auto view = woby::createView(state);
        woby::setSceneUpAxis(state, opposite);
        woby::applyView(state, view);
        CHECK(state.upAxis == up);
        const auto path = f.root / "uv-orientation.woby";
        woby::writeSceneDocument(path, woby::createSceneDocument(state));
        auto document = woby::readSceneDocument(path);
        for (auto& file : document.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
        const auto restored = woby::prepareSceneReplacement(state, state.files, document);
        CHECK(restored.upAxis == up);
        const auto checkLayout = [&](const woby::UiState& current, woby::SceneObjectId currentId) {
            const auto mesh = woby::comparisonWorldMesh(current, woby::ComparisonSide::a, currentId);
            const size_t vertical = current.upAxis == woby::SceneUpAxis::y ? 1 : 2;
            const size_t depth = current.upAxis == woby::SceneUpAxis::y ? 2 : 1;
            CHECK(mesh.bounds.max[vertical] - mesh.bounds.min[vertical] == doctest::Approx(1));
            CHECK(mesh.bounds.max[depth] == mesh.bounds.min[depth]);
            CHECK(mesh.vertices[0].position[vertical] > mesh.vertices[2].position[vertical]);
        };
        checkLayout(state, id);
        checkLayout(restored, restored.activeComparisonId);
    }
}

TEST_CASE("UV analysis controls validate types and reject incompatible edits atomically")
{
    UvFixture f;
    const auto clean = woby::createSceneDocument(f.state);
    const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
    const auto& create = woby::controlMethod(woby::ControlAction::comparisonCreate);
    CHECK_THROWS(woby::parseControlOperation(create, {{"type", "unknown"}}));
    CHECK_THROWS(woby::parseControlOperation(create, {{"type", "uv"}, {"b", "1"}}));
    auto command = woby::parseControlOperation(create, {{"type", "uv"}, {"a", format(f.state.files[0].objectId)}});
    command.aId = f.state.files[0].objectId;
    (void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800);
    const auto id = f.state.comparisons.back().objectId;
    const auto& set = woby::controlMethod(woby::ControlAction::comparisonSet);
    command = woby::parseControlOperation(set, {{"target", format(id)}, {"uvView", "surface"}, {"uvDensityU", 5}, {"uvDensityV", 9}});
    command.objectId = id;
    CHECK(woby::controlOperationParams(command)["uvView"] == "surface");
    (void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800);
    CHECK(woby::comparisonSettings(f.state, id).uvView == woby::UvView::surface);
    CHECK(woby::comparisonSettings(f.state, id).uvGrid == woby::UvGridSettings{true, 5, 9});
    const auto before = woby::createSceneDocument(f.state);
    command.mode = "distance";
    command.name = "Should not rename";
    CHECK_THROWS_AS((void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800), std::invalid_argument);
    CHECK(woby::createSceneDocument(f.state) == before);
    CHECK_THROWS(woby::parseControlOperation(set, {{"target", format(id)}, {"uvView", "invalid"}}));
}
