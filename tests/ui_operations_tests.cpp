#include "ui_operations.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>

namespace {

woby::Bounds makeBounds(float minX, float maxX)
{
    woby::Bounds bounds;
    bounds.min = {minX, 0.0f, 0.0f};
    bounds.max = {maxX, 1.0f, 1.0f};
    bounds.center = {
        (minX + maxX) * 0.5f,
        0.5f,
        0.5f,
    };

    const float halfX = (maxX - minX) * 0.5f;
    bounds.radius = std::sqrt(halfX * halfX + 0.5f);
    return bounds;
}

woby::Mesh makeMesh(const char* groupName, float minX, float maxX)
{
    woby::Mesh mesh;
    mesh.vertices = {
        {{minX, 0.0f, 0.0f}, {}, {}},
        {{maxX, 0.0f, 0.0f}, {}, {}},
        {{minX, 1.0f, 0.0f}, {}, {}},
    };
    mesh.indices = {0u, 1u, 2u};
    mesh.nodes.push_back({groupName, 0u, 3u});
    mesh.bounds = makeBounds(minX, maxX);
    return mesh;
}

woby::UiFileState makeFile(const char* path, const char* groupName, float minX, float maxX, size_t colorIndex)
{
    return woby::createUiFileState(path, makeMesh(groupName, minX, maxX), colorIndex);
}

bool finiteVec3(const std::array<float, 3>& value)
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool finiteBxVec3(const bx::Vec3& value)
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

float generatedFloat(std::mt19937& random, float low, float high)
{
    return std::uniform_real_distribution<float>(low, high)(random);
}

std::array<float, 3> generatedFloat3(std::mt19937& random, float low, float high)
{
    return {
        generatedFloat(random, low, high),
        generatedFloat(random, low, high),
        generatedFloat(random, low, high),
    };
}

std::array<float, 4> generatedFloat4(std::mt19937& random, float low, float high)
{
    return {
        generatedFloat(random, low, high),
        generatedFloat(random, low, high),
        generatedFloat(random, low, high),
        generatedFloat(random, low, high),
    };
}

void checkClampedGroupSettings(const woby::UiGroupState& group)
{
    CHECK(group.scale >= woby::minGroupScale);
    CHECK(group.scale <= woby::maxGroupScale);
    CHECK(group.opacity >= woby::minGroupOpacity);
    CHECK(group.opacity <= woby::maxGroupOpacity);
    CHECK(group.vertexSizeScale >= woby::minVertexSizeScale);
    CHECK(group.vertexSizeScale <= woby::maxVertexSizeScale);
    CHECK(finiteVec3(group.translation));
    for (const float rotation : group.rotationDegrees) {
        CHECK(rotation >= woby::minRotationDegrees);
        CHECK(rotation <= woby::maxRotationDegrees);
    }
    for (const float channel : group.color) {
        CHECK(channel >= 0.0f);
        CHECK(channel <= 1.0f);
    }
}

void checkClampedFileSettings(const woby::UiFileState& file)
{
    CHECK(file.fileSettings.scale >= woby::minGroupScale);
    CHECK(file.fileSettings.scale <= woby::maxGroupScale);
    CHECK(file.fileSettings.opacity >= woby::minGroupOpacity);
    CHECK(file.fileSettings.opacity <= woby::maxGroupOpacity);
    CHECK(file.vertexSizeScale >= woby::minVertexSizeScale);
    CHECK(file.vertexSizeScale <= woby::maxVertexSizeScale);
    CHECK(finiteVec3(file.fileSettings.translation));
    for (const float rotation : file.fileSettings.rotationDegrees) {
        CHECK(rotation >= woby::minRotationDegrees);
        CHECK(rotation <= woby::maxRotationDegrees);
    }
}

void checkVisibilityInvariants(const woby::UiState& state)
{
    size_t expectedVisibleSceneGroups = 0u;
    size_t expectedTotalGroups = 0u;
    for (const auto& file : state.files) {
        expectedTotalGroups += file.groupSettings.size();
        const size_t visibleGroups = woby::countVisibleGroups(file.groupSettings);
        CHECK(woby::countVisibleFileGroups(file) <= file.groupSettings.size());
        if (!file.fileSettings.visible) {
            CHECK(visibleGroups == 0u);
            CHECK(woby::countVisibleFileGroups(file) == 0u);
        } else {
            CHECK(woby::countVisibleFileGroups(file) == visibleGroups);
            expectedVisibleSceneGroups += visibleGroups;
        }
    }

    CHECK(woby::totalGroupCount(state) == expectedTotalGroups);
    CHECK(woby::countVisibleSceneGroups(state) == expectedVisibleSceneGroups);
}

void compareFloat3(const std::array<float, 3>& left, const std::array<float, 3>& right)
{
    CHECK(left[0] == doctest::Approx(right[0]));
    CHECK(left[1] == doctest::Approx(right[1]));
    CHECK(left[2] == doctest::Approx(right[2]));
}

void compareFloat4(const std::array<float, 4>& left, const std::array<float, 4>& right)
{
    CHECK(left[0] == doctest::Approx(right[0]));
    CHECK(left[1] == doctest::Approx(right[1]));
    CHECK(left[2] == doctest::Approx(right[2]));
    CHECK(left[3] == doctest::Approx(right[3]));
}

} // namespace

TEST_CASE("scene render mode operations update all groups")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.files.push_back(makeFile("b.obj", "b", 2.0f, 3.0f, 1u));

    CHECK(woby::totalGroupCount(state) == 2u);
    CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::vertices) == 2u);

    woby::toggleGroupRenderMode(state.files[0].groupSettings[0], woby::UiRenderMode::vertices);
    CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::vertices) == 1u);

    woby::setAllSceneRenderModes(state, woby::UiRenderMode::solidMesh, false);
    CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::solidMesh) == 0u);

    woby::setAllSceneRenderModes(state, woby::UiRenderMode::solidMesh, true);
    CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::solidMesh) == 2u);
}

TEST_CASE("visibility master operations update descendant groups")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.files.push_back(makeFile("b.obj", "b", 2.0f, 3.0f, 1u));
    state.files[0].mesh.nodes.push_back({"a-extra", 0u, 3u});
    state.files[0].groupSettings.push_back(woby::UiGroupState{});

    CHECK(woby::totalGroupCount(state) == 3u);
    CHECK(woby::countVisibleSceneGroups(state) == 3u);
    CHECK(woby::countVisibleFileGroups(state.files[0]) == 2u);

    woby::setGroupVisible(state.files[0], state.files[0].groupSettings[0], false);

    CHECK(state.files[0].fileSettings.visible);
    CHECK(woby::countVisibleFileGroups(state.files[0]) == 1u);
    CHECK(woby::countVisibleSceneGroups(state) == 2u);

    woby::setFileVisible(state.files[0], false);

    CHECK_FALSE(state.files[0].fileSettings.visible);
    CHECK(woby::countVisibleFileGroups(state.files[0]) == 0u);
    CHECK_FALSE(state.files[0].groupSettings[0].visible);
    CHECK_FALSE(state.files[0].groupSettings[1].visible);

    woby::setGroupVisible(state.files[0], state.files[0].groupSettings[1], true);

    CHECK(state.files[0].fileSettings.visible);
    CHECK_FALSE(state.files[0].groupSettings[0].visible);
    CHECK(state.files[0].groupSettings[1].visible);
    CHECK(woby::countVisibleFileGroups(state.files[0]) == 1u);

    woby::setAllSceneVisible(state, false);

    CHECK(woby::countVisibleSceneGroups(state) == 0u);
    CHECK_FALSE(state.files[0].fileSettings.visible);
    CHECK_FALSE(state.files[1].fileSettings.visible);

    woby::setAllSceneVisible(state, true);

    CHECK(woby::countVisibleSceneGroups(state) == 3u);
    CHECK(state.files[0].fileSettings.visible);
    CHECK(state.files[1].fileSettings.visible);
}

TEST_CASE("showing a child under a hidden folder restores mixed folder visibility")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.files[0].mesh.nodes.push_back({"a-extra", 0u, 3u});
    state.files[0].groupSettings.push_back(woby::UiGroupState{});

    woby::UiSceneNode folder;
    folder.kind = woby::UiSceneNodeKind::folder;
    folder.name = "parent";
    folder.children.push_back(woby::createFileSceneNode(state.files[0], 0u));
    state.sceneNodes.push_back(folder);

    auto& parent = state.sceneNodes[0];
    CHECK(woby::countSceneNodeGroups(state, parent) == 2u);
    CHECK(woby::countVisibleSceneNodeGroups(state, parent) == 2u);

    woby::setSceneNodeSubtreeVisible(state, parent, false);

    CHECK_FALSE(parent.settings.visible);
    CHECK_FALSE(state.files[0].fileSettings.visible);
    CHECK(woby::countVisibleSceneNodeGroups(state, parent) == 0u);

    woby::setGroupVisible(state, state.files[0], state.files[0].groupSettings[1], true);

    CHECK(parent.settings.visible);
    CHECK(state.files[0].fileSettings.visible);
    CHECK_FALSE(state.files[0].groupSettings[0].visible);
    CHECK(state.files[0].groupSettings[1].visible);
    CHECK(woby::countVisibleSceneNodeGroups(state, parent) == 1u);
    CHECK(woby::countVisibleSceneGroups(state) == 1u);

    woby::setGroupVisible(state, state.files[0], state.files[0].groupSettings[1], false);

    CHECK_FALSE(parent.settings.visible);
    CHECK_FALSE(state.files[0].fileSettings.visible);
    CHECK(woby::countVisibleSceneNodeGroups(state, parent) == 0u);

    woby::setSceneNodeSubtreeVisible(state, parent, false);
    woby::setSceneNodeSubtreeVisible(state, parent.children[0], true);

    CHECK(parent.settings.visible);
    CHECK(state.files[0].fileSettings.visible);
    CHECK(woby::countVisibleSceneNodeGroups(state, parent) == 2u);
}

TEST_CASE("numeric setters clamp values for saved ui state")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    auto& file = state.files[0];
    auto& group = file.groupSettings[0];

    woby::setMasterVertexPointSize(state, -4.0f);
    CHECK(state.masterVertexPointSize == doctest::Approx(woby::minVertexPointSize));
    woby::setMasterVertexPointSize(state, std::numeric_limits<float>::quiet_NaN());
    CHECK(state.masterVertexPointSize == doctest::Approx(woby::defaultMasterVertexPointSize));

    woby::setFileVertexSizeScale(file, 100.0f);
    CHECK(file.vertexSizeScale == doctest::Approx(woby::maxVertexSizeScale));
    woby::setGroupVertexSizeScale(group, -100.0f);
    CHECK(group.vertexSizeScale == doctest::Approx(woby::minVertexSizeScale));

    woby::setGroupScale(group, -1.0f);
    CHECK(group.scale == doctest::Approx(woby::minGroupScale));
    woby::setGroupOpacity(group, 2.0f);
    CHECK(group.opacity == doctest::Approx(woby::maxGroupOpacity));

    woby::setGroupRotationDegrees(
        group,
        {400.0f, -400.0f, std::numeric_limits<float>::quiet_NaN()});
    CHECK(group.rotationDegrees[0] == doctest::Approx(woby::maxRotationDegrees));
    CHECK(group.rotationDegrees[1] == doctest::Approx(woby::minRotationDegrees));
    CHECK(group.rotationDegrees[2] == doctest::Approx(0.0f));

    woby::setGroupColor(group, {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), 0.5f});
    CHECK(group.color[0] == doctest::Approx(0.0f));
    CHECK(group.color[1] == doctest::Approx(1.0f));
    CHECK(group.color[2] == doctest::Approx(1.0f));
    CHECK(group.color[3] == doctest::Approx(0.5f));
}

TEST_CASE("removing a file recalculates bounds and reframes the camera")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.files.push_back(makeFile("b.obj", "b", 10.0f, 12.0f, 1u));
    woby::recalculateSceneBounds(state);
    woby::frameCameraToScene(state);

    CHECK(woby::removeFileFromState(state, 0u));

    CHECK(state.files.size() == 1u);
    CHECK(state.sceneBounds.center[0] == doctest::Approx(11.0f));
    CHECK(state.camera.target[0] == doctest::Approx(state.sceneBounds.center[0]));
    CHECK_FALSE(woby::removeFileFromState(state, 4u));
}

TEST_CASE("empty scenes use default display bounds")
{
    woby::UiState state;

    woby::recalculateSceneBounds(state);

    CHECK(state.sceneBounds.min[0] == doctest::Approx(woby::defaultDisplayBoundsMin));
    CHECK(state.sceneBounds.min[1] == doctest::Approx(woby::defaultDisplayBoundsMin));
    CHECK(state.sceneBounds.min[2] == doctest::Approx(woby::defaultDisplayBoundsMin));
    CHECK(state.sceneBounds.max[0] == doctest::Approx(woby::defaultDisplayBoundsMax));
    CHECK(state.sceneBounds.max[1] == doctest::Approx(woby::defaultDisplayBoundsMax));
    CHECK(state.sceneBounds.max[2] == doctest::Approx(woby::defaultDisplayBoundsMax));
}

TEST_CASE("scene bounds include file and group transforms")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    auto& file = state.files[0];
    auto& group = file.groupSettings[0];

    woby::setFileTranslation(file.fileSettings, {10.0f, 0.0f, 0.0f});
    woby::setGroupTranslation(group, {0.0f, 20.0f, 0.0f});
    woby::recalculateSceneBounds(state);

    CHECK(state.sceneBounds.min[0] == doctest::Approx(10.0f));
    CHECK(state.sceneBounds.max[0] == doctest::Approx(11.0f));
    CHECK(state.sceneBounds.min[1] == doctest::Approx(20.0f));
    CHECK(state.sceneBounds.max[1] == doctest::Approx(21.0f));
}

TEST_CASE("scene bounds include folder scene node transforms")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));

    woby::UiSceneNode folder;
    folder.kind = woby::UiSceneNodeKind::folder;
    folder.name = "models";
    folder.children.push_back(woby::createFileSceneNode(state.files[0], 0u));
    state.sceneNodes.push_back(std::move(folder));
    woby::refreshSceneTreeFolderCenters(state);
    woby::setSceneNodeTranslation(state.sceneNodes[0].settings, {10.0f, 0.0f, 0.0f});

    woby::recalculateSceneBounds(state);

    CHECK(state.sceneBounds.min[0] == doctest::Approx(10.0f));
    CHECK(state.sceneBounds.max[0] == doctest::Approx(11.0f));
}

TEST_CASE("folder tree scene nodes preserve selected root hierarchy")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_folder_tree_scene_node";
    const std::string firstPath = (root / "alpha" / "one.obj").string();
    const std::string secondPath = (root / "alpha" / "nested" / "two.stl").string();
    const std::string thirdPath = (root / "root.obj").string();

    woby::UiState state;
    state.files.push_back(makeFile(firstPath.c_str(), "one", 0.0f, 1.0f, 0u));
    state.files.push_back(makeFile(secondPath.c_str(), "two", 2.0f, 3.0f, 1u));
    state.files.push_back(makeFile(thirdPath.c_str(), "root", 4.0f, 5.0f, 2u));

    woby::appendFolderTreeSceneNode(state, root, 0u, state.files.size());

    REQUIRE(state.sceneNodes.size() == 1u);
    const auto& rootNode = state.sceneNodes[0];
    CHECK(rootNode.kind == woby::UiSceneNodeKind::folder);
    CHECK(rootNode.name == root.filename().string());
    CHECK(woby::countSceneNodeGroups(state, rootNode) == 3u);
    CHECK(finiteVec3(rootNode.settings.center));
    REQUIRE(rootNode.children.size() == 2u);
    CHECK(rootNode.children[0].kind == woby::UiSceneNodeKind::folder);
    CHECK(rootNode.children[0].name == "alpha");
    REQUIRE(rootNode.children[0].children.size() == 2u);
    CHECK(rootNode.children[0].children[0].kind == woby::UiSceneNodeKind::file);
    CHECK(rootNode.children[0].children[0].fileIndex == 0u);
    CHECK(rootNode.children[0].children[1].kind == woby::UiSceneNodeKind::folder);
    CHECK(rootNode.children[0].children[1].name == "nested");
    REQUIRE(rootNode.children[0].children[1].children.size() == 1u);
    CHECK(rootNode.children[0].children[1].children[0].fileIndex == 1u);
    CHECK(rootNode.children[1].kind == woby::UiSceneNodeKind::file);
    CHECK(rootNode.children[1].fileIndex == 2u);
}

TEST_CASE("scene bounds reuse cached group local bounds")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    auto& file = state.files[0];
    auto& group = file.groupSettings[0];

    REQUIRE(group.localBoundsValid);
    CHECK(group.localBounds.min[0] == doctest::Approx(0.0f));
    CHECK(group.localBounds.max[0] == doctest::Approx(1.0f));

    file.mesh.vertices[1].position[0] = 1000.0f;
    woby::recalculateSceneBounds(state);

    CHECK(state.sceneBounds.min[0] == doctest::Approx(0.0f));
    CHECK(state.sceneBounds.max[0] == doctest::Approx(1.0f));
}

TEST_CASE("event operations update top-level ui state")
{
    woby::UiState state;
    state.sceneBounds = makeBounds(4.0f, 8.0f);

    woby::setCameraOrbiting(state, true);
    woby::setCameraRolling(state, true);
    woby::setCameraPanning(state, true);
    CHECK(state.cameraInput.orbiting);
    CHECK(state.cameraInput.rolling);
    CHECK(state.cameraInput.panning);

    woby::setCameraOrbiting(state, false);
    woby::setCameraRolling(state, false);
    woby::setCameraPanning(state, false);
    CHECK_FALSE(state.cameraInput.orbiting);
    CHECK_FALSE(state.cameraInput.rolling);
    CHECK_FALSE(state.cameraInput.panning);

    CHECK(state.running);
    woby::requestQuit(state);
    CHECK_FALSE(state.running);

    CHECK(state.viewerPaneVisible);
    woby::toggleViewerPaneVisible(state);
    CHECK_FALSE(state.viewerPaneVisible);
    woby::setViewerPaneVisible(state, true);
    CHECK(state.viewerPaneVisible);

    CHECK(state.upAxis == woby::SceneUpAxis::z);
    woby::toggleSceneUpAxis(state);
    CHECK(state.upAxis == woby::SceneUpAxis::y);
    CHECK(state.camera.target[0] == doctest::Approx(state.sceneBounds.center[0]));
    CHECK(state.isDirty);
}

TEST_CASE("camera panning drags the scene with the cursor")
{
    woby::SceneCamera camera;
    camera.distance = 10.0f;
    camera.verticalFovDegrees = 60.0f;

    woby::panCamera(camera, 100.0f, 100.0f, 100.0f);

    CHECK(camera.target[0] == doctest::Approx(0.0f));
    CHECK(camera.target[1] > 0.0f);
    CHECK(camera.target[2] > 0.0f);
}

TEST_CASE("camera roll changes the view up direction")
{
    woby::SceneCamera camera;

    woby::rollCamera(camera, 100.0f);
    const bx::Vec3 up = woby::cameraUp(camera);

    CHECK(camera.rollRadians == doctest::Approx(0.6f));
    CHECK(up.x == doctest::Approx(0.0f));
    CHECK(up.y == doctest::Approx(-std::sin(0.6f)));
    CHECK(up.z == doctest::Approx(std::cos(0.6f)));
}

TEST_CASE("camera supports Y-up scene controls")
{
    woby::SceneCamera camera;
    camera.distance = 10.0f;
    camera.verticalFovDegrees = 60.0f;

    woby::panCamera(camera, 100.0f, 100.0f, 100.0f, woby::SceneUpAxis::y);
    const bx::Vec3 up = woby::cameraUp(camera, woby::SceneUpAxis::y);

    CHECK(camera.target[0] == doctest::Approx(0.0f));
    CHECK(camera.target[1] > 0.0f);
    CHECK(camera.target[2] < 0.0f);
    CHECK(up.x == doctest::Approx(0.0f));
    CHECK(up.y == doctest::Approx(1.0f));
    CHECK(up.z == doctest::Approx(0.0f));
}

TEST_CASE("camera panning follows the rolled screen axes")
{
    woby::SceneCamera camera;
    camera.distance = 10.0f;
    camera.verticalFovDegrees = 60.0f;
    camera.rollRadians = 3.14159265358979323846f * 0.5f;

    woby::panCamera(camera, 100.0f, 0.0f, 100.0f);

    CHECK(camera.target[0] == doctest::Approx(0.0f));
    CHECK(camera.target[1] == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(camera.target[2] > 0.0f);
}

TEST_CASE("dirty tracking follows persisted scene document only")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    const woby::SceneDocument cleanDocument = woby::createSceneDocument(state);

    woby::updateSceneDirty(state, cleanDocument);
    CHECK_FALSE(state.isDirty);

    woby::orbitUiCamera(state, 20.0f, 10.0f);
    woby::rollUiCamera(state, 8.0f);
    woby::panUiCamera(state, 5.0f, 3.0f, 720.0f);
    woby::dollyUiCamera(state, 0.5f);
    woby::toggleViewerPaneVisible(state);
    woby::updateSceneDirty(state, cleanDocument);
    CHECK_FALSE(state.isDirty);

    woby::setMasterVertexPointSize(state, 9.0f);
    woby::updateSceneDirty(state, cleanDocument);
    CHECK(state.isDirty);

    woby::setMasterVertexPointSize(state, woby::defaultMasterVertexPointSize);
    woby::setShowGrid(state, false);
    woby::updateSceneDirty(state, cleanDocument);
    CHECK(state.isDirty);

    const woby::SceneDocument newCleanDocument = woby::createSceneDocument(state);
    woby::updateSceneDirty(state, newCleanDocument);
    CHECK_FALSE(state.isDirty);
}

TEST_CASE("scene document mapping preserves ui-editable fields")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.masterVertexPointSize = 9.0f;
    woby::setShowOrigin(state, false);
    woby::setShowGrid(state, false);
    woby::setSceneUpAxis(state, woby::SceneUpAxis::y);
    auto& file = state.files[0];
    auto& group = file.groupSettings[0];

    woby::setFileVisible(file, false);
    woby::setFileVertexSizeScale(file, 2.5f);
    woby::setFileTranslation(file.fileSettings, {1.0f, 2.0f, 3.0f});
    woby::setFileRotationDegrees(file.fileSettings, {4.0f, 5.0f, 6.0f});
    woby::setFileScale(file.fileSettings, 3.0f);
    woby::setFileOpacity(file.fileSettings, 0.4f);
    woby::setGroupVisible(group, false);
    woby::setGroupRenderMode(group, woby::UiRenderMode::triangles, false);
    woby::setGroupVertexSizeScale(group, 1.75f);
    woby::setGroupTranslation(group, {7.0f, 8.0f, 9.0f});
    woby::setGroupRotationDegrees(group, {10.0f, 11.0f, 12.0f});
    woby::setGroupScale(group, 4.0f);
    woby::setGroupOpacity(group, 0.25f);
    woby::setGroupColor(group, {0.1f, 0.2f, 0.3f, 1.0f});

    const woby::SceneDocument document = woby::createSceneDocument(state);
    REQUIRE(document.files.size() == 1u);
    REQUIRE(document.files[0].groups.size() == 1u);
    REQUIRE(document.nodes.size() == 2u);
    CHECK(document.nodes[0].kind == woby::SceneNodeKind::file);
    CHECK(document.nodes[0].fileIndex == 0);
    CHECK(document.nodes[1].kind == woby::SceneNodeKind::group);
    CHECK(document.nodes[1].parentIndex == 0);
    CHECK(document.nodes[1].fileIndex == 0);
    CHECK(document.nodes[1].groupIndex == 0);
    CHECK(document.masterVertexPointSize == doctest::Approx(9.0f));
    CHECK_FALSE(document.showOrigin);
    CHECK_FALSE(document.showGrid);
    CHECK(document.upAxis == woby::SceneUpAxis::y);
    CHECK_FALSE(document.files[0].settings.visible);
    CHECK(document.files[0].vertexSizeScale == doctest::Approx(2.5f));
    CHECK_FALSE(document.files[0].groups[0].settings.visible);
    CHECK_FALSE(document.files[0].groups[0].settings.showTriangles);
    CHECK(document.files[0].groups[0].settings.scale == doctest::Approx(4.0f));
    CHECK(document.files[0].groups[0].settings.opacity == doctest::Approx(0.25f));
    CHECK(document.files[0].groups[0].settings.color[2] == doctest::Approx(0.3f));

    woby::UiFileState restored = makeFile("a.obj", "a", 0.0f, 1.0f, 0u);
    woby::applySceneFileRecord(restored, document.files[0]);
    CHECK_FALSE(restored.fileSettings.visible);
    CHECK(restored.vertexSizeScale == doctest::Approx(2.5f));
    CHECK_FALSE(restored.groupSettings[0].visible);
    CHECK_FALSE(restored.groupSettings[0].showTriangles);
    CHECK(restored.groupSettings[0].translation[2] == doctest::Approx(9.0f));
    CHECK(restored.groupSettings[0].color[1] == doctest::Approx(0.2f));
}

TEST_CASE("scene document mapping preserves nested scene nodes")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.files.push_back(makeFile("b.obj", "b", 2.0f, 3.0f, 1u));

    woby::UiSceneNode root;
    root.kind = woby::UiSceneNodeKind::folder;
    root.name = "models";
    woby::setSceneNodeTranslation(root.settings, {1.0f, 2.0f, 3.0f});
    root.children.push_back(woby::createFileSceneNode(state.files[0], 0u));

    woby::UiSceneNode nested;
    nested.kind = woby::UiSceneNodeKind::folder;
    nested.name = "nested";
    nested.children.push_back(woby::createFileSceneNode(state.files[1], 1u));
    root.children.push_back(std::move(nested));
    state.sceneNodes.push_back(std::move(root));

    const woby::SceneDocument document = woby::createSceneDocument(state);
    REQUIRE(document.nodes.size() == 6u);
    CHECK(document.nodes[0].kind == woby::SceneNodeKind::folder);
    CHECK(document.nodes[0].parentIndex == -1);
    CHECK(document.nodes[0].settings.translation[1] == doctest::Approx(2.0f));
    CHECK(document.nodes[1].kind == woby::SceneNodeKind::file);
    CHECK(document.nodes[1].parentIndex == 0);
    CHECK(document.nodes[3].kind == woby::SceneNodeKind::folder);
    CHECK(document.nodes[3].parentIndex == 0);
    CHECK(document.nodes[4].kind == woby::SceneNodeKind::file);
    CHECK(document.nodes[4].parentIndex == 3);

    woby::UiState restored;
    restored.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    restored.files.push_back(makeFile("b.obj", "b", 2.0f, 3.0f, 1u));
    woby::applySceneNodeRecords(restored, document.nodes);

    REQUIRE(restored.sceneNodes.size() == 1u);
    CHECK(restored.sceneNodes[0].name == "models");
    CHECK(restored.sceneNodes[0].settings.translation[2] == doctest::Approx(3.0f));
    REQUIRE(restored.sceneNodes[0].children.size() == 2u);
    CHECK(restored.sceneNodes[0].children[1].name == "nested");
}

TEST_CASE("scene document writer omits camera state")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.camera.distance = 42.0f;
    const woby::SceneDocument document = woby::createSceneDocument(state);
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_scene_document_writer_omits_camera_state.woby";

    woby::writeSceneDocument(path, document);
    std::string text;
    {
        std::ifstream stream(path);
        text.assign(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>());
    }
    std::filesystem::remove(path);

    CHECK(text.find("[camera]") == std::string::npos);
    CHECK(text.find("distance") == std::string::npos);
    CHECK(text.find("vertical_fov_degrees") == std::string::npos);
}

TEST_CASE("scene document persists helper visibility and up axis")
{
    woby::SceneDocument document;
    document.showOrigin = false;
    document.showGrid = false;
    document.upAxis = woby::SceneUpAxis::y;
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_scene_document_helper_visibility.woby";

    woby::writeSceneDocument(path, document);
    const woby::SceneDocument restored = woby::readSceneDocument(path);

    CHECK_FALSE(restored.showOrigin);
    CHECK_FALSE(restored.showGrid);
    CHECK(restored.upAxis == woby::SceneUpAxis::y);

    std::filesystem::remove(path);
}

TEST_CASE("scene record apply clamps invalid persisted numeric values")
{
    woby::UiFileState file = makeFile("a.obj", "a", 0.0f, 1.0f, 0u);
    woby::SceneFileRecord record;
    record.settings.scale = -5.0f;
    record.settings.opacity = 5.0f;
    record.settings.translation = {
        std::numeric_limits<float>::quiet_NaN(),
        2.0f,
        3.0f,
    };
    record.vertexSizeScale = 50.0f;

    woby::SceneGroupRecord groupRecord;
    groupRecord.settings.scale = 100.0f;
    groupRecord.settings.opacity = -1.0f;
    groupRecord.settings.vertexSizeScale = -10.0f;
    groupRecord.settings.rotationDegrees = {
        300.0f,
        -300.0f,
        std::numeric_limits<float>::quiet_NaN(),
    };
    groupRecord.settings.color = {-1.0f, 2.0f, std::numeric_limits<float>::quiet_NaN(), 4.0f};
    record.groups.push_back(groupRecord);

    woby::applySceneFileRecord(file, record);

    CHECK(file.fileSettings.scale == doctest::Approx(woby::minGroupScale));
    CHECK(file.fileSettings.opacity == doctest::Approx(woby::maxGroupOpacity));
    CHECK(file.fileSettings.translation[0] == doctest::Approx(0.0f));
    CHECK(file.vertexSizeScale == doctest::Approx(woby::maxVertexSizeScale));
    CHECK(file.groupSettings[0].scale == doctest::Approx(woby::maxGroupScale));
    CHECK(file.groupSettings[0].opacity == doctest::Approx(woby::minGroupOpacity));
    CHECK(file.groupSettings[0].vertexSizeScale == doctest::Approx(woby::minVertexSizeScale));
    CHECK(file.groupSettings[0].rotationDegrees[0] == doctest::Approx(woby::maxRotationDegrees));
    CHECK(file.groupSettings[0].rotationDegrees[1] == doctest::Approx(woby::minRotationDegrees));
    CHECK(file.groupSettings[0].rotationDegrees[2] == doctest::Approx(0.0f));
    CHECK(file.groupSettings[0].color[0] == doctest::Approx(0.0f));
    CHECK(file.groupSettings[0].color[1] == doctest::Approx(1.0f));
    CHECK(file.groupSettings[0].color[2] == doctest::Approx(1.0f));
    CHECK(file.groupSettings[0].color[3] == doctest::Approx(1.0f));
}

TEST_CASE("scene document round trips generated editable state")
{
    std::mt19937 random(0x5c3e47u);
    const std::filesystem::path scenePath = std::filesystem::temp_directory_path()
        / "woby_scene_document_generated_round_trip.woby";

    for (size_t iteration = 0; iteration < 32u; ++iteration) {
        woby::SceneDocument document;
        document.masterVertexPointSize = generatedFloat(
            random,
            woby::minVertexPointSize,
            woby::maxVertexPointSize);
        document.showOrigin = (iteration & 1u) == 0u;
        document.showGrid = (iteration & 2u) == 0u;
        document.upAxis = (iteration & 4u) == 0u ? woby::SceneUpAxis::z : woby::SceneUpAxis::y;

        const size_t fileCount = 1u + iteration % 4u;
        for (size_t fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
            woby::SceneFileRecord file;
            file.path = std::filesystem::temp_directory_path()
                / ("woby generated model " + std::to_string(iteration) + "_" + std::to_string(fileIndex) + ".obj");
            file.settings.visible = (fileIndex + iteration) % 3u != 0u;
            file.settings.scale = generatedFloat(random, woby::minGroupScale, woby::maxGroupScale);
            file.settings.opacity = generatedFloat(random, woby::minGroupOpacity, woby::maxGroupOpacity);
            file.settings.translation = generatedFloat3(random, -100.0f, 100.0f);
            file.settings.rotationDegrees = generatedFloat3(
                random,
                woby::minRotationDegrees,
                woby::maxRotationDegrees);
            file.vertexSizeScale = generatedFloat(
                random,
                woby::minVertexSizeScale,
                woby::maxVertexSizeScale);

            const size_t groupCount = 1u + (iteration + fileIndex) % 5u;
            for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
                woby::SceneGroupRecord group;
                group.name = "generated \"group\" \\" + std::to_string(iteration) + "_"
                    + std::to_string(fileIndex) + "_" + std::to_string(groupIndex);
                group.settings.visible = (groupIndex + iteration) % 2u == 0u;
                group.settings.showSolidMesh = (groupIndex & 1u) == 0u;
                group.settings.showTriangles = (groupIndex & 2u) == 0u;
                group.settings.showVertices = (groupIndex & 4u) == 0u;
                group.settings.scale = generatedFloat(random, woby::minGroupScale, woby::maxGroupScale);
                group.settings.opacity = generatedFloat(random, woby::minGroupOpacity, woby::maxGroupOpacity);
                group.settings.vertexSizeScale = generatedFloat(
                    random,
                    woby::minVertexSizeScale,
                    woby::maxVertexSizeScale);
                group.settings.translation = generatedFloat3(random, -50.0f, 50.0f);
                group.settings.rotationDegrees = generatedFloat3(
                    random,
                    woby::minRotationDegrees,
                    woby::maxRotationDegrees);
                group.settings.color = generatedFloat4(random, 0.0f, 1.0f);
                file.groups.push_back(group);
            }

            document.files.push_back(file);
        }

        woby::writeSceneDocument(scenePath, document);
        const woby::SceneDocument restored = woby::readSceneDocument(scenePath);

        CHECK(restored.files.size() == document.files.size());
        CHECK(restored.masterVertexPointSize == doctest::Approx(document.masterVertexPointSize));
        CHECK(restored.showOrigin == document.showOrigin);
        CHECK(restored.showGrid == document.showGrid);
        CHECK(restored.upAxis == document.upAxis);

        for (size_t fileIndex = 0; fileIndex < document.files.size(); ++fileIndex) {
            const auto& expectedFile = document.files[fileIndex];
            const auto& actualFile = restored.files[fileIndex];
            CHECK(woby::sceneAbsolutePath(scenePath, actualFile.path)
                == std::filesystem::absolute(expectedFile.path).lexically_normal());
            CHECK(actualFile.settings.visible == expectedFile.settings.visible);
            CHECK(actualFile.settings.scale == doctest::Approx(expectedFile.settings.scale));
            CHECK(actualFile.settings.opacity == doctest::Approx(expectedFile.settings.opacity));
            compareFloat3(actualFile.settings.translation, expectedFile.settings.translation);
            compareFloat3(actualFile.settings.rotationDegrees, expectedFile.settings.rotationDegrees);
            CHECK(actualFile.vertexSizeScale == doctest::Approx(expectedFile.vertexSizeScale));
            REQUIRE(actualFile.groups.size() == expectedFile.groups.size());

            for (size_t groupIndex = 0; groupIndex < expectedFile.groups.size(); ++groupIndex) {
                const auto& expectedGroup = expectedFile.groups[groupIndex];
                const auto& actualGroup = actualFile.groups[groupIndex];
                CHECK(actualGroup.name == expectedGroup.name);
                CHECK(actualGroup.settings.visible == expectedGroup.settings.visible);
                CHECK(actualGroup.settings.showSolidMesh == expectedGroup.settings.showSolidMesh);
                CHECK(actualGroup.settings.showTriangles == expectedGroup.settings.showTriangles);
                CHECK(actualGroup.settings.showVertices == expectedGroup.settings.showVertices);
                CHECK(actualGroup.settings.scale == doctest::Approx(expectedGroup.settings.scale));
                CHECK(actualGroup.settings.opacity == doctest::Approx(expectedGroup.settings.opacity));
                CHECK(actualGroup.settings.vertexSizeScale == doctest::Approx(expectedGroup.settings.vertexSizeScale));
                compareFloat3(actualGroup.settings.translation, expectedGroup.settings.translation);
                compareFloat3(actualGroup.settings.rotationDegrees, expectedGroup.settings.rotationDegrees);
                compareFloat4(actualGroup.settings.color, expectedGroup.settings.color);
            }
        }
    }

    std::filesystem::remove(scenePath);
}

TEST_CASE("generated ui operations preserve clamping and visibility invariants")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.files.push_back(makeFile("b.obj", "b", 2.0f, 4.0f, 1u));
    state.files.push_back(makeFile("c.obj", "c", 5.0f, 9.0f, 2u));
    state.files[1].mesh.nodes.push_back({"b-extra", 0u, 3u});
    state.files[1].groupSettings.push_back(woby::UiGroupState{});
    state.files[2].mesh.nodes.push_back({"c-extra", 0u, 3u});
    state.files[2].groupSettings.push_back(woby::UiGroupState{});

    std::mt19937 random(0x0b70f5u);
    for (size_t iteration = 0; iteration < 256u; ++iteration) {
        const size_t fileIndex = std::uniform_int_distribution<size_t>(0u, state.files.size() - 1u)(random);
        auto& file = state.files[fileIndex];
        const size_t groupIndex = std::uniform_int_distribution<size_t>(0u, file.groupSettings.size() - 1u)(random);
        auto& group = file.groupSettings[groupIndex];
        const bool flag = std::uniform_int_distribution<int>(0, 1)(random) != 0;
        const float numeric = generatedFloat(random, -1000.0f, 1000.0f);

        switch (iteration % 15u) {
        case 0u:
            woby::setFileVisible(file, flag);
            break;
        case 1u:
            woby::setGroupVisible(file, group, flag);
            break;
        case 2u:
            woby::setAllSceneVisible(state, flag);
            break;
        case 3u:
            woby::setAllSceneRenderModes(state, woby::UiRenderMode::solidMesh, flag);
            break;
        case 4u:
            woby::setAllSceneRenderModes(state, woby::UiRenderMode::triangles, flag);
            break;
        case 5u:
            woby::setAllSceneRenderModes(state, woby::UiRenderMode::vertices, flag);
            break;
        case 6u:
            woby::setMasterVertexPointSize(state, numeric);
            break;
        case 7u:
            woby::setFileVertexSizeScale(file, numeric);
            break;
        case 8u:
            woby::setGroupVertexSizeScale(group, numeric);
            break;
        case 9u:
            woby::setFileTranslation(file.fileSettings, generatedFloat3(random, -1000.0f, 1000.0f));
            break;
        case 10u:
            woby::setGroupTranslation(group, generatedFloat3(random, -1000.0f, 1000.0f));
            break;
        case 11u:
            woby::setFileRotationDegrees(file.fileSettings, generatedFloat3(random, -1000.0f, 1000.0f));
            break;
        case 12u:
            woby::setGroupRotationDegrees(group, generatedFloat3(random, -1000.0f, 1000.0f));
            break;
        case 13u:
            woby::setFileScale(file.fileSettings, numeric);
            woby::setFileOpacity(file.fileSettings, numeric);
            break;
        default:
            woby::setGroupScale(group, numeric);
            woby::setGroupOpacity(group, numeric);
            woby::setGroupColor(group, generatedFloat4(random, -5.0f, 5.0f));
            break;
        }

        checkVisibilityInvariants(state);
        CHECK(state.masterVertexPointSize >= woby::minVertexPointSize);
        CHECK(state.masterVertexPointSize <= woby::maxVertexPointSize);
        for (const auto& checkedFile : state.files) {
            checkClampedFileSettings(checkedFile);
            for (const auto& checkedGroup : checkedFile.groupSettings) {
                checkClampedGroupSettings(checkedGroup);
            }
        }
        CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::solidMesh) <= woby::totalGroupCount(state));
        CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::triangles) <= woby::totalGroupCount(state));
        CHECK(woby::countEnabledSceneRenderMode(state, woby::UiRenderMode::vertices) <= woby::totalGroupCount(state));
    }
}

TEST_CASE("generated persisted scene records clamp values and synchronize visibility")
{
    std::mt19937 random(0x7219fdu);

    for (size_t iteration = 0; iteration < 128u; ++iteration) {
        woby::UiFileState file = makeFile("generated.obj", "generated", 0.0f, 1.0f, 0u);
        file.mesh.nodes.push_back({"generated-extra", 0u, 3u});
        file.groupSettings.push_back(woby::UiGroupState{});

        woby::SceneFileRecord record;
        record.settings.visible = (iteration % 5u) != 0u;
        record.settings.scale = generatedFloat(random, -1000.0f, 1000.0f);
        record.settings.opacity = generatedFloat(random, -1000.0f, 1000.0f);
        record.settings.translation = generatedFloat3(random, -1000.0f, 1000.0f);
        record.settings.rotationDegrees = generatedFloat3(random, -1000.0f, 1000.0f);
        record.vertexSizeScale = generatedFloat(random, -1000.0f, 1000.0f);

        for (size_t groupIndex = 0; groupIndex < file.groupSettings.size(); ++groupIndex) {
            woby::SceneGroupRecord group;
            group.name = "generated";
            group.settings.visible = ((iteration + groupIndex) % 3u) != 0u;
            group.settings.showSolidMesh = (groupIndex & 1u) == 0u;
            group.settings.showTriangles = (groupIndex & 2u) == 0u;
            group.settings.showVertices = (groupIndex & 4u) == 0u;
            group.settings.scale = generatedFloat(random, -1000.0f, 1000.0f);
            group.settings.opacity = generatedFloat(random, -1000.0f, 1000.0f);
            group.settings.vertexSizeScale = generatedFloat(random, -1000.0f, 1000.0f);
            group.settings.translation = generatedFloat3(random, -1000.0f, 1000.0f);
            group.settings.rotationDegrees = generatedFloat3(random, -1000.0f, 1000.0f);
            group.settings.color = generatedFloat4(random, -1000.0f, 1000.0f);
            record.groups.push_back(group);
        }

        woby::applySceneFileRecord(file, record);

        checkClampedFileSettings(file);
        for (const auto& group : file.groupSettings) {
            checkClampedGroupSettings(group);
        }
        if (!record.settings.visible) {
            CHECK_FALSE(file.fileSettings.visible);
            CHECK(woby::countVisibleGroups(file.groupSettings) == 0u);
        } else {
            CHECK(file.fileSettings.visible == (woby::countVisibleGroups(file.groupSettings) != 0u));
        }
    }
}

TEST_CASE("generated camera operations keep finite clamped view state")
{
    std::mt19937 random(0xca4e12u);
    for (const woby::SceneUpAxis upAxis : {woby::SceneUpAxis::z, woby::SceneUpAxis::y}) {
        woby::SceneCamera camera;
        camera.distance = 10.0f;
        camera.verticalFovDegrees = 60.0f;

        for (size_t iteration = 0; iteration < 512u; ++iteration) {
            switch (iteration % 5u) {
            case 0u:
                woby::orbitCamera(
                    camera,
                    generatedFloat(random, -250.0f, 250.0f),
                    generatedFloat(random, -250.0f, 250.0f),
                    upAxis);
                break;
            case 1u:
                woby::rollCamera(camera, generatedFloat(random, -250.0f, 250.0f));
                break;
            case 2u:
                woby::panCamera(
                    camera,
                    generatedFloat(random, -400.0f, 400.0f),
                    generatedFloat(random, -400.0f, 400.0f),
                    generatedFloat(random, 1.0f, 2000.0f),
                    upAxis);
                break;
            case 3u:
                woby::dollyCamera(camera, generatedFloat(random, -0.4f, 0.4f));
                break;
            default:
                woby::moveCameraLocal(
                    camera,
                    generatedFloat(random, -5.0f, 5.0f),
                    generatedFloat(random, -5.0f, 5.0f),
                    generatedFloat(random, -5.0f, 5.0f),
                    upAxis);
                break;
            }

            CHECK(camera.pitchRadians >= -1.45f);
            CHECK(camera.pitchRadians <= 1.45f);
            CHECK(camera.distance >= 0.001f);
            CHECK(std::isfinite(camera.distance));
            CHECK(finiteVec3(camera.target));
            CHECK(finiteBxVec3(woby::cameraEye(camera, upAxis)));
            CHECK(finiteBxVec3(woby::cameraLookAt(camera)));
            CHECK(finiteBxVec3(woby::cameraUp(camera, upAxis)));
            CHECK(std::isfinite(woby::cameraFarPlane(camera, makeBounds(-1.0f, 1.0f))));
        }
    }
}
