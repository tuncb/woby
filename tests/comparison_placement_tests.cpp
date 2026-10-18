#include "comparison_placement.h"
#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "ui_operations.h"
#include "scene_history.h"
#include "control_scene.h"
#include "command_line.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {
using namespace woby;
using Json = nlohmann::json;

void setSceneObjectProperty(UiState& state, SceneObjectId id, UiObjectProperty property, float value)
{
    selectSceneObject(state, id);
    setSelectedObjectProperty(state, property, value);
}

struct PlacementFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-placement-" + automationRandomHex(8));
    UiState state;
    PlacementFixture() {
        std::filesystem::create_directory(root);
        Mesh mesh;
        auto& geometry = editMeshGeometry(mesh);
        geometry.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{1, 1, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
        geometry.indices = {0, 1, 2, 0, 2, 3};
        geometry.nodes = {{"surface", 0, 6}};
        mesh.bounds = calculateBounds(geometry.vertices);
        for (size_t i = 0; i < 3; ++i) {
            state.files.push_back(createUiFileState(root / (std::to_string(i) + ".obj"), mesh, i));
        }
        state.files[0].fileSettings.translation = {10, 0, 0};
        state.files[1].fileSettings.translation = {15, 3, 0};
        state.files[2].fileSettings.translation = {30, 10, 2};
        appendFolderTreeSceneNode(state, root, 0, 2);
        appendDefaultSceneNodesForFiles(state, 2);
        recalculateSceneBounds(state);
    }
    ~PlacementFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    SceneObjectId create(bool multi = false, bool folder = false) {
        const auto id = createComparison(state, AnalysisType::surfaceComparison);
        setComparisonObjects(state, folder ? std::vector{state.sceneNodes[0].objectId}
            : multi ? std::vector{state.files[0].objectId, state.files[1].objectId}
            : std::vector{state.files[0].objectId}, ComparisonSide::a, true, id);
        setComparisonObjects(state, {state.files[2].objectId}, ComparisonSide::b, true, id);
        return id;
    }
};

std::vector<Coordinate> points(const Mesh& mesh)
{
    std::vector<Coordinate> result;
    for (size_t i = 0; i < meshGeometry(mesh).vertices.size(); ++i) { result.push_back(meshPosition(mesh, i)); }
    return result;
}
std::vector<Coordinate> points(const UiState& state, SceneObjectId id, ComparisonSide side = ComparisonSide::a)
{
    return points(comparisonWorldMesh(state, side, id));
}
void checkPoint(const Coordinate& actual, const Coordinate& expected)
{
    for (size_t k = 0; k < 3; ++k) { CHECK(actual[k] == doctest::Approx(expected[k]).epsilon(2e-6).scale(1)); }
}
void checkPoints(const std::vector<Coordinate>& actual, const std::vector<Coordinate>& expected)
{
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) { checkPoint(actual[i], expected[i]); }
}
void unlock(UiState& state, SceneObjectId id, bool align = true)
{
    auto settings = comparisonSettings(state, id);
    settings.lockPlacement = false; settings.alignOrigins = align;
    setComparisonSettings(state, settings, id);
}
Json command(UiState& state, const std::string& method, Json params, SceneObjectId id = 0)
{
    const auto format = [](SceneObjectId value) { return std::to_string(value); };
    if (id) { params["target"] = format(id); }
    auto operation = parseControlOperation(*findControlMethod(method), params);
    operation.objectId = id;
    if (operation.a) { operation.aId = std::stoull(*operation.a); }
    if (operation.b) { operation.bId = std::stoull(*operation.b); }
    if (operation.originA) { operation.originAId = std::stoull(*operation.originA); }
    if (operation.originB) { operation.originBId = std::stoull(*operation.originB); }
    return applyControlSceneOperation(state, createSceneDocument(state), operation, format, 200, 800);
}
}

TEST_CASE("locked rotated comparison placement rebases input coordinates before its transform")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    auto& file = state.files[0];
    file.fileSettings.rotationDegrees = {17,-31,74};
    file.fileSettings.scale = 2;
    const auto transforms = comparisonSourceTransforms(state);
    const auto found = std::find_if(transforms.begin(),transforms.end(),[&](const auto& t) {
        return t.objectId == file.groupSettings[0].objectId;
    });
    REQUIRE(found != transforms.end());
    const auto placement = comparisonPartPlacement(state,*found,{});
    const Coordinate oldPoint{.25,.5,0};
    const auto expected = transformCoordinate(placement.model.data(),oldPoint);
    file.mesh.origin = {100,-200,300};
    CoordinateMatrix parent, model;
    resolveComparisonPlacement(state,file,placement,parent,model);
    checkPoint(transformCoordinate(model.data(),relativePosition(oldPoint,file.mesh.origin)),expected);
}

TEST_CASE("new surface comparisons align and lock while source transforms remain editable")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto sourceSettings = state.files[0].fileSettings;
    const auto id = fixture.create();
    CHECK(comparisonSettings(state, id).alignOrigins);
    CHECK(comparisonSettings(state, id).lockPlacement);
    CHECK(state.files[0].fileSettings.translation == sourceSettings.translation);
    const auto a = points(state, id);
    const auto b = points(state, id, ComparisonSide::b);
    checkPoints(a, b);
    const auto signature = comparisonGeometrySignature(state, id);
    const auto bounds = comparisonDisplayBounds(state, id);
    const auto snapshot = snapshotComparisonInputs(state, id);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 150);
    setSceneObjectProperty(state, state.files[0].groupSettings[0].objectId, UiObjectProperty::rotationZ, 75);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::scale, 2);
    setSceneObjectProperty(state, state.sceneNodes[0].objectId, UiObjectProperty::rotationY, 20);
    setSceneObjectProperty(state, state.files[2].objectId, UiObjectProperty::translationZ, 400);
    checkPoints(points(state, id), a);
    checkPoints(points(state, id, ComparisonSide::b), b);
    checkPoints(points(comparisonWorldMesh(snapshot, ComparisonSide::a)), a);
    checkPoints(points(comparisonWorldMesh(snapshotComparisonInputs(state, id), ComparisonSide::b)), b);
    CHECK(comparisonGeometrySignature(state, id) == signature);
    REQUIRE(bounds);
    CHECK(comparisonDisplayBounds(state, id)->min == bounds->min);
    const auto measured = compareMeshes(comparisonWorldMesh(state, ComparisonSide::a, id), comparisonWorldMesh(state, ComparisonSide::b, id));
    CHECK(measured.original.maximum == doctest::Approx(0));
    auto settings = comparisonSettings(state, id);
    settings.distanceOnOriginal = !settings.distanceOnOriginal;
    setComparisonSettings(state, settings, id);
    CHECK(comparisonGeometrySignature(state, id) == signature);
    checkPoints(points(state, id), a);
    setComparisonTranslation(state, id, {400, 0, 0});
    CHECK(comparisonGeometrySignature(state, id) == signature);
    checkPoints(points(state, id), a);
    // Geometry stays live at the captured transform; only placement is frozen.
    auto& mesh = state.files[0].mesh;
    editMeshGeometry(mesh).vertices[0].position[2] = .25f;
    mesh.contentRevision = nextMeshContentRevision();
    CHECK(comparisonGeometrySignature(state, id) != signature);
    checkPoint(points(state, id)[0], {10, 0, .25});
    checkPoints(points(comparisonWorldMesh(snapshot, ComparisonSide::a)), a);
}

TEST_CASE("origin alignment translates each input as a unit and direction chooses the reference")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto id = fixture.create(true);
    CHECK(findComparison(state, id)->origins[0] == state.files[0].objectId);
    unlock(state, id);
    auto settings = comparisonSettings(state, id);
    settings.distanceOnOriginal = true;
    setComparisonSettings(state, settings, id);
    const auto aligned = points(state, id);
    REQUIRE(aligned.size() == 8);
    checkPoint(aligned[0], {30, 10, 2});
    checkPoint(aligned[4], {35, 13, 2});
    checkPoint(points(state, id, ComparisonSide::b)[0], {30, 10, 2});
    const auto signature = comparisonGeometrySignature(state, id);
    REQUIRE(setComparisonOrigin(state, id, ComparisonSide::a, state.files[1].objectId));
    checkPoint(points(state, id)[0], {25, 7, 2});
    checkPoint(points(state, id)[4], {30, 10, 2});
    CHECK(comparisonGeometrySignature(state, id) != signature);
    CHECK_FALSE(setComparisonOrigin(state, id, ComparisonSide::a, state.files[2].objectId));
    settings.distanceOnOriginal = false;
    setComparisonSettings(state, settings, id);
    checkPoint(points(state, id)[0], {10, 0, 0});
    checkPoint(points(state, id, ComparisonSide::b)[0], {15, 3, 0});
    // Tree reordering must not choose a different origin.
    std::reverse(state.sceneNodes[0].children.begin(), state.sceneNodes[0].children.end());
    CHECK(findComparison(state, id)->origins[0] == state.files[1].objectId);
    checkPoint(points(state, id, ComparisonSide::b)[0], {15, 3, 0});
    swapComparisonGroups(state, id);
    CHECK(findComparison(state, id)->origins[1] == state.files[1].objectId);
}

TEST_CASE("folder inputs use the same origin as the transform alignment action")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    state.sceneNodes[0].settings.translation = {3, -5, 4};
    state.sceneNodes[0].settings.rotationDegrees = {0, 0, 40};
    const auto id = fixture.create(true, true);
    CHECK(findComparison(state, id)->origins[0] == state.sceneNodes[0].objectId);
    unlock(state, id);
    const auto& folder = state.sceneNodes[0].settings;
    CoordinateMatrix transform;
    sceneNodeTransformMatrix(folder, transform.data());
    const auto expected = transformCoordinate(transform.data(), {folder.center[0], folder.center[1], folder.center[2]});
    const auto b = comparisonWorldMesh(state, ComparisonSide::b, id);
    checkPoint({b.bounds.center[0], b.bounds.center[1], b.bounds.center[2]}, expected);
    const auto before = points(state, id);
    auto settings = comparisonSettings(state, id);
    settings.distanceOnOriginal = true;
    setComparisonSettings(state, settings, id);
    const auto after = points(state, id);
    for (size_t i = 1; i < before.size(); ++i) {
        for (size_t k = 0; k < 3; ++k) {
            CHECK(after[i][k] - after[0][k] == doctest::Approx(before[i][k] - before[0][k]).epsilon(1e-9).scale(1));
        }
    }
}

TEST_CASE("unlock and relock captures current placements without an update action")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto id = fixture.create();
    const auto original = points(state, id);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 100);
    auto settings = comparisonSettings(state, id);
    setComparisonSettings(state, settings, id); // Idempotent lock=true.
    checkPoints(points(state, id), original);
    settings.alignOrigins = false;
    CHECK_THROWS_AS(setComparisonSettings(state, settings, id), std::invalid_argument);
    CHECK_FALSE(setComparisonOrigin(state, id, ComparisonSide::a, state.files[0].objectId));
    unlock(state, id, false);
    checkPoint(points(state, id)[0], {100, 0, 0});
    settings = comparisonSettings(state, id);
    settings.lockPlacement = true;
    setComparisonSettings(state, settings, id);
    const auto captured = points(state, id);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 200);
    checkPoints(points(state, id), captured);
    const auto duplicate = duplicateComparison(state, id);
    checkPoints(points(state, duplicate), captured);
    CHECK(findComparison(state, duplicate)->origins == findComparison(state, id)->origins);
    // Disabled members also retain their placement when re-enabled.
    setComparisonObjectsEnabled(state, {}, ComparisonSide::a, false, id);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::scale, 0);
    setComparisonObjectsEnabled(state, {}, ComparisonSide::a, true, id);
    checkPoints(points(state, id), captured);
}

TEST_CASE("locked placements survive origin rebasing without losing small coordinates")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    state.coordinateOrigin = Coordinate{1e12, -2e12, 3e12};
    for (auto& file : state.files) { file.mesh.origin = *state.coordinateOrigin; }
    synchronizeCoordinateFrames(state);
    const auto id = fixture.create();
    const auto before = points(state, id);
    const Coordinate offset{1024, -2048, 512};
    auto sourceOrigin = state.files[0].mesh.origin;
    for (size_t k = 0; k < 3; ++k) { sourceOrigin[k] += offset[k]; }
    rebaseMesh(state.files[0].mesh, sourceOrigin);
    checkPoints(points(state, id), before);
    for (size_t k = 0; k < 3; ++k) { (*state.coordinateOrigin)[k] += offset[k]; }
    synchronizeCoordinateFrames(state);
    const auto after = points(state, id);
    for (size_t i = 0; i < before.size(); ++i) {
        Coordinate expected;
        for (size_t k = 0; k < 3; ++k) { expected[k] = before[i][k] - offset[k]; }
        checkPoint(after[i], expected);
    }
}

TEST_CASE("scene files preserve origin references and captured transforms with fresh object IDs")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto id = fixture.create(true, true);
    const auto captured = points(state, id);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 100);
    const auto document = createSceneDocument(state);
    const auto path = fixture.root / "comparison.woby";
    writeSceneDocument(path, document);
    auto read = readSceneDocument(path);
    CHECK(read.comparisons == document.comparisons);
    auto files = state.files;
    for (size_t i = 0; i < files.size(); ++i) { applySceneFileRecord(files[i], read.files[i]); }
    const auto loaded = prepareSceneReplacement(state, std::move(files), read);
    const auto loadedId = loaded.comparisons[0].objectId;
    CHECK(loadedId != id);
    CHECK(loaded.comparisons[0].origins[0] == loaded.sceneNodes[0].objectId);
    CHECK(loaded.comparisons[0].origins[1] == loaded.files[2].objectId);
    checkPoints(points(loaded, loadedId), captured);
    CHECK(createSceneDocument(loaded).comparisons == document.comparisons);
    auto invalid = document;
    invalid.comparisons[0].a[0].placement->model[15] = 0;
    CHECK_THROWS_AS((void)prepareSceneReplacement(state, state.files, invalid), std::invalid_argument);
    invalid.comparisons[0].a[0].placement->model[15] = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS((void)prepareSceneReplacement(state, state.files, invalid), std::invalid_argument);
    // Missing fields keep the pre-feature live, unaligned behavior.
    const auto legacy = fixture.root / "legacy.woby";
    { std::ofstream output(legacy); output << "[[analyses]]\nname = \"Legacy\"\nanalysis_type = \"surface_comparison\"\n"; }
    const auto old = readSceneDocument(legacy);
    REQUIRE(old.comparisons.size() == 1);
    CHECK_FALSE(old.comparisons[0].settings.alignOrigins);
    CHECK_FALSE(old.comparisons[0].settings.lockPlacement);
}

TEST_CASE("undo and redo restore captured placement instead of recapturing moved sources")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto id = fixture.create();
    const auto captured = points(state, id);
    SceneHistory history;
    resetSceneHistory(history, state);
    const auto clean = createSceneDocument(state);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 100);
    recordSceneHistory(history, state);
    unlock(state, id, false);
    recordSceneHistory(history, state);
    auto restored = prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(restored);
    CHECK(comparisonSettings(*restored, id).lockPlacement);
    CHECK(restored->files[0].fileSettings.translation[0] == 100);
    checkPoints(points(*restored, id), captured);
    commitSceneHistoryStep(history, state, std::move(*restored), false);
    auto redone = prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redone);
    CHECK_FALSE(comparisonSettings(*redone, id).lockPlacement);
    checkPoint(points(*redone, id)[0], {100, 0, 0});
}

TEST_CASE("CLI placement options share defaults validation and relock behavior with the GUI")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto a = std::to_string(state.sceneNodes[0].objectId);
    const auto b = std::to_string(state.files[2].objectId);
    const auto origin = std::to_string(state.files[1].objectId);
    const auto created = command(state, "analysis.create", {{"type", "surface_comparison"}, {"a", a}, {"b", b},
        {"originA", origin}, {"distanceOnA", true}});
    const auto id = std::stoull(created.at("target").get<std::string>());
    CHECK(comparisonSettings(state, id).alignOrigins);
    CHECK(comparisonSettings(state, id).lockPlacement);
    CHECK(findComparison(state, id)->origins[0] == state.files[1].objectId);
    checkPoint(points(state, id)[0], {25, 7, 2});
    const auto before = createSceneDocument(state);
    CHECK_THROWS(command(state, "analysis.set", {{"alignment", "invalid"}}, id));
    CHECK_THROWS(command(state, "analysis.set", {{"alignment", "scene"}}, id));
    CHECK_THROWS(command(state, "analysis.set", {{"originA", b}, {"lockPlacement", false}}, id));
    CHECK(createSceneDocument(state) == before);
    command(state, "analysis.set", {{"lockPlacement", false}, {"alignment", "scene"}, {"originA", a}}, id);
    checkPoint(points(state, id)[0], {10, 0, 0});
    command(state, "analysis.set", {{"lockPlacement", true}}, id);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 200);
    command(state, "analysis.set", {{"lockPlacement", true}}, id);
    checkPoint(points(state, id)[0], {10, 0, 0});
    command(state, "analysis.set", {{"lockPlacement", false}}, id);
    command(state, "analysis.set", {{"lockPlacement", true}}, id);
    checkPoint(points(state, id)[0], {200, 0, 0});
    std::vector<std::string> args = {"woby", "ctl", "--instance", "test", "analysis", "set", "test-id",
        "--alignment", "origins", "--lock-placement", "false", "--origin-a", "model-a", "--origin-b", "model-b"};
    std::vector<char*> argv;
    for (auto& arg : args) { argv.push_back(arg.data()); }
    const auto parsed = parseCommandLine(static_cast<int>(argv.size()), argv.data()).control.operation;
    CHECK(controlOperationParams(parsed) == Json{{"target", "test-id"}, {"alignment", "origins"},
        {"lockPlacement", false}, {"originA", "model-a"}, {"originB", "model-b"}});
}

TEST_CASE("saved views keep the current lock and geometry edits remain independent across comparisons")
{
    PlacementFixture fixture;
    auto& state = fixture.state;
    const auto first = fixture.create();
    const auto firstPoints = points(state, first);
    const auto view = createView(state);
    unlock(state, first, false);
    applyView(state, view);
    CHECK_FALSE(comparisonSettings(state, first).lockPlacement);
    CHECK_FALSE(comparisonSettings(state, first).alignOrigins);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 100);
    const auto second = fixture.create();
    const auto secondPoints = points(state, second);
    CHECK(secondPoints != firstPoints);
    CHECK(comparisonSettings(state, second).lockPlacement);
    setSceneObjectProperty(state, state.files[0].objectId, UiObjectProperty::translationX, 200);
    checkPoint(points(state, first)[0], {200, 0, 0});
    checkPoints(points(state, second), secondPoints);
    // A removed origin has a deterministic replacement. Existing captures stay intact.
    const auto multi = fixture.create(true);
    const auto multiPoints = points(state, multi);
    setComparisonObjects(state, {state.files[0].objectId}, ComparisonSide::a, false, multi);
    CHECK(findComparison(state, multi)->origins[0] == state.files[1].objectId);
    checkPoints(points(state, multi), std::vector<Coordinate>(multiPoints.begin() + 4, multiPoints.end()));
}
