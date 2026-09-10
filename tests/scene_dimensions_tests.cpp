#include "scene_dimensions.h"
#include "scene_history.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <cmath>
#include <fstream>
#include <limits>

namespace {
woby::UiState dimensionScene()
{
    woby::Mesh mesh;
    for (const auto& point : {std::array<float, 3>{0, 0, 0}, {4, 0, 0}, {0, 3, 0}, {900, 800, 700}}) {
        woby::Vertex vertex;
        vertex.position = point;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0, 1, 2};
    mesh.nodes = {{"triangle", 0, 3}};
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    woby::UiState state;
    state.files.push_back(woby::createUiFileState("triangle.obj", mesh, 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::assignSceneObjectIds(state);
    woby::selectSceneObject(state, state.files[0].objectId);
    return state;
}

woby::ScenePickView identityView()
{
    woby::ScenePickView result;
    result.width = 400;
    result.height = 200;
    bx::mtxIdentity(result.view.data());
    bx::mtxIdentity(result.projection.data());
    return result;
}
} // namespace

TEST_CASE("dimensions measure referenced vertices and compose parent and part scaling")
{
    auto state = dimensionScene();
    state.files[0].fileSettings.scale = 2;
    state.files[0].groupSettings[0].scale = 3;
    state.files[0].groupSettings[0].rotationDegrees = {0, 0, 90};
    state.files[0].fileSettings.translation = {400, -700, 200};
    const auto dimensions = woby::sceneDimensions(woby::scenePickParts(state));
    REQUIRE(dimensions);
    CHECK(dimensions->objectAxes);
    CHECK(dimensions->lengths[0] == doctest::Approx(24));
    CHECK(dimensions->lengths[1] == doctest::Approx(18));
    CHECK(dimensions->lengths[2] == 0);
    // The long edge rotates with the object while retaining its length.
    CHECK(std::abs(dimensions->corners[1][1] - dimensions->corners[0][1]) == doctest::Approx(24));
    CHECK(dimensions->corners[1][0] == doctest::Approx(dimensions->corners[0][0]));
}

TEST_CASE("combined dimensions use tight world extents instead of transformed local boxes")
{
    auto state = dimensionScene();
    auto parts = woby::scenePickParts(state);
    REQUIRE(parts.size() == 1);
    bx::mtxIdentity(parts[0].model.data());
    const float c = std::sqrt(.5f);
    parts[0].model[0] = c;
    parts[0].model[1] = c;
    parts[0].model[4] = -c;
    parts[0].model[5] = c;
    parts.push_back(parts[0]);
    parts[1].model[12] = 10;
    const auto dimensions = woby::sceneDimensions(parts);
    REQUIRE(dimensions);
    CHECK_FALSE(dimensions->objectAxes);
    CHECK(dimensions->lengths[0] == doctest::Approx(10 + 7 * c));
    CHECK(dimensions->lengths[1] == doctest::Approx(4 * c));
    CHECK(dimensions->lengths[2] == 0);
    CHECK(dimensions->corners[0][0] == doctest::Approx(-3 * c));
}

TEST_CASE("dimensions exclude hidden children and handle an empty selection")
{
    auto state = dimensionScene();
    auto parts = woby::scenePickParts(state);
    REQUIRE(woby::sceneDimensions(parts));
    SUBCASE("no selection") { woby::clearSceneSelection(state); }
    SUBCASE("hidden parent") { state.files[0].fileSettings.visible = false; }
    SUBCASE("transparent part") { state.files[0].groupSettings[0].opacity = 0; }
    SUBCASE("no displayed primitives") { state.files[0].groupSettings[0].showSolidMesh = false; }
    CHECK_FALSE(woby::sceneDimensions(woby::scenePickParts(state)));
}

TEST_CASE("dimension cache refreshes for transform selection replacement and geometry edits")
{
    auto state = dimensionScene();
    woby::SceneDimensionsCache cache;
    const auto update = [&]() -> const auto& {
        return woby::updateSceneDimensions(cache, woby::scenePickParts(state), state.sceneGeneration, state.sceneEditRevision);
    };
    REQUIRE(update());
    CHECK(cache.dimensions->lengths[0] == doctest::Approx(4));
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::scale, 2);
    REQUIRE(update());
    CHECK(cache.dimensions->lengths[0] == doctest::Approx(8));
    state.files[0].mesh.vertices[1].position[0] = 7;
    woby::notifySceneEdit(state);
    REQUIRE(update());
    CHECK(cache.dimensions->lengths[0] == doctest::Approx(14));
    state.files[0].mesh.vertices[1].position[0] = 5;
    ++state.sceneGeneration;
    REQUIRE(update());
    CHECK(cache.dimensions->lengths[0] == doctest::Approx(10));
    woby::clearSceneSelection(state);
    CHECK_FALSE(update());
}

TEST_CASE("selecting a folder and its descendant counts the visible part once")
{
    auto state = dimensionScene();
    woby::UiSceneNode folder;
    folder.name = "Assembly";
    folder.settings.scale = 5;
    folder.children = state.sceneNodes;
    state.sceneNodes = {folder};
    woby::assignSceneObjectIds(state);
    woby::selectSceneObject(state, state.sceneNodes[0].objectId);
    woby::selectSceneObject(state, state.files[0].groupSettings[0].objectId, true);
    const auto dimensions = woby::sceneDimensions(woby::scenePickParts(state));
    REQUIRE(dimensions);
    CHECK(dimensions->objectAxes);
    CHECK(dimensions->lengths[0] == doctest::Approx(20));
    CHECK(dimensions->lengths[1] == doctest::Approx(15));
}

TEST_CASE("dimensions reject unusable geometry and nonfinite transforms")
{
    auto state = dimensionScene();
    auto parts = woby::scenePickParts(state);
    SUBCASE("out of range index interval") { parts[0].indexOffset = 1000; }
    SUBCASE("empty interval") { parts[0].indexCount = 0; }
    SUBCASE("invalid vertex references") { state.files[0].mesh.indices = {99, 99, 99}; }
    SUBCASE("invalid transform") { parts[0].model[0] = std::numeric_limits<float>::infinity(); }
    CHECK_FALSE(woby::sceneDimensions(parts));
}

TEST_CASE("dimension lengths preserve small values at large scene translations")
{
    auto state = dimensionScene();
    auto parts = woby::scenePickParts(state);
    bx::mtxIdentity(parts[0].model.data());
    parts[0].model[12] = 1e15f;
    parts[0].model[0] = .01f;
    const auto dimensions = woby::sceneDimensions(parts);
    REQUIRE(dimensions);
    CHECK(dimensions->lengths[0] == doctest::Approx(.04));
}

TEST_CASE("dimension projection respects viewport size and the full clip volume")
{
    auto view = identityView();
    REQUIRE(woby::projectDimensionPoint({.5, -.5, .5}, view));
    CHECK(*woby::projectDimensionPoint({.5, -.5, .5}, view) == woby::PickPoint{300, 150});
    CHECK_FALSE(woby::projectDimensionPoint({2, 0, .5}, view));
    CHECK_FALSE(woby::projectDimensionPoint({0, 0, -.1}, view));
    CHECK_FALSE(woby::projectDimensionPoint({0, 0, 1.1}, view));
    CHECK_FALSE(woby::projectDimensionPoint({std::numeric_limits<double>::infinity(), 0, .5}, view));
    view.homogeneousDepth = true;
    CHECK(woby::projectDimensionPoint({0, 0, -.5}, view));
    view.projection[15] = -1;
    CHECK_FALSE(woby::projectDimensionPoint({0, 0, 0}, view));
}

TEST_CASE("grid labels share nice spacing and up-axis bounds with the rendered grid")
{
    auto bounds = woby::defaultDisplayBounds();
    CHECK(woby::sceneGrid(bounds, woby::SceneUpAxis::z).spacing == 1);
    bounds.max[0] = 13;
    auto grid = woby::sceneGrid(bounds, woby::SceneUpAxis::z);
    CHECK(grid.spacing == 2);
    CHECK(grid.radius == 7);
    CHECK(grid.extent == 14);
    bounds.max[0] = 43;
    CHECK(woby::sceneGrid(bounds, woby::SceneUpAxis::z).spacing == 5);
    bounds.max[0] = 83;
    CHECK(woby::sceneGrid(bounds, woby::SceneUpAxis::z).spacing == 10);
    bounds.max = {1, 250, 750};
    CHECK(woby::sceneGrid(bounds, woby::SceneUpAxis::z).spacing == 50);
    CHECK(woby::sceneGrid(bounds, woby::SceneUpAxis::y).spacing == 100);
}

TEST_CASE("dimension display persists and participates in dirty tracking undo and redo")
{
    woby::UiState state;
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    woby::setShowDimensions(state, true);
    CHECK(state.isDirty);
    REQUIRE(woby::recordSceneHistory(history, state));
    CHECK_FALSE(woby::sceneContentEqual(clean, woby::createSceneDocument(state)));
    const auto revision = state.sceneEditRevision;
    woby::setShowDimensions(state, true);
    CHECK(state.sceneEditRevision == revision);
    auto undone = woby::prepareSceneHistoryStep(history, state, clean, false, {});
    REQUIRE(undone);
    woby::commitSceneHistoryStep(history, state, std::move(*undone), false);
    CHECK_FALSE(state.showDimensions);
    CHECK_FALSE(state.isDirty);
    auto redone = woby::prepareSceneHistoryStep(history, state, clean, true, {});
    REQUIRE(redone);
    woby::commitSceneHistoryStep(history, state, std::move(*redone), true);
    CHECK(state.showDimensions);

    const auto path = std::filesystem::temp_directory_path() / "woby-dimension-display.woby";
    woby::writeSceneDocument(path, woby::createSceneDocument(state));
    const auto document = woby::readSceneDocument(path);
    CHECK(document.showDimensions);
    CHECK(woby::prepareSceneReplacement(state, {}, document).showDimensions);
    std::ifstream saved(path);
    const std::string text{std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()};
    CHECK(text.find("unit") == std::string::npos);
    saved.close();
    { std::ofstream legacy(path); legacy << "version = 6\n"; }
    CHECK_FALSE(woby::readSceneDocument(path).showDimensions);
    { std::ofstream invalid(path); invalid << "version = 6\nshow_dimensions = 12\n"; }
    CHECK_THROWS((void)woby::readSceneDocument(path));
    std::filesystem::remove(path);
}
