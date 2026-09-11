#include "scene_pick.h"
#include "scene_viewport.h"
#include "ui_operations.h"

#include <doctest/doctest.h>

#include <limits>

namespace {
woby::Mesh triangle(float z = .4f)
{
    woby::Mesh mesh;
    for (const auto p : {std::array<float, 3>{-.8f, -.8f, z}, {.8f, -.8f, z}, {0, .8f, z}}) {
        woby::Vertex vertex;
        vertex.position = p;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0, 1, 2};
    mesh.nodes = {{"part", 0, 3}};
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    return mesh;
}
woby::UiState scene()
{
    woby::UiState state;
    state.files.push_back(woby::createUiFileState("pick.obj", triangle(), 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::assignSceneObjectIds(state);
    return state;
}
woby::ScenePickView view()
{
    woby::ScenePickView result;
    bx::mtxIdentity(result.view.data());
    bx::mtxIdentity(result.projection.data());
    result.width = result.height = 100;
    return result;
}
woby::SceneObjectId pick(const woby::UiState& state, woby::PickPoint point = {50, 50})
{
    return woby::pickSceneObject(woby::scenePickParts(state), view(), point);
}
}

TEST_CASE("hiding an inspected object removes viewport highlighting without clearing selection")
{
    auto state = scene();
    const auto id = state.files[0].groupSettings[0].objectId;
    woby::selectSceneObject(state, id);
    REQUIRE(woby::scenePickParts(state).size() == 1u);
    CHECK(woby::scenePickParts(state)[0].selected);
    woby::setSelectedObjectsVisible(state, false);
    CHECK(woby::scenePickParts(state).empty());
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{id});
    CHECK(state.propertiesPaneVisible);
    CHECK(woby::selectedObjectProperty(state, woby::UiObjectProperty::opacity).available);
    woby::setSelectedObjectsVisible(state, true);
    REQUIRE(woby::scenePickParts(state).size() == 1u);
    CHECK(woby::scenePickParts(state)[0].selected);
    CHECK(pick(state) == id);
}

TEST_CASE("canvas clicks select surfaces and reuse transient multi selection")
{
    auto state = scene();
    const auto document = woby::createSceneDocument(state);
    woby::clearSceneDirty(state);
    const auto hit = pick(state);
    CHECK(hit == state.files[0].groupSettings[0].objectId);
    woby::selectSceneObject(state, hit);
    CHECK(state.selectedSceneObjects == std::vector<woby::SceneObjectId>{hit});
    CHECK(state.propertiesPaneVisible);
    CHECK_FALSE(state.isDirty);
    CHECK(woby::createSceneDocument(state) == document);
    woby::selectSceneObject(state, hit, true);
    CHECK(state.selectedSceneObjects.empty());
    CHECK(pick(state, {2, 2}) == woby::invalidSceneObjectId);
    CHECK(pick(state, {-1, 50}) == woby::invalidSceneObjectId);
    CHECK(pick(state, {100, 50}) == woby::invalidSceneObjectId);
    CHECK(pick(state, {50, std::numeric_limits<float>::quiet_NaN()}) == woby::invalidSceneObjectId);
}

TEST_CASE("canvas picking respects opaque depth and deterministic coplanar order")
{
    auto state = scene();
    state.files.push_back(woby::createUiFileState("front.obj", triangle(.2f), 1));
    woby::appendDefaultSceneNodesForFiles(state, 1);
    woby::assignSceneObjectIds(state);
    const auto front = state.files[1].groupSettings[0].objectId;
    CHECK(pick(state) == front);
    std::reverse(state.sceneNodes.begin(), state.sceneNodes.end());
    CHECK(pick(state) == front);
    state.files[0].groupSettings[0].translation[2] = -.2f;
    CHECK(pick(state) == front);
    // Transparent geometry behind an opaque surface cannot steal its selection.
    state.files[0].groupSettings[0].translation[2] = 0;
    state.files[0].groupSettings[0].opacity = .5f;
    CHECK(pick(state) == front);
    state.files[1].groupSettings[0].opacity = .5f;
    CHECK(pick(state) == state.files[0].groupSettings[0].objectId);
}

TEST_CASE("canvas picking ignores invisible and disabled modes including inherited opacity")
{
    auto state = scene();
    const auto id = state.files[0].groupSettings[0].objectId;
    REQUIRE(pick(state) == id);
    SUBCASE("hidden file") { state.files[0].fileSettings.visible = false; }
    SUBCASE("hidden group") { state.files[0].groupSettings[0].visible = false; }
    SUBCASE("transparent group") { state.files[0].groupSettings[0].opacity = 0; }
    SUBCASE("transparent file") { state.files[0].fileSettings.opacity = 0; }
    SUBCASE("no displayed primitives") { state.files[0].groupSettings[0].showSolidMesh = false; }
    SUBCASE("transparent ancestor") {
        woby::UiSceneNode folder;
        folder.children = state.sceneNodes;
        folder.settings.opacity = 0;
        state.sceneNodes = {folder};
    }
    SUBCASE("hidden ancestor") {
        woby::UiSceneNode folder;
        folder.children = state.sceneNodes;
        folder.settings.visible = false;
        state.sceneNodes = {folder};
    }
    CHECK(pick(state) == woby::invalidSceneObjectId);
}

TEST_CASE("canvas picker handles clipping reversed winding and malformed triangles")
{
    auto state = scene();
    SUBCASE("back face") {
        std::reverse(state.files[0].mesh.indices.begin(), state.files[0].mesh.indices.end());
        CHECK(pick(state) != woby::invalidSceneObjectId);
        return;
    }
    SUBCASE("before near plane") { state.files[0].groupSettings[0].translation[2] = -1; }
    SUBCASE("past far plane") { state.files[0].groupSettings[0].translation[2] = 1; }
    SUBCASE("bad index") { state.files[0].mesh.indices[0] = 99; }
    SUBCASE("non finite position") { state.files[0].mesh.vertices[0].position[0] = std::numeric_limits<float>::infinity(); }
    SUBCASE("degenerate triangle") { state.files[0].mesh.indices = {0, 0, 0}; }
    SUBCASE("out of range node") { state.files[0].mesh.nodes[0].indexOffset = 99; }
    CHECK(pick(state) == woby::invalidSceneObjectId);
}

TEST_CASE("wireframe and vertex only picking follows displayed primitives")
{
    auto state = scene();
    auto& group = state.files[0].groupSettings[0];
    const auto id = group.objectId;
    group.showSolidMesh = false;
    group.showTriangles = true;
    CHECK(pick(state) == woby::invalidSceneObjectId);
    CHECK(pick(state, {50, 90}) == id);
    CHECK(pick(state, {50, 92}) == id);
    CHECK(pick(state, {50, 95}) == woby::invalidSceneObjectId);
    group.showTriangles = false;
    group.showVertices = true;
    CHECK(pick(state, {50, 90}) == woby::invalidSceneObjectId);
    CHECK(pick(state, {50, 10}) == id);
    CHECK(pick(state, {52, 10}) == id);
    CHECK(pick(state, {55, 10}) == woby::invalidSceneObjectId);
    state.masterVertexPointSize = 20;
    CHECK(pick(state, {58, 10}) == id);
}

TEST_CASE("xray edges pick through surfaces while hidden vertices do not")
{
    auto state = scene();
    // The rear triangle's top vertex and its base edge both fall inside the front surface.
    auto rear = triangle(.8f);
    for (auto& vertex : rear.vertices) { vertex.position[0] *= .3f; vertex.position[1] *= .3f; }
    rear.bounds = woby::calculateBounds(rear.vertices);
    state.files.push_back(woby::createUiFileState("rear.obj", rear, 1));
    woby::appendDefaultSceneNodesForFiles(state, 1);
    woby::assignSceneObjectIds(state);
    auto& back = state.files[1].groupSettings[0];
    back.showSolidMesh = false;
    back.showVertices = true;
    CHECK(pick(state, {50, 38}) == state.files[0].groupSettings[0].objectId);
    back.showVertices = false;
    back.showTriangles = true;
    CHECK(pick(state, {50, 62}) == back.objectId);
}

TEST_CASE("edge picking clips segments crossing the near plane")
{
    auto state = scene();
    auto& file = state.files[0];
    file.groupSettings[0].showSolidMesh = false;
    file.groupSettings[0].showTriangles = true;
    file.mesh.vertices[0].position = {-.8f, 0, -.4f};
    file.mesh.vertices[1].position = {.8f, 0, .4f};
    file.mesh.vertices[2].position = {.8f, .8f, .4f};
    CHECK(pick(state, {70, 50}) == file.groupSettings[0].objectId);
    CHECK(pick(state, {20, 50}) == woby::invalidSceneObjectId);
}

TEST_CASE("picking and selection boxes use renderer hierarchy transforms")
{
    auto state = scene();
    auto& file = state.files[0];
    file.groupSettings[0].center = {};
    file.fileSettings.center = {};
    file.groupSettings[0].scale = .2f;
    file.groupSettings[0].rotationDegrees[2] = 90;
    file.fileSettings.translation[0] = 1;
    woby::UiSceneNode folder;
    folder.settings.translation[1] = 1;
    folder.children = state.sceneNodes;
    state.sceneNodes = {folder};
    woby::assignSceneObjectIds(state);
    woby::selectSceneObject(state, state.sceneNodes[0].objectId);
    // Renderer order applies the parent translations before this group's rotation/scale.
    CHECK(pick(state, {60, 60}) == file.groupSettings[0].objectId);
    CHECK(pick(state, {50, 50}) == woby::invalidSceneObjectId);
    const auto parts = woby::scenePickParts(state);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].selected);
    const auto lines = woby::sceneSelectionLines(parts);
    REQUIRE(lines.size() == 24);
    for (const auto& p : lines) {
        CHECK(p[0] >= .039f);
        CHECK(p[0] <= .361f);
        CHECK(p[1] >= -.361f);
        CHECK(p[1] <= -.039f);
    }
    CHECK_FALSE(state.isDirty);
}

TEST_CASE("picking supports flat scenes files without child nodes and missing cached bounds")
{
    auto state = scene();
    SUBCASE("flat scene") { state.sceneNodes.clear(); }
    SUBCASE("file without children") { state.sceneNodes[0].children.clear(); }
    state.files[0].groupSettings[0].localBoundsValid = false;
    CHECK(pick(state) == state.files[0].groupSettings[0].objectId);
    woby::selectSceneObject(state, state.files[0].objectId);
    const auto lines = woby::sceneSelectionLines(woby::scenePickParts(state));
    CHECK(lines.size() == 24);
}

TEST_CASE("canvas selection reveals only the selected part ancestor path")
{
    auto state = scene();
    state.files.push_back(woby::createUiFileState("other.obj", triangle(), 1));
    woby::appendDefaultSceneNodesForFiles(state, 1);
    woby::UiSceneNode folder;
    folder.children = {state.sceneNodes[0]};
    state.sceneNodes[0] = folder;
    woby::assignSceneObjectIds(state);
    const auto partId = state.files[0].groupSettings[0].objectId;
    CHECK(woby::sceneSelectionPath(state, partId) == std::vector<woby::SceneObjectId>{
        state.sceneNodes[0].objectId, state.files[0].objectId, partId});
    CHECK(woby::sceneSelectionPath(state, 0).empty());
    CHECK(woby::sceneSelectionPath(state, state.nextObjectId + 1).empty());
}

TEST_CASE("canvas picking resolves separate part index ranges within one mesh")
{
    auto mesh = triangle();
    for (auto& vertex : mesh.vertices) { vertex.position[0] = vertex.position[0] * .3f - .5f; }
    const auto other = triangle();
    for (auto vertex : other.vertices) {
        vertex.position[0] = vertex.position[0] * .3f + .5f;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0, 1, 2, 3, 4, 5};
    mesh.nodes.push_back({"right", 3, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    woby::UiState state;
    state.files.push_back(woby::createUiFileState("two.obj", mesh, 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::assignSceneObjectIds(state);
    CHECK(pick(state, {25, 50}) == state.files[0].groupSettings[0].objectId);
    CHECK(pick(state, {75, 50}) == state.files[0].groupSettings[1].objectId);
    CHECK(pick(state, {50, 50}) == woby::invalidSceneObjectId);
}

TEST_CASE("perspective picking works with roll up axis depth conventions and DPI panels")
{
    auto state = scene();
    auto camera = woby::frameCameraBounds(state.files[0].mesh.bounds);
    camera.rollRadians = .5f;
    for (const auto up : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        for (const bool homogeneous : {false, true}) {
            for (const float scale : {1.0f, 1.25f, 2.0f}) {
                for (const float right : {0.0f, 400.0f}) {
                    const auto viewport = woby::sceneViewport(static_cast<uint32_t>(1200 * scale),
                        static_cast<uint32_t>(800 * scale), 1200, 300, right);
                    auto pickView = woby::scenePickView(camera, up, state.files[0].mesh.bounds,
                        viewport.width, viewport.height, homogeneous, scale);
                    // Project an interior point with the camera used by rendering, then unproject to pick.
                    const std::array<float, 3> interior{0, -.2f, .4f};
                    float eye[4], clip[4];
                    const float p[4] = {interior[0], interior[1], interior[2], 1};
                    bx::vec4MulMtx(eye, p, pickView.view.data());
                    bx::vec4MulMtx(clip, eye, pickView.projection.data());
                    const float px = (clip[0] / clip[3] * .5f + .5f) * static_cast<float>(viewport.width);
                    const float py = (.5f - clip[1] / clip[3] * .5f) * static_cast<float>(viewport.height);
                    CAPTURE(up);
                    CAPTURE(homogeneous);
                    CAPTURE(scale);
                    CAPTURE(right);
                    CHECK(woby::pickSceneObject(woby::scenePickParts(state), pickView, {px, py}) == state.files[0].groupSettings[0].objectId);
                }
            }
        }
    }
}

TEST_CASE("analysis picks select result identity and honor mode translation and diagnostic edges")
{
    woby::UiComparison comparison;
    comparison.objectId = 42;
    comparison.settings.enabled = true;
    comparison.settings.showBoundaries = false;
    comparison.settings.showNonManifold = false;
    woby::MeshComparison result;
    result.original.source = triangle(.2f);
    result.repaired.source = triangle(.6f);
    for (auto& vertex : result.original.source.vertices) { vertex.position[0] += 3; }
    result.original.source.bounds = woby::calculateBounds(result.original.source.vertices);
    std::vector<woby::ScenePickPart> parts;
    for (const auto mode : {woby::ComparisonMode::distance, woby::ComparisonMode::original,
            woby::ComparisonMode::repaired, woby::ComparisonMode::overlay}) {
        comparison.settings.mode = mode;
        parts.clear();
        woby::appendComparisonPickParts(parts, comparison, comparison.settings, result, true);
        CHECK(woby::pickSceneObject(parts, view(), {50, 50}) == (mode == woby::ComparisonMode::original ? 0 : 42));
    }
    comparison.settings.mode = woby::ComparisonMode::distance;
    comparison.settings.distanceOnOriginal = true;
    comparison.translation[0] = -3;
    parts.clear();
    woby::appendComparisonPickParts(parts, comparison, comparison.settings, result, true);
    CHECK(woby::pickSceneObject(parts, view(), {50, 50}) == 42);
    CHECK(woby::sceneSelectionLines(parts).size() == 24);
    comparison.settings.showBoundaries = true;
    result.original.diagnostics.boundaryEdges.push_back({{2.5f, .9f, .2f}, {3.5f, .9f, .2f}});
    parts.clear();
    woby::appendComparisonPickParts(parts, comparison, comparison.settings, result, true);
    CHECK(woby::pickSceneObject(parts, view(), {50, 5}) == 42);
    comparison.settings.enabled = false;
    parts.clear();
    woby::appendComparisonPickParts(parts, comparison, comparison.settings, result, true);
    CHECK(parts.empty());
}

TEST_CASE("click threshold preserves gestures modifiers and canceled presses")
{
    woby::ScenePointerGesture gesture;
    CHECK_FALSE(woby::endScenePointer(gesture, {10, 10}, true));
    woby::beginScenePointer(gesture, {10, 10}, false, true);
    CHECK_FALSE(woby::moveScenePointer(gesture, {12, 11}));
    const auto click = woby::endScenePointer(gesture, {12, 11}, true);
    REQUIRE(click);
    CHECK(click->toggle);
    CHECK_FALSE(gesture.active);
    woby::beginScenePointer(gesture, {10, 10}, false, false);
    CHECK(woby::moveScenePointer(gesture, {14, 10}));
    CHECK(woby::moveScenePointer(gesture, {10, 10}));
    CHECK_FALSE(woby::endScenePointer(gesture, {10, 10}, true));
    woby::beginScenePointer(gesture, {10, 10}, true, false);
    CHECK_FALSE(woby::endScenePointer(gesture, {10, 10}, true));
    woby::beginScenePointer(gesture, {10, 10}, false, false);
    CHECK_FALSE(woby::endScenePointer(gesture, {10, 10}, false));
    woby::beginScenePointer(gesture, {10, 10}, false, false);
    // A release far away is a drag even without an intervening motion event.
    CHECK_FALSE(woby::endScenePointer(gesture, {30, 30}, true));
}
