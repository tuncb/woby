#pragma once

#include "ui_state.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <vector>

namespace woby {

void setComparisonSettings(UiState& state, ComparisonSettings settings);
void setComparisonPaneVisible(UiState& state, bool visible);
void frameComparisonBounds(UiState& state, const Bounds& bounds);
[[nodiscard]] bool sceneObjectSelected(const UiState& state, SceneObjectId id);
// Plain click replaces, Ctrl-click toggles, context click preserves an existing selection.
void selectSceneObject(UiState& state, SceneObjectId id, bool toggle = false, bool contextClick = false);
void clearSceneSelection(UiState& state);
// Files/folders expand to their current triangular mesh parts. IDs are deduplicated.
[[nodiscard]] std::vector<SceneObjectId> comparisonObjectParts(
    const UiState& state, const std::vector<SceneObjectId>& objects);
[[nodiscard]] size_t comparisonPartCount(const UiState& state, ComparisonSide side);
enum class ComparisonMembershipAction { unavailable, add, remove };
// Mixed selections add missing parts; fully included selections remove their parts.
[[nodiscard]] ComparisonMembershipAction comparisonMembershipAction(
    const UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side);
[[nodiscard]] bool canCompareGroups(const UiState& state);
// With both sides empty, assign two distinct selected objects to A/B in click order.
[[nodiscard]] bool canCompareSceneSelection(const UiState& state);
bool compareSceneSelection(UiState& state);
void setComparisonObjects(UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side, bool member);
void clearComparisonGroup(UiState& state, ComparisonSide side);
void swapComparisonGroups(UiState& state);

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
[[nodiscard]] size_t countSceneNodeGroups(const UiState& state, const UiSceneNode& node);
[[nodiscard]] size_t countVisibleSceneNodeGroups(const UiState& state, const UiSceneNode& node);
[[nodiscard]] size_t countEnabledGroupRenderMode(
    const std::vector<UiGroupState>& groups,
    UiRenderMode mode);
[[nodiscard]] size_t countEnabledSceneRenderMode(const UiState& state, UiRenderMode mode);
[[nodiscard]] size_t countEnabledSceneNodeRenderMode(
    const UiState& state,
    const UiSceneNode& node,
    UiRenderMode mode);
[[nodiscard]] bool groupRenderModeEnabled(const UiGroupState& group, UiRenderMode mode);
void setGroupRenderMode(UiGroupState& group, UiRenderMode mode, bool enabled);
void toggleGroupRenderMode(UiGroupState& group, UiRenderMode mode);
void setAllGroupRenderModes(std::vector<UiGroupState>& groups, UiRenderMode mode, bool enabled);
void setAllSceneRenderModes(UiState& state, UiRenderMode mode, bool enabled);
void setSceneNodeSubtreeRenderMode(
    UiState& state,
    UiSceneNode& node,
    UiRenderMode mode,
    bool enabled);

void setFileVisible(UiFileState& file, bool visible);
void toggleFileVisible(UiFileState& file);
void setAllSceneVisible(UiState& state, bool visible);
void setSceneNodeSubtreeVisible(UiState& state, UiSceneNode& node, bool visible);
void setGroupVisible(UiGroupState& group, bool visible);
void toggleGroupVisible(UiGroupState& group);
void setGroupVisible(UiFileState& file, UiGroupState& group, bool visible);
void toggleGroupVisible(UiFileState& file, UiGroupState& group);
void setGroupVisible(UiState& state, UiFileState& file, UiGroupState& group, bool visible);
void toggleGroupVisible(UiState& state, UiFileState& file, UiGroupState& group);
void setShowOrigin(UiState& state, bool visible);
void toggleShowOrigin(UiState& state);
void setShowGrid(UiState& state, bool visible);
void toggleShowGrid(UiState& state);
void setSceneUpAxis(UiState& state, SceneUpAxis upAxis);
void toggleSceneUpAxis(UiState& state);

void setMasterVertexPointSize(UiState& state, float value);
void setFileVertexSizeScale(UiFileState& file, float value);
void setGroupVertexSizeScale(UiGroupState& group, float value);
void setFileTranslation(UiFileSettings& settings, const std::array<float, 3>& value);
void setSceneNodeTranslation(UiSceneNodeSettings& settings, const std::array<float, 3>& value);
void setGroupTranslation(UiGroupState& group, const std::array<float, 3>& value);
void setFileRotationDegrees(UiFileSettings& settings, const std::array<float, 3>& value);
void setSceneNodeRotationDegrees(UiSceneNodeSettings& settings, const std::array<float, 3>& value);
void setGroupRotationDegrees(UiGroupState& group, const std::array<float, 3>& value);
void setFileScale(UiFileSettings& settings, float value);
void setSceneNodeScale(UiSceneNodeSettings& settings, float value);
void setGroupScale(UiGroupState& group, float value);
void setFileOpacity(UiFileSettings& settings, float value);
void setSceneNodeOpacity(UiSceneNodeSettings& settings, float value);
void setGroupOpacity(UiGroupState& group, float value);
void setGroupColor(UiGroupState& group, const std::array<float, 4>& value);
void resetGroupColor(UiGroupState& group, size_t colorIndex);
void resetGroupTransform(UiGroupState& group);
void resetFileTransform(UiFileSettings& settings);
void resetSceneNodeTransform(UiSceneNodeSettings& settings);

[[nodiscard]] bool groupTransformIsDefault(const UiGroupState& group);
[[nodiscard]] bool fileTransformIsDefault(const UiFileSettings& settings);
[[nodiscard]] bool sceneNodeTransformIsDefault(const UiSceneNodeSettings& settings);

void recalculateSceneBounds(UiState& state);
void frameCameraToScene(UiState& state);
void appendFolderTreeSceneNode(
    UiState& state,
    const std::filesystem::path& root,
    size_t firstFileIndex,
    size_t fileCount);
bool removeFileFromState(UiState& state, size_t fileIndex);
// Prepare without mutating the live state; commit only after resources are ready.
[[nodiscard]] UiState prepareSceneReplacement(const UiState& current,
    std::vector<UiFileState> files, const SceneDocument& document);
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

struct CameraNavigation {
    float yawDegrees = 0, pitchDegrees = 0, rollDegrees = 0;
    float right = 0, up = 0, forward = 0;
    float distanceFactor = 1;
};
// Angles change the stored camera angles; translation uses rolled camera-local units.
// Reject non-finite input/results without changing the camera.
void navigateUiCamera(UiState& state, const CameraNavigation& navigation);

} // namespace woby
