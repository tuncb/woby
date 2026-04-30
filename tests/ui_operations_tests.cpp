#include "ui_operations.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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

woby::ObjMesh makeMesh(const char* groupName, float minX, float maxX)
{
    woby::ObjMesh mesh;
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
    CHECK(camera.target[1] < 0.0f);
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
    CHECK(camera.target[2] > 0.0f);
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
    CHECK(camera.target[2] < 0.0f);
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

TEST_CASE("scene document persists helper visibility and up axis and defaults old files")
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

    {
        std::ofstream stream(path, std::ios::trunc);
        stream << "# woby scene\n";
        stream << "version = 1\n";
        stream << "master_vertex_point_size = 4\n";
    }

    const woby::SceneDocument oldStyleDocument = woby::readSceneDocument(path);
    std::filesystem::remove(path);

    CHECK(oldStyleDocument.showOrigin);
    CHECK(oldStyleDocument.showGrid);
    CHECK(oldStyleDocument.upAxis == woby::SceneUpAxis::z);
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
