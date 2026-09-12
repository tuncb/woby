#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace woby {

using SceneObjectId = uint64_t;
inline constexpr SceneObjectId invalidSceneObjectId = 0;

enum class SceneObjectKind {
    folder,
    file,
    group,
    comparison,
    annotation,
};

struct SceneObjectInfo {
    SceneObjectId id = invalidSceneObjectId;
    SceneObjectKind kind = SceneObjectKind::folder;
    std::string name;
    std::filesystem::path path;
    SceneObjectId fileId = invalidSceneObjectId;
};

struct UiState;

// Assign only at scene construction/commit boundaries. Existing objects retain
// their IDs; the allocator belongs to UiState and must survive scene replacement.
// Newly created objects (including copies attached as new objects) start with ID 0.
void assignSceneObjectIds(UiState& state);
[[nodiscard]] std::vector<SceneObjectInfo> sceneObjects(const UiState& state);
// Unknown, removed and zero IDs do not resolve. Resolve afresh for each command.
[[nodiscard]] std::optional<SceneObjectInfo> findSceneObject(const UiState& state, SceneObjectId id);

} // namespace woby
