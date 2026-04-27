#pragma once

#include "camera.h"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace woby {

struct SceneGroupSettings {
    bool visible = true;
    bool showSolidMesh = true;
    bool showTriangles = true;
    bool showVertices = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    float vertexSizeScale = 1.0f;
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
    std::array<float, 4> color{};
};

struct SceneFileSettings {
    bool visible = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};
};

struct SceneGroupRecord {
    std::string name;
    SceneGroupSettings settings;
};

struct SceneFileRecord {
    std::filesystem::path path;
    SceneFileSettings settings;
    float vertexSizeScale = 1.0f;
    std::vector<SceneGroupRecord> groups;
};

struct SceneDocument {
    float masterVertexPointSize = 4.0f;
    SceneCamera camera;
    bool cameraLoaded = false;
    std::vector<SceneFileRecord> files;
};

[[nodiscard]] SceneDocument readSceneDocument(const std::filesystem::path& scenePath);
void writeSceneDocument(const std::filesystem::path& scenePath, const SceneDocument& document);

[[nodiscard]] std::filesystem::path sceneAbsolutePath(
    const std::filesystem::path& scenePath,
    const std::filesystem::path& storedPath);
[[nodiscard]] std::filesystem::path sceneSavePathWithExtension(std::filesystem::path path);

} // namespace woby
