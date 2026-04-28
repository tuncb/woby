#include "ui_operations.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

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

TEST_CASE("event operations update top-level ui state")
{
    woby::UiState state;

    woby::setCameraOrbiting(state, true);
    woby::setCameraPanning(state, true);
    CHECK(state.cameraInput.orbiting);
    CHECK(state.cameraInput.panning);

    woby::setCameraOrbiting(state, false);
    woby::setCameraPanning(state, false);
    CHECK_FALSE(state.cameraInput.orbiting);
    CHECK_FALSE(state.cameraInput.panning);

    CHECK(state.running);
    woby::requestQuit(state);
    CHECK_FALSE(state.running);
}

TEST_CASE("scene document mapping preserves ui-editable fields")
{
    woby::UiState state;
    state.files.push_back(makeFile("a.obj", "a", 0.0f, 1.0f, 0u));
    state.masterVertexPointSize = 9.0f;
    state.camera.distance = 42.0f;
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
    CHECK(document.camera.distance == doctest::Approx(42.0f));
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
    CHECK_FALSE(restored.groupSettings[0].showTriangles);
    CHECK(restored.groupSettings[0].translation[2] == doctest::Approx(9.0f));
    CHECK(restored.groupSettings[0].color[1] == doctest::Approx(0.2f));
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
