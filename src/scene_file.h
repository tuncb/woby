#pragma once

#include "scene_up_axis.h"
#include "comparison_settings.h"
#include "camera.h"

#include <array>
#include <filesystem>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace woby {

struct SceneGroupSettings {
    // Legacy v2-v4 read mapping; cleared when migrating to comparison records.
    ComparisonMembership comparison;
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

    friend bool operator==(const SceneGroupSettings&, const SceneGroupSettings&) = default;
};

struct SceneFileSettings {
    bool visible = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};

    friend bool operator==(const SceneFileSettings&, const SceneFileSettings&) = default;
};

struct SceneGroupRecord {
    std::string name;
    SceneGroupSettings settings;

    friend bool operator==(const SceneGroupRecord&, const SceneGroupRecord&) = default;
};

struct SceneFileRecord {
    std::filesystem::path path;
    std::string importerId;
    SceneFileSettings settings;
    float vertexSizeScale = 1.0f;
    std::vector<SceneGroupRecord> groups;

    friend bool operator==(const SceneFileRecord&, const SceneFileRecord&) = default;
};

enum class SceneNodeKind {
    folder,
    file,
    group,
};

struct SceneNodeSettings {
    bool visible = true;
    float scale = 1.0f;
    float opacity = 1.0f;
    std::array<float, 3> translation{};
    std::array<float, 3> rotationDegrees{};

    friend bool operator==(const SceneNodeSettings&, const SceneNodeSettings&) = default;
};

struct SceneNodeRecord {
    SceneNodeKind kind = SceneNodeKind::folder;
    std::string name;
    int parentIndex = -1;
    int fileIndex = -1;
    int groupIndex = -1;
    SceneNodeSettings settings;

    friend bool operator==(const SceneNodeRecord&, const SceneNodeRecord&) = default;
};

struct SceneComparisonPartRecord {
    int fileIndex = -1;
    int groupIndex = -1;
    std::string name;
    bool enabled = true;
    friend bool operator==(const SceneComparisonPartRecord&, const SceneComparisonPartRecord&) = default;
};

struct SceneComparisonRecord {
    std::string name;
    ComparisonSettings settings;
    // Older scenes have no result position. Resolve an omitted position once,
    // after source geometry is loaded; an explicit zero is an intentional overlay.
    std::optional<std::array<float, 3>> translation;
    std::vector<SceneComparisonPartRecord> a, b;
    friend bool operator==(const SceneComparisonRecord&, const SceneComparisonRecord&) = default;
};

enum class ViewObjectKind { folder, file, group, comparison };

// Display values only. Geometry, names and hierarchy are never restored by a view.
struct ViewObjectSettings {
    SceneGroupSettings appearance;
    ComparisonSettings comparison;
    int selectionOrder = -1;
    friend bool operator==(const ViewObjectSettings&, const ViewObjectSettings&) = default;
};

struct SceneViewObjectRecord {
    ViewObjectKind kind = ViewObjectKind::group;
    int index = -1; // File, flattened folder node, or comparison index.
    int groupIndex = -1;
    ViewObjectSettings settings;
    friend bool operator==(const SceneViewObjectRecord&, const SceneViewObjectRecord&) = default;
};

struct ViewSceneSettings {
    SceneCamera camera;
    bool showOrigin = false, showGrid = false, showDimensions = false;
    SceneUpAxis upAxis = SceneUpAxis::z;
    float masterVertexPointSize = 4.0f;
    friend bool operator==(const ViewSceneSettings&, const ViewSceneSettings&) = default;
};

struct SceneViewPartRecord {
    int comparisonIndex = -1, fileIndex = -1, groupIndex = -1;
    ComparisonSide side = ComparisonSide::a;
    bool enabled = true;
    friend bool operator==(const SceneViewPartRecord&, const SceneViewPartRecord&) = default;
};

struct SceneViewRecord {
    std::string name;
    ViewSceneSettings scene;
    std::vector<SceneViewObjectRecord> objects;
    std::vector<SceneViewPartRecord> parts;
    friend bool operator==(const SceneViewRecord&, const SceneViewRecord&) = default;
};

struct SceneDocument {
    std::vector<SceneViewRecord> views;
    // Saved review view; absent in legacy scenes. Excluded from edits/history.
    std::optional<SceneCamera> camera;
    std::vector<SceneComparisonRecord> comparisons;
    // Legacy v2-v4 input only. New scenes store comparison objects above.
    ComparisonSettings comparison;
    float masterVertexPointSize = 4.0f;
    bool showOrigin = true;
    bool showGrid = true;
    bool showDimensions = false;
    SceneUpAxis upAxis = SceneUpAxis::z;
    std::vector<SceneFileRecord> files;
    std::vector<SceneNodeRecord> nodes;

    friend bool operator==(const SceneDocument&, const SceneDocument&) = default;
};

[[nodiscard]] SceneDocument readSceneDocument(const std::filesystem::path& scenePath);
[[nodiscard]] bool sceneContentEqual(const SceneDocument& a, const SceneDocument& b);
void writeSceneDocument(const std::filesystem::path& scenePath, const SceneDocument& document, bool overwrite = true);

[[nodiscard]] std::filesystem::path sceneAbsolutePath(
    const std::filesystem::path& scenePath,
    const std::filesystem::path& storedPath);
[[nodiscard]] std::filesystem::path sceneSavePathWithExtension(std::filesystem::path path);

} // namespace woby
