#pragma once

#include "camera.h"
#include "model_mesh.h"
#include "scene_file.h"

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
    bool showTriangles = true;
    bool showVertices = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    float vertexSizeScale = 1.0f;
    std::array<float, 3> center{};
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
    std::array<float, 4> color{};
    Bounds localBounds;
    bool localBoundsValid = false;
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
    Mesh mesh;
    std::vector<UiGroupState> groupSettings;
    UiFileSettings fileSettings;
    float vertexSizeScale = 1.0f;
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
};

struct UiState {
    bool running = true;
    bool isDirty = false;
    bool showOrigin = true;
    bool showGrid = true;
    SceneUpAxis upAxis = SceneUpAxis::z;
    float masterVertexPointSize = defaultMasterVertexPointSize;
    SceneCamera camera;
    CameraInput cameraInput;
    Bounds sceneBounds;
    float viewerPaneWidth = 0.0f;
    bool viewerPaneVisible = true;
    std::vector<UiFileState> files;
    std::vector<UiSceneNode> sceneNodes;
};

[[nodiscard]] std::array<float, 4> defaultGroupColor(size_t groupIndex);
[[nodiscard]] std::array<float, 3> nodeCenter(const Mesh& mesh, const MeshNode& node);
[[nodiscard]] std::vector<UiGroupState> createUiGroupStates(const Mesh& mesh, size_t firstColorIndex);
[[nodiscard]] UiFileState createUiFileState(
    std::filesystem::path modelPath,
    Mesh mesh,
    size_t firstColorIndex);
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
void refreshSceneTreeFolderCenters(UiState& state);

[[nodiscard]] SceneFileSettings sceneFileSettings(const UiFileSettings& settings);
[[nodiscard]] SceneGroupSettings sceneGroupSettings(const UiGroupState& settings);
[[nodiscard]] SceneNodeSettings sceneNodeSettings(const UiSceneNodeSettings& settings);
[[nodiscard]] SceneDocument createSceneDocument(const UiState& state);
void applySceneFileRecord(UiFileState& file, const SceneFileRecord& record);
void applySceneNodeRecords(UiState& state, const std::vector<SceneNodeRecord>& records);

} // namespace woby
