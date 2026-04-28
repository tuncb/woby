#pragma once

#include "camera.h"
#include "obj_mesh.h"
#include "scene_file.h"

#include <array>
#include <filesystem>
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
    ObjMesh mesh;
    std::vector<UiGroupState> groupSettings;
    UiFileSettings fileSettings;
    float vertexSizeScale = 1.0f;
};

struct UiState {
    bool running = true;
    bool isDirty = false;
    bool showOrigin = true;
    bool showGrid = true;
    float masterVertexPointSize = defaultMasterVertexPointSize;
    SceneCamera camera;
    CameraInput cameraInput;
    Bounds sceneBounds;
    float viewerPaneWidth = 0.0f;
    std::vector<UiFileState> files;
};

[[nodiscard]] std::array<float, 4> defaultGroupColor(size_t groupIndex);
[[nodiscard]] std::array<float, 3> nodeCenter(const ObjMesh& mesh, const ObjNode& node);
[[nodiscard]] std::vector<UiGroupState> createUiGroupStates(const ObjMesh& mesh, size_t firstColorIndex);
[[nodiscard]] UiFileState createUiFileState(
    std::filesystem::path modelPath,
    ObjMesh mesh,
    size_t firstColorIndex);
void groupTransformMatrix(const UiGroupState& settings, float* model);
void fileTransformMatrix(const UiFileSettings& settings, float* model);
[[nodiscard]] Bounds defaultDisplayBounds();
[[nodiscard]] Bounds combineBounds(const std::vector<UiFileState>& files);

[[nodiscard]] SceneFileSettings sceneFileSettings(const UiFileSettings& settings);
[[nodiscard]] SceneGroupSettings sceneGroupSettings(const UiGroupState& settings);
[[nodiscard]] SceneDocument createSceneDocument(const UiState& state);
void applySceneFileRecord(UiFileState& file, const SceneFileRecord& record);

} // namespace woby
