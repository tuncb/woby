#include "scene_draw_plan.h"
#include "scene_history.h"
#include "scene_pick.h"
#include "ui_operations.h"
#include "automation_registry.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <fstream>

namespace {
struct EdgeDirectory {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-edges-" + woby::automationRandomHex(8));
    EdgeDirectory() { std::filesystem::create_directory(root); }
    ~EdgeDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};
woby::UiState edgeScene(const std::filesystem::path& root)
{
    woby::Mesh mesh;
    for (const auto& position : std::array<std::array<float,3>,7>{{
        {-.8f,0,.7f},{.8f,0,.7f},{0,.8f,.7f},
        {-.4f,-.4f,.3f},{.4f,-.4f,.3f},{.4f,.4f,.3f},{-.4f,.4f,.3f}}}) {
        woby::Vertex vertex; vertex.position = position; vertex.normal = {0,0,1}; mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0,1,2,3,4,5,3,5,6}; mesh.nodes = {{"rear",0,3},{"front",3,6}};
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    woby::UiState state;
    state.files.push_back(woby::createUiFileState(root / "mesh.obj", std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(state, 0); woby::assignSceneObjectIds(state);
    for (auto& group : state.files[0].groupSettings) { group.showVertices = false; group.showTriangles = false; }
    return state;
}
woby::ScenePickView edgeView()
{
    woby::ScenePickView view; view.width = view.height = 100;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    return view;
}
}

TEST_CASE("Triangle edge visibility persists through scene files views and undo")
{
    EdgeDirectory directory;
    woby::UiState state;
    CHECK_FALSE(state.triangleEdgeXray);
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history; woby::resetSceneHistory(history, state);
    woby::setTriangleEdgeXray(state, true);
    CHECK(state.isDirty); CHECK(state.revisions.appearance == 1);
    const auto revision = state.sceneEditRevision;
    woby::setTriangleEdgeXray(state, true);
    CHECK(state.sceneEditRevision == revision);
    CHECK(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(undo); woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK_FALSE(state.triangleEdgeXray);
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redo); woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(state.triangleEdgeXray);
    const auto view = woby::createView(state);
    woby::setTriangleEdgeXray(state, false); woby::applyView(state, view);
    CHECK(state.triangleEdgeXray);
    const auto path = directory.root / "scene.woby";
    woby::writeSceneDocument(path, woby::createSceneDocument(state));
    const auto saved = woby::readSceneDocument(path);
    CHECK(saved.triangleEdgeXray); REQUIRE(saved.views.size() == 1);
    CHECK(saved.views[0].scene.triangleEdgeXray);
    const auto restored = woby::prepareSceneReplacement({}, {}, saved);
    CHECK(restored.triangleEdgeXray); CHECK(restored.views[0].scene.triangleEdgeXray);
    auto different = saved; different.triangleEdgeXray = false;
    CHECK_FALSE(woby::sceneContentEqual(saved, different));
    { std::ofstream out(path); out << "version = 22\n[[views]]\nname = \"legacy\"\n"; }
    const auto legacy = woby::readSceneDocument(path);
    CHECK_FALSE(legacy.triangleEdgeXray); CHECK_FALSE(legacy.views[0].scene.triangleEdgeXray);
    { std::ofstream out(path); out << "version = 23\ntriangle_edge_xray = 4\n"; }
    CHECK_THROWS((void)woby::readSceneDocument(path));
    { std::ofstream out(path); out << "version = 23\n[[views]]\ntriangle_edge_xray = \"yes\"\n"; }
    CHECK_THROWS((void)woby::readSceneDocument(path));
}

TEST_CASE("Scene draw plans own hierarchical transforms and invalidate only relevant scene changes")
{
    EdgeDirectory directory;
    auto state = edgeScene(directory.root);
    auto& file = state.files[0];
    file.fileSettings.translation = {2,0,0}; file.fileSettings.opacity = .5f;
    file.groupSettings[0].translation = {0,4,0}; file.groupSettings[0].opacity = .5f;
    file.groupSettings[0].showVertices = true;
    state.masterVertexPointSize = 10; file.vertexSizeScale = 2; file.groupSettings[0].vertexSizeScale = 3;
    woby::UiSceneNode folder; folder.settings.translation = {3,0,0}; folder.settings.opacity = .5f;
    folder.children = std::move(state.sceneNodes); state.sceneNodes = {std::move(folder)};
    woby::SceneDrawCache cache;
    CHECK(woby::updateSceneDrawPlan(cache, state)); REQUIRE(cache.plan.items.size() == 2);
    CHECK(cache.plan.items[0].model[12] == doctest::Approx(5));
    CHECK(cache.plan.items[0].model[13] == doctest::Approx(4));
    CHECK(cache.plan.items[0].color[3] == doctest::Approx(.125f));
    CHECK(cache.plan.items[0].pointSize == 40);
    const auto* storage = cache.plan.items.data();
    state.camera.distance *= 2; state.selectedSceneObjects.push_back(file.groupSettings[0].objectId);
    CHECK_FALSE(woby::updateSceneDrawPlan(cache, state)); CHECK(cache.plan.items.data() == storage);
    woby::setTriangleEdgeXray(state, true);
    CHECK(woby::updateSceneDrawPlan(cache, state)); CHECK(cache.plan.triangleEdgeXray);
    woby::notifySceneEdit(state, woby::SceneChange::labels);
    CHECK_FALSE(woby::updateSceneDrawPlan(cache, state));
    state.sceneNodes[0].settings.visible = false; woby::notifySceneEdit(state, woby::SceneChange::visibility);
    CHECK(woby::updateSceneDrawPlan(cache, state)); CHECK(cache.plan.items.empty());
    state.files.clear(); state.sceneNodes.clear(); ++state.sceneGeneration;
    CHECK(woby::updateSceneDrawPlan(cache, state)); CHECK(cache.plan.items.empty());
}

TEST_CASE("Triangle edge picking follows visible surfaces X-ray and hidden-line occlusion in either group order")
{
    EdgeDirectory directory;
    auto state = edgeScene(directory.root);
    auto& groups = state.files[0].groupSettings;
    groups[0].showSolidMesh = false; groups[0].showTriangles = true;
    const auto pick = [&](woby::PickPoint point) {
        return woby::pickSceneObject(woby::scenePickParts(state), edgeView(), point);
    };
    for (int order = 0; order < 2; ++order) {
        woby::setTriangleEdgeXray(state, false);
        CHECK(pick({50,50}) == groups[1].objectId);
        woby::setTriangleEdgeXray(state, true);
        CHECK(pick({50,50}) == groups[0].objectId);
        std::reverse(state.sceneNodes[0].children.begin(), state.sceneNodes[0].children.end());
    }
    woby::setTriangleEdgeXray(state, false);
    groups[0].showSolidMesh = true; groups[0].showTriangles = false;
    groups[1].showSolidMesh = false; groups[1].showTriangles = true;
    CHECK(pick({45,40}) == woby::invalidSceneObjectId); // Hidden-line interior is not a selectable fill.
    woby::setTriangleEdgeXray(state, true);
    CHECK(pick({45,40}) == groups[0].objectId);
}
