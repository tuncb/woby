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

bool validFileIndex(const UiState& state, size_t fileIndex)
{
    return fileIndex != invalidSceneNodeIndex && fileIndex < state.files.size();
}

bool validGroupIndex(const UiState& state, size_t fileIndex, size_t groupIndex)
{
    return validFileIndex(state, fileIndex)
        && groupIndex != invalidSceneNodeIndex
        && groupIndex < state.files[fileIndex].groupSettings.size();
}

bool pruneRemovedFile(UiSceneNode& node, size_t removedFileIndex)
{
    if ((node.kind == UiSceneNodeKind::file || node.kind == UiSceneNodeKind::group)
        && node.fileIndex == removedFileIndex) {
        return false;
    }

    if ((node.kind == UiSceneNodeKind::file || node.kind == UiSceneNodeKind::group)
        && node.fileIndex != invalidSceneNodeIndex
        && node.fileIndex > removedFileIndex) {
        --node.fileIndex;
    }

    node.children.erase(
        std::remove_if(
            node.children.begin(),
            node.children.end(),
            [removedFileIndex](UiSceneNode& child) {
                return !pruneRemovedFile(child, removedFileIndex);
            }),
        node.children.end());

    return node.kind != UiSceneNodeKind::folder || !node.children.empty();
}

bool setFolderNodesVisible(UiSceneNode& node, bool visible)
{
    bool changed = false;
    if (node.kind == UiSceneNodeKind::folder) {
        changed = node.settings.visible != visible;
        node.settings.visible = visible;
    }

    for (auto& child : node.children) {
        changed = setFolderNodesVisible(child, visible) || changed;
    }
    return changed;
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
    if (!state.sceneNodes.empty()) {
        size_t visibleCount = 0;
        for (const auto& node : state.sceneNodes) {
            visibleCount += countVisibleSceneNodeGroups(state, node);
        }
        return visibleCount;
    }

    size_t visibleCount = 0;
    for (const auto& file : state.files) {
        visibleCount += countVisibleFileGroups(file);
    }

    return visibleCount;
}

size_t countSceneNodeGroups(const UiState& state, const UiSceneNode& node)
{
    if (node.kind == UiSceneNodeKind::group) {
        return validGroupIndex(state, node.fileIndex, node.groupIndex) ? 1u : 0u;
    }

    if (node.kind == UiSceneNodeKind::file
        && validFileIndex(state, node.fileIndex)
        && node.children.empty()) {
        return state.files[node.fileIndex].groupSettings.size();
    }

    size_t groupCount = 0;
    for (const auto& child : node.children) {
        groupCount += countSceneNodeGroups(state, child);
    }
    return groupCount;
}

size_t countVisibleSceneNodeGroups(const UiState& state, const UiSceneNode& node)
{
    if (node.kind == UiSceneNodeKind::folder && !node.settings.visible) {
        return 0u;
    }

    if (node.kind == UiSceneNodeKind::group) {
        if (!validGroupIndex(state, node.fileIndex, node.groupIndex)) {
            return 0u;
        }
        const auto& file = state.files[node.fileIndex];
        return file.fileSettings.visible && file.groupSettings[node.groupIndex].visible ? 1u : 0u;
    }

    if (node.kind == UiSceneNodeKind::file) {
        if (!validFileIndex(state, node.fileIndex)) {
            return 0u;
        }
        const auto& file = state.files[node.fileIndex];
        if (!file.fileSettings.visible) {
            return 0u;
        }
        if (node.children.empty()) {
            return countVisibleFileGroups(file);
        }
    }

    size_t visibleCount = 0;
    for (const auto& child : node.children) {
        visibleCount += countVisibleSceneNodeGroups(state, child);
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
    if (!state.sceneNodes.empty()) {
        size_t enabledCount = 0;
        for (const auto& node : state.sceneNodes) {
            enabledCount += countEnabledSceneNodeRenderMode(state, node, mode);
        }
        return enabledCount;
    }

    size_t enabledCount = 0;
    for (const auto& file : state.files) {
        enabledCount += countEnabledGroupRenderMode(file.groupSettings, mode);
    }

    return enabledCount;
}

size_t countEnabledSceneNodeRenderMode(
    const UiState& state,
    const UiSceneNode& node,
    UiRenderMode mode)
{
    if (node.kind == UiSceneNodeKind::group) {
        if (!validGroupIndex(state, node.fileIndex, node.groupIndex)) {
            return 0u;
        }
        return groupRenderModeEnabled(state.files[node.fileIndex].groupSettings[node.groupIndex], mode) ? 1u : 0u;
    }

    if (node.kind == UiSceneNodeKind::file
        && validFileIndex(state, node.fileIndex)
        && node.children.empty()) {
        return countEnabledGroupRenderMode(state.files[node.fileIndex].groupSettings, mode);
    }

    size_t enabledCount = 0;
    for (const auto& child : node.children) {
        enabledCount += countEnabledSceneNodeRenderMode(state, child, mode);
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

void setSceneNodeSubtreeRenderMode(
    UiState& state,
    UiSceneNode& node,
    UiRenderMode mode,
    bool enabled)
{
    if (node.kind == UiSceneNodeKind::group) {
        if (validGroupIndex(state, node.fileIndex, node.groupIndex)) {
            setGroupRenderMode(state.files[node.fileIndex].groupSettings[node.groupIndex], mode, enabled);
        }
        return;
    }

    if (node.kind == UiSceneNodeKind::file
        && validFileIndex(state, node.fileIndex)
        && node.children.empty()) {
        setAllGroupRenderModes(state.files[node.fileIndex].groupSettings, mode, enabled);
        return;
    }

    for (auto& child : node.children) {
        setSceneNodeSubtreeRenderMode(state, child, mode, enabled);
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
    for (auto& node : state.sceneNodes) {
        changed = setFolderNodesVisible(node, visible) || changed;
    }
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

void setSceneNodeSubtreeVisible(UiState& state, UiSceneNode& node, bool visible)
{
    if (node.kind == UiSceneNodeKind::folder) {
        node.settings.visible = visible;
    } else if (node.kind == UiSceneNodeKind::file) {
        if (validFileIndex(state, node.fileIndex)) {
            setFileVisible(state.files[node.fileIndex], visible);
        }
    } else if (validGroupIndex(state, node.fileIndex, node.groupIndex)) {
        auto& file = state.files[node.fileIndex];
        setGroupVisible(file, file.groupSettings[node.groupIndex], visible);
    }

    for (auto& child : node.children) {
        setSceneNodeSubtreeVisible(state, child, visible);
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

void setSceneUpAxis(UiState& state, SceneUpAxis upAxis)
{
    if (state.upAxis != upAxis) {
        state.upAxis = upAxis;
        frameCameraToScene(state);
        markSceneDirty(state);
    }
}

void toggleSceneUpAxis(UiState& state)
{
    setSceneUpAxis(
        state,
        state.upAxis == SceneUpAxis::z ? SceneUpAxis::y : SceneUpAxis::z);
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

void setSceneNodeTranslation(UiSceneNodeSettings& settings, const std::array<float, 3>& value)
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

void setSceneNodeRotationDegrees(UiSceneNodeSettings& settings, const std::array<float, 3>& value)
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

void setSceneNodeScale(UiSceneNodeSettings& settings, float value)
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

void setSceneNodeOpacity(UiSceneNodeSettings& settings, float value)
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

void resetSceneNodeTransform(UiSceneNodeSettings& settings)
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

bool sceneNodeTransformIsDefault(const UiSceneNodeSettings& settings)
{
    return settings.scale == 1.0f
        && settings.opacity == 1.0f
        && settings.translation == std::array<float, 3>{}
        && settings.rotationDegrees == std::array<float, 3>{};
}

void recalculateSceneBounds(UiState& state)
{
    state.sceneBounds = combineBounds(state.files, state.sceneNodes);
}

void frameCameraToScene(UiState& state)
{
    state.camera = frameCameraBounds(state.sceneBounds, state.upAxis);
}

bool removeFileFromState(UiState& state, size_t fileIndex)
{
    if (fileIndex >= state.files.size()) {
        return false;
    }

    state.files.erase(state.files.begin() + static_cast<std::ptrdiff_t>(fileIndex));
    state.sceneNodes.erase(
        std::remove_if(
            state.sceneNodes.begin(),
            state.sceneNodes.end(),
            [fileIndex](UiSceneNode& node) {
                return !pruneRemovedFile(node, fileIndex);
            }),
        state.sceneNodes.end());
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
    orbitCamera(state.camera, deltaX, deltaY, state.upAxis);
}

void rollUiCamera(UiState& state, float deltaX)
{
    rollCamera(state.camera, deltaX);
}

void panUiCamera(UiState& state, float deltaX, float deltaY, float viewportHeight)
{
    panCamera(state.camera, deltaX, deltaY, viewportHeight, state.upAxis);
}

void dollyUiCamera(UiState& state, float amount)
{
    dollyCamera(state.camera, amount);
}

} // namespace woby
