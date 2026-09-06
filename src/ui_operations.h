#pragma once

#include "ui_state.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <vector>

namespace woby {

// An omitted ID targets the last active comparison (also retained while inspecting sources).
[[nodiscard]] const UiComparison* findComparison(const UiState& state, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] UiComparison* findComparison(UiState& state, SceneObjectId id = invalidSceneObjectId);
// Only a single selected comparison is shown in the shared Properties pane.
[[nodiscard]] const UiComparison* selectedComparison(const UiState& state);
[[nodiscard]] ComparisonSettings comparisonSettings(const UiState& state, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] bool comparisonContains(const UiState& state, SceneObjectId part, ComparisonSide side,
    SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] size_t missingComparisonPartCount(const UiState& state, SceneObjectId id = invalidSceneObjectId);
SceneObjectId createComparison(UiState& state);
SceneObjectId duplicateComparison(UiState& state, SceneObjectId id);
void removeComparison(UiState& state, SceneObjectId id);
void renameComparison(UiState& state, SceneObjectId id, const std::string& name);
void setComparisonTranslation(UiState& state, SceneObjectId id, const std::array<float, 3>& translation);
void removeMissingComparisonParts(UiState& state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);
void frameComparison(UiState& state, SceneObjectId id);
void setComparisonSettings(UiState& state, ComparisonSettings settings, SceneObjectId id = invalidSceneObjectId);
void setPropertiesPaneVisible(UiState& state, bool visible);
void setScreenshotSettings(UiState& state, ScreenshotSettings settings);
void frameComparisonBounds(UiState& state, const Bounds& bounds);
[[nodiscard]] bool sceneObjectSelected(const UiState& state, SceneObjectId id);
// Plain click replaces, Ctrl-click toggles, context click preserves selection and includes the clicked object.
void selectSceneObject(UiState& state, SceneObjectId id, bool toggle = false, bool contextClick = false);
void clearSceneSelection(UiState& state);

enum class UiObjectProperty {
    translationX, translationY, translationZ,
    rotationX, rotationY, rotationZ,
    scale, opacity, vertexSize, solidMesh, triangles, vertices, red, green, blue,
};
enum class UiPropertyGroup { translation, rotation, scale, transform, appearance };
struct UiPropertyValue {
    float value = 0.0f;
    bool available = false;
    bool mixed = false;
};
// Values are local to each selected object. A property is available only when
// every target supports it. Comparisons use their dedicated inspector.
[[nodiscard]] UiPropertyValue selectedObjectProperty(const UiState& state, UiObjectProperty property);
// Edit only this component on explicit targets, including both parent and child
// when both are selected. Never expand a parent selection to its descendants.
void setSelectedObjectProperty(UiState& state, UiObjectProperty property, float value);
void resetSelectedObjectProperties(UiState& state, UiPropertyGroup group);
// Files/folders expand to their current triangular mesh parts. IDs are deduplicated.
[[nodiscard]] std::vector<SceneObjectId> comparisonObjectParts(
    const UiState& state, const std::vector<SceneObjectId>& objects);
[[nodiscard]] size_t comparisonPartCount(const UiState& state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);
enum class ComparisonMembershipAction { unavailable, add, remove };
// Mixed selections add missing parts; fully included selections remove their parts.
[[nodiscard]] ComparisonMembershipAction comparisonMembershipAction(
    const UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);
[[nodiscard]] bool canCompareGroups(const UiState& state, SceneObjectId id = invalidSceneObjectId);
// Create a new comparison from two distinct selected objects in click order.
[[nodiscard]] bool canCompareSceneSelection(const UiState& state);
bool compareSceneSelection(UiState& state);
void setComparisonObjects(UiState& state, const std::vector<SceneObjectId>& objects, ComparisonSide side, bool member,
    SceneObjectId id = invalidSceneObjectId);
void clearComparisonGroup(UiState& state, ComparisonSide side, SceneObjectId id = invalidSceneObjectId);
void swapComparisonGroups(UiState& state, SceneObjectId id = invalidSceneObjectId);

enum class UiRenderMode {
    solidMesh,
    triangles,
    vertices,
};

enum class UiInspectionPreset { solid, edges, vertices };
// Presets edit the existing persisted display properties on all current parts.
void applyInspectionPreset(UiState& state, UiInspectionPreset preset);
void setUiScale(UiState& state, float scale);

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
