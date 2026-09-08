#pragma once

#include "camera.h"
#include "comparison_report.h"
#include "model_mesh.h"
#include "scene_file.h"
#include "scene_objects.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace woby {

inline constexpr float defaultMasterVertexPointSize = 4.0f;
inline constexpr float minVertexPointSize = 1.0f;
inline constexpr float maxVertexPointSize = 40.0f;
inline constexpr float minVertexSizeScale = 0.1f;
inline constexpr float maxVertexSizeScale = 10.0f;
inline constexpr float minGroupScale = 0.01f;
inline constexpr float maxGroupScale = 20.0f;
inline constexpr float minGroupOpacity = 0.0f;
inline constexpr float maxGroupOpacity = 1.0f;
inline constexpr float minRotationDegrees = -180.0f;
inline constexpr float maxRotationDegrees = 180.0f;
inline constexpr float defaultDisplayBoundsMin = -10.0f;
inline constexpr float defaultDisplayBoundsMax = 10.0f;

struct UiGroupState {
    bool visible = true;
    bool showSolidMesh = true;
    bool showTriangles = false;
    bool showVertices = false;
    float scale = 1.0f;
    float opacity = 1.0f;
    float vertexSizeScale = 1.0f;
    std::array<float, 3> center{};
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
    std::array<float, 4> color{};
    Bounds localBounds;
    bool localBoundsValid = false;
    SceneObjectId objectId = invalidSceneObjectId;
};

struct UiFileSettings {
    bool visible = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    std::array<float, 3> center{};
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
};

struct UiFileState {
    std::filesystem::path path;
    std::string importerId;
    Mesh mesh;
    std::vector<UiGroupState> groupSettings;
    UiFileSettings fileSettings;
    float vertexSizeScale = 1.0f;
    SceneObjectId objectId = invalidSceneObjectId;
};

inline constexpr size_t invalidSceneNodeIndex = static_cast<size_t>(-1);

enum class UiSceneNodeKind {
    folder,
    file,
    group,
};

struct UiSceneNodeSettings {
    bool visible = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    std::array<float, 3> center{};
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
};

struct UiSceneNode {
    UiSceneNodeKind kind = UiSceneNodeKind::folder;
    std::string name;
    UiSceneNodeSettings settings;
    size_t fileIndex = invalidSceneNodeIndex;
    size_t groupIndex = invalidSceneNodeIndex;
    std::vector<UiSceneNode> children;
    // Folder identity, or the referenced file/group's canonical identity.
    SceneObjectId objectId = invalidSceneObjectId;
};

struct UiComparisonPart {
    SceneObjectId objectId = invalidSceneObjectId;
    // Retained when a source is removed, so missing inputs remain repairable.
    std::string name;
    friend bool operator==(const UiComparisonPart&, const UiComparisonPart&) = default;
};

struct UiComparison {
    SceneObjectId objectId = invalidSceneObjectId;
    std::string name;
    ComparisonSettings settings;
    // Presentation only: source measurements always use source world transforms.
    std::array<float, 3> translation{};
    std::vector<UiComparisonPart> a, b;
};

struct UiState {
    // Application preference, excluded from scene persistence and dirty tracking.
    float uiScale = 1.0f;
    // Export preferences are session-only, not scene content or dirty state.
    ScreenshotSettings screenshotSettings;
    std::vector<UiComparison> comparisons;
    // Last active comparison, retained for source membership commands and badges.
    SceneObjectId activeComparisonId = invalidSceneObjectId;
    // Transient tree/canvas selection in click order; excluded from scene files and dirty tracking.
    std::vector<SceneObjectId> selectedSceneObjects;
    bool running = true;
    bool isDirty = false;
    bool showOrigin = false;
    bool showGrid = false;
    SceneUpAxis upAxis = SceneUpAxis::z;
    float masterVertexPointSize = defaultMasterVertexPointSize;
    SceneCamera camera;
    CameraInput cameraInput;
    Bounds sceneBounds;
    float viewerPaneWidth = 0.0f;
    bool viewerPaneVisible = true;
    // Shared right-hand inspector visibility; session-only, not scene content.
    bool propertiesPaneVisible = false;
    std::vector<UiFileState> files;
    std::vector<UiSceneNode> sceneNodes;
    // Session metadata: never saved, reset on scene open, or used for dirty tracking.
    SceneObjectId nextObjectId = 1;
};

[[nodiscard]] std::array<float, 4> defaultGroupColor(size_t groupIndex);
[[nodiscard]] std::array<float, 3> nodeCenter(const Mesh& mesh, const MeshNode& node);
[[nodiscard]] std::vector<UiGroupState> createUiGroupStates(const Mesh& mesh, size_t firstColorIndex);
[[nodiscard]] UiFileState createUiFileState(
    std::filesystem::path modelPath,
    Mesh mesh,
    size_t firstColorIndex,
    std::string importerId = {});
void groupTransformMatrix(const UiGroupState& settings, float* model);
void fileTransformMatrix(const UiFileSettings& settings, float* model);
void sceneNodeTransformMatrix(const UiSceneNodeSettings& settings, float* model);
[[nodiscard]] Bounds defaultDisplayBounds();
[[nodiscard]] Bounds combineBounds(const std::vector<UiFileState>& files);
[[nodiscard]] Bounds combineBounds(
    const std::vector<UiFileState>& files,
    const std::vector<UiSceneNode>& sceneNodes);

[[nodiscard]] UiSceneNode createFileSceneNode(const UiFileState& file, size_t fileIndex);
void appendDefaultSceneNodesForFiles(UiState& state, size_t firstFileIndex);
void refreshSceneTreeFolderVisibility(UiState& state);
void refreshSceneTreeFolderCenters(UiState& state);

[[nodiscard]] SceneFileSettings sceneFileSettings(const UiFileSettings& settings);
[[nodiscard]] SceneGroupSettings sceneGroupSettings(const UiGroupState& settings);
[[nodiscard]] SceneNodeSettings sceneNodeSettings(const UiSceneNodeSettings& settings);
[[nodiscard]] SceneDocument createSceneDocument(const UiState& state);
void applySceneFileRecord(UiFileState& file, const SceneFileRecord& record);
void applySceneNodeRecords(UiState& state, const std::vector<SceneNodeRecord>& records);

} // namespace woby
