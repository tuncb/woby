#pragma once

#include "ui_state.h"

#include <array>
#include <cstddef>
#include <vector>

namespace woby {

enum class UiRenderMode {
    solidMesh,
    triangles,
    vertices,
};

[[nodiscard]] size_t totalGroupCount(const std::vector<UiFileState>& files);
[[nodiscard]] size_t totalGroupCount(const UiState& state);
[[nodiscard]] size_t countVisibleGroups(const std::vector<UiGroupState>& groups);
[[nodiscard]] size_t countVisibleFileGroups(const UiFileState& file);
[[nodiscard]] size_t countVisibleSceneGroups(const UiState& state);
[[nodiscard]] size_t countEnabledGroupRenderMode(
    const std::vector<UiGroupState>& groups,
    UiRenderMode mode);
[[nodiscard]] size_t countEnabledSceneRenderMode(const UiState& state, UiRenderMode mode);
[[nodiscard]] bool groupRenderModeEnabled(const UiGroupState& group, UiRenderMode mode);
void setGroupRenderMode(UiGroupState& group, UiRenderMode mode, bool enabled);
void toggleGroupRenderMode(UiGroupState& group, UiRenderMode mode);
void setAllGroupRenderModes(std::vector<UiGroupState>& groups, UiRenderMode mode, bool enabled);
void setAllSceneRenderModes(UiState& state, UiRenderMode mode, bool enabled);

void setFileVisible(UiFileState& file, bool visible);
void toggleFileVisible(UiFileState& file);
void setAllSceneVisible(UiState& state, bool visible);
void setGroupVisible(UiGroupState& group, bool visible);
void toggleGroupVisible(UiGroupState& group);
void setGroupVisible(UiFileState& file, UiGroupState& group, bool visible);
void toggleGroupVisible(UiFileState& file, UiGroupState& group);
void setShowOrigin(UiState& state, bool visible);
void toggleShowOrigin(UiState& state);
void setShowGrid(UiState& state, bool visible);
void toggleShowGrid(UiState& state);

void setMasterVertexPointSize(UiState& state, float value);
void setFileVertexSizeScale(UiFileState& file, float value);
void setGroupVertexSizeScale(UiGroupState& group, float value);
void setFileTranslation(UiFileSettings& settings, const std::array<float, 3>& value);
void setGroupTranslation(UiGroupState& group, const std::array<float, 3>& value);
void setFileRotationDegrees(UiFileSettings& settings, const std::array<float, 3>& value);
void setGroupRotationDegrees(UiGroupState& group, const std::array<float, 3>& value);
void setFileScale(UiFileSettings& settings, float value);
void setGroupScale(UiGroupState& group, float value);
void setFileOpacity(UiFileSettings& settings, float value);
void setGroupOpacity(UiGroupState& group, float value);
void setGroupColor(UiGroupState& group, const std::array<float, 4>& value);
void resetGroupColor(UiGroupState& group, size_t colorIndex);
void resetGroupTransform(UiGroupState& group);
void resetFileTransform(UiFileSettings& settings);

[[nodiscard]] bool groupTransformIsDefault(const UiGroupState& group);
[[nodiscard]] bool fileTransformIsDefault(const UiFileSettings& settings);

void recalculateSceneBounds(UiState& state);
void frameCameraToScene(UiState& state);
bool removeFileFromState(UiState& state, size_t fileIndex);
void setSceneDirty(UiState& state, bool dirty);
void markSceneDirty(UiState& state);
void clearSceneDirty(UiState& state);
void updateSceneDirty(UiState& state, const SceneDocument& cleanDocument);
void setViewerPaneWidth(UiState& state, float value, float minWidth, float maxWidth);
void setViewerPaneVisible(UiState& state, bool visible);
void toggleViewerPaneVisible(UiState& state);
void requestQuit(UiState& state);
void setCameraOrbiting(UiState& state, bool enabled);
void setCameraRolling(UiState& state, bool enabled);
void setCameraPanning(UiState& state, bool enabled);
void orbitUiCamera(UiState& state, float deltaX, float deltaY);
void rollUiCamera(UiState& state, float deltaX);
void panUiCamera(UiState& state, float deltaX, float deltaY, float viewportHeight);
void dollyUiCamera(UiState& state, float amount);

} // namespace woby
