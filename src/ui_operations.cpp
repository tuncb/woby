#include "ui_operations.h"

#include <algorithm>
#include <cmath>

namespace woby {
namespace {

float finiteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float clampFinite(float value, float minValue, float maxValue, float fallback)
{
    return std::clamp(finiteOr(value, fallback), minValue, maxValue);
}

std::array<float, 3> finiteArrayOrZero(const std::array<float, 3>& value)
{
    return {
        finiteOr(value[0], 0.0f),
        finiteOr(value[1], 0.0f),
        finiteOr(value[2], 0.0f),
    };
}

std::array<float, 3> clampFiniteArray(
    const std::array<float, 3>& value,
    float minValue,
    float maxValue)
{
    return {
        clampFinite(value[0], minValue, maxValue, 0.0f),
        clampFinite(value[1], minValue, maxValue, 0.0f),
        clampFinite(value[2], minValue, maxValue, 0.0f),
    };
}

} // namespace

size_t totalGroupCount(const std::vector<UiFileState>& files)
{
    size_t groupCount = 0;
    for (const auto& file : files) {
        groupCount += file.groupSettings.size();
    }

    return groupCount;
}

size_t totalGroupCount(const UiState& state)
{
    return totalGroupCount(state.files);
}

size_t countVisibleGroups(const std::vector<UiGroupState>& groups)
{
    size_t visibleCount = 0;
    for (const auto& group : groups) {
        if (group.visible) {
            ++visibleCount;
        }
    }

    return visibleCount;
}

size_t countVisibleFileGroups(const UiFileState& file)
{
    if (!file.fileSettings.visible) {
        return 0u;
    }

    return countVisibleGroups(file.groupSettings);
}

size_t countVisibleSceneGroups(const UiState& state)
{
    size_t visibleCount = 0;
    for (const auto& file : state.files) {
        visibleCount += countVisibleFileGroups(file);
    }

    return visibleCount;
}

size_t countEnabledGroupRenderMode(const std::vector<UiGroupState>& groups, UiRenderMode mode)
{
    size_t enabledCount = 0;
    for (const auto& group : groups) {
        if (groupRenderModeEnabled(group, mode)) {
            ++enabledCount;
        }
    }

    return enabledCount;
}

size_t countEnabledSceneRenderMode(const UiState& state, UiRenderMode mode)
{
    size_t enabledCount = 0;
    for (const auto& file : state.files) {
        enabledCount += countEnabledGroupRenderMode(file.groupSettings, mode);
    }

    return enabledCount;
}

bool groupRenderModeEnabled(const UiGroupState& group, UiRenderMode mode)
{
    switch (mode) {
    case UiRenderMode::solidMesh:
        return group.showSolidMesh;
    case UiRenderMode::triangles:
        return group.showTriangles;
    case UiRenderMode::vertices:
        return group.showVertices;
    }

    return false;
}

void setGroupRenderMode(UiGroupState& group, UiRenderMode mode, bool enabled)
{
    switch (mode) {
    case UiRenderMode::solidMesh:
        group.showSolidMesh = enabled;
        break;
    case UiRenderMode::triangles:
        group.showTriangles = enabled;
        break;
    case UiRenderMode::vertices:
        group.showVertices = enabled;
        break;
    }
}

void toggleGroupRenderMode(UiGroupState& group, UiRenderMode mode)
{
    setGroupRenderMode(group, mode, !groupRenderModeEnabled(group, mode));
}

void setAllGroupRenderModes(std::vector<UiGroupState>& groups, UiRenderMode mode, bool enabled)
{
    for (auto& group : groups) {
        setGroupRenderMode(group, mode, enabled);
    }
}

void setAllSceneRenderModes(UiState& state, UiRenderMode mode, bool enabled)
{
    bool changed = false;
    for (auto& file : state.files) {
        for (const auto& group : file.groupSettings) {
            changed = changed || groupRenderModeEnabled(group, mode) != enabled;
        }
        setAllGroupRenderModes(file.groupSettings, mode, enabled);
    }
    if (changed) {
        markSceneDirty(state);
    }
}

void setFileVisible(UiFileState& file, bool visible)
{
    file.fileSettings.visible = visible;
    for (auto& group : file.groupSettings) {
        group.visible = visible;
    }
}

void toggleFileVisible(UiFileState& file)
{
    setFileVisible(file, countVisibleFileGroups(file) != file.groupSettings.size());
}

void setAllSceneVisible(UiState& state, bool visible)
{
    bool changed = false;
    for (auto& file : state.files) {
        changed = changed || file.fileSettings.visible != visible;
        for (const auto& group : file.groupSettings) {
            changed = changed || group.visible != visible;
        }
        setFileVisible(file, visible);
    }
    if (changed) {
        markSceneDirty(state);
    }
}

void setGroupVisible(UiGroupState& group, bool visible)
{
    group.visible = visible;
}

void toggleGroupVisible(UiGroupState& group)
{
    setGroupVisible(group, !group.visible);
}

void setGroupVisible(UiFileState& file, UiGroupState& group, bool visible)
{
    setGroupVisible(group, visible);
    if (visible) {
        file.fileSettings.visible = true;
        return;
    }

    if (countVisibleGroups(file.groupSettings) == 0u) {
        file.fileSettings.visible = false;
    }
}

void toggleGroupVisible(UiFileState& file, UiGroupState& group)
{
    setGroupVisible(file, group, !group.visible);
}

void setShowOrigin(UiState& state, bool visible)
{
    if (state.showOrigin != visible) {
        state.showOrigin = visible;
        markSceneDirty(state);
    }
}

void toggleShowOrigin(UiState& state)
{
    setShowOrigin(state, !state.showOrigin);
}

void setShowGrid(UiState& state, bool visible)
{
    if (state.showGrid != visible) {
        state.showGrid = visible;
        markSceneDirty(state);
    }
}

void toggleShowGrid(UiState& state)
{
    setShowGrid(state, !state.showGrid);
}

void setMasterVertexPointSize(UiState& state, float value)
{
    const float clampedValue = clampFinite(
        value,
        minVertexPointSize,
        maxVertexPointSize,
        defaultMasterVertexPointSize);
    if (state.masterVertexPointSize != clampedValue) {
        state.masterVertexPointSize = clampedValue;
        markSceneDirty(state);
    }
}

void setFileVertexSizeScale(UiFileState& file, float value)
{
    file.vertexSizeScale = clampFinite(value, minVertexSizeScale, maxVertexSizeScale, 1.0f);
}

void setGroupVertexSizeScale(UiGroupState& group, float value)
{
    group.vertexSizeScale = clampFinite(value, minVertexSizeScale, maxVertexSizeScale, 1.0f);
}

void setFileTranslation(UiFileSettings& settings, const std::array<float, 3>& value)
{
    settings.translation = finiteArrayOrZero(value);
}

void setGroupTranslation(UiGroupState& group, const std::array<float, 3>& value)
{
    group.translation = finiteArrayOrZero(value);
}

void setFileRotationDegrees(UiFileSettings& settings, const std::array<float, 3>& value)
{
    settings.rotationDegrees = clampFiniteArray(value, minRotationDegrees, maxRotationDegrees);
}

void setGroupRotationDegrees(UiGroupState& group, const std::array<float, 3>& value)
{
    group.rotationDegrees = clampFiniteArray(value, minRotationDegrees, maxRotationDegrees);
}

void setFileScale(UiFileSettings& settings, float value)
{
    settings.scale = clampFinite(value, minGroupScale, maxGroupScale, 1.0f);
}

void setGroupScale(UiGroupState& group, float value)
{
    group.scale = clampFinite(value, minGroupScale, maxGroupScale, 1.0f);
}

void setFileOpacity(UiFileSettings& settings, float value)
{
    settings.opacity = clampFinite(value, minGroupOpacity, maxGroupOpacity, 1.0f);
}

void setGroupOpacity(UiGroupState& group, float value)
{
    group.opacity = clampFinite(value, minGroupOpacity, maxGroupOpacity, 1.0f);
}

void setGroupColor(UiGroupState& group, const std::array<float, 4>& value)
{
    group.color = {
        clampFinite(value[0], 0.0f, 1.0f, 1.0f),
        clampFinite(value[1], 0.0f, 1.0f, 1.0f),
        clampFinite(value[2], 0.0f, 1.0f, 1.0f),
        clampFinite(value[3], 0.0f, 1.0f, 1.0f),
    };
}

void resetGroupColor(UiGroupState& group, size_t colorIndex)
{
    group.color = defaultGroupColor(colorIndex);
}

void resetGroupTransform(UiGroupState& group)
{
    group.scale = 1.0f;
    group.opacity = 1.0f;
    group.translation = {};
    group.rotationDegrees = {};
}

void resetFileTransform(UiFileSettings& settings)
{
    settings.scale = 1.0f;
    settings.opacity = 1.0f;
    settings.translation = {};
    settings.rotationDegrees = {};
}

bool groupTransformIsDefault(const UiGroupState& group)
{
    return group.scale == 1.0f
        && group.opacity == 1.0f
        && group.translation == std::array<float, 3>{}
        && group.rotationDegrees == std::array<float, 3>{};
}

bool fileTransformIsDefault(const UiFileSettings& settings)
{
    return settings.scale == 1.0f
        && settings.opacity == 1.0f
        && settings.translation == std::array<float, 3>{}
        && settings.rotationDegrees == std::array<float, 3>{};
}

void recalculateSceneBounds(UiState& state)
{
    state.sceneBounds = combineBounds(state.files);
}

void frameCameraToScene(UiState& state)
{
    state.camera = frameCameraBounds(state.sceneBounds);
}

bool removeFileFromState(UiState& state, size_t fileIndex)
{
    if (fileIndex >= state.files.size()) {
        return false;
    }

    state.files.erase(state.files.begin() + static_cast<std::ptrdiff_t>(fileIndex));
    recalculateSceneBounds(state);
    frameCameraToScene(state);
    markSceneDirty(state);
    return true;
}

void setSceneDirty(UiState& state, bool dirty)
{
    state.isDirty = dirty;
}

void markSceneDirty(UiState& state)
{
    setSceneDirty(state, true);
}

void clearSceneDirty(UiState& state)
{
    setSceneDirty(state, false);
}

void updateSceneDirty(UiState& state, const SceneDocument& cleanDocument)
{
    setSceneDirty(state, createSceneDocument(state) != cleanDocument);
}

void setViewerPaneWidth(UiState& state, float value, float minWidth, float maxWidth)
{
    state.viewerPaneWidth = std::clamp(
        finiteOr(value, minWidth),
        minWidth,
        std::max(minWidth, maxWidth));
}

void setViewerPaneVisible(UiState& state, bool visible)
{
    state.viewerPaneVisible = visible;
}

void toggleViewerPaneVisible(UiState& state)
{
    setViewerPaneVisible(state, !state.viewerPaneVisible);
}

void requestQuit(UiState& state)
{
    state.running = false;
}

void setCameraOrbiting(UiState& state, bool enabled)
{
    state.cameraInput.orbiting = enabled;
}

void setCameraRolling(UiState& state, bool enabled)
{
    state.cameraInput.rolling = enabled;
}

void setCameraPanning(UiState& state, bool enabled)
{
    state.cameraInput.panning = enabled;
}

void orbitUiCamera(UiState& state, float deltaX, float deltaY)
{
    orbitCamera(state.camera, deltaX, deltaY);
}

void rollUiCamera(UiState& state, float deltaX)
{
    rollCamera(state.camera, deltaX);
}

void panUiCamera(UiState& state, float deltaX, float deltaY, float viewportHeight)
{
    panCamera(state.camera, deltaX, deltaY, viewportHeight);
}

void dollyUiCamera(UiState& state, float amount)
{
    dollyCamera(state.camera, amount);
}

} // namespace woby
