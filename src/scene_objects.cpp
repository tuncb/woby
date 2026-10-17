#include "scene_objects.h"
#include "ui_state.h"
#include "utf8_path.h"

#include <algorithm>
#include <stdexcept>

namespace woby {
std::string normalizedSceneName(std::string_view name)
{
    const auto first = name.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string_view::npos) { return {}; }
    name = name.substr(first, name.find_last_not_of(" \t\r\n\f\v") - first + 1);
    if (std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) { return {}; }
    return std::string(name);
}

std::string sceneCustomName(std::string_view name, std::string_view original)
{
    auto normalized = normalizedSceneName(name);
    return name == original || normalized == original ? std::string{} : normalized;
}

std::string modelDisplayName(const UiFileState& file)
{
    if (!file.customName.empty()) { return file.customName; }
    const auto filename = pathToUtf8(file.path.filename());
    return filename.empty() ? pathToUtf8(file.path) : filename;
}

const std::string& groupDisplayName(const UiFileState& file, size_t groupIndex)
{
    const auto& custom = file.groupSettings.at(groupIndex).customName;
    return custom.empty() ? meshNodeDisplayName(meshGeometry(file.mesh).nodes.at(groupIndex)) : custom;
}

const std::string& sceneNodeDisplayName(const UiState& state, const UiSceneNode& node)
{
    if (node.kind == UiSceneNodeKind::folder) { return node.customName.empty() ? node.name : node.customName; }
    if (node.fileIndex < state.files.size()) {
        const auto& file = state.files[node.fileIndex];
        if (node.kind == UiSceneNodeKind::file && !file.customName.empty()) { return file.customName; }
        if (node.kind == UiSceneNodeKind::group && node.groupIndex < file.groupSettings.size()
            && node.groupIndex < meshGeometry(file.mesh).nodes.size()) { return groupDisplayName(file, node.groupIndex); }
    }
    return node.name;
}

namespace {

SceneObjectId allocateObjectId(UiState& state)
{
    if (state.nextObjectId == invalidSceneObjectId) {
        throw std::overflow_error("Scene object IDs exhausted.");
    }
    return state.nextObjectId++;
}

void assignNodeIds(UiState& state, UiSceneNode& node)
{
    if (node.kind == UiSceneNodeKind::folder) {
        if (node.objectId == invalidSceneObjectId) {
            node.objectId = allocateObjectId(state);
        }
    } else if (node.fileIndex < state.files.size()) {
        const auto& file = state.files[node.fileIndex];
        if (node.kind == UiSceneNodeKind::file) {
            node.objectId = file.objectId;
        } else if (node.groupIndex < file.groupSettings.size()) {
            node.objectId = file.groupSettings[node.groupIndex].objectId;
        }
    }
    for (auto& child : node.children) {
        assignNodeIds(state, child);
    }
}

template <typename Visitor>
bool visitFolders(const std::vector<UiSceneNode>& nodes, const Visitor& visitor, SceneObjectId filter)
{
    for (const auto& node : nodes) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId != invalidSceneObjectId
            && (filter == invalidSceneObjectId || node.objectId == filter)
            && visitor(SceneObjectInfo{node.objectId, SceneObjectKind::folder,
                node.customName.empty() ? node.name : node.customName, {}, invalidSceneObjectId, node.name})) {
            return true;
        }
        if (visitFolders(node.children, visitor, filter)) {
            return true;
        }
    }
    return false;
}

template <typename Visitor>
void visitObjects(const UiState& state, const Visitor& visitor, SceneObjectId filter = invalidSceneObjectId)
{
    // Filter IDs before constructing owning names/paths for inspector queries.
    const auto wanted = [filter](SceneObjectId id) {
        return id != invalidSceneObjectId && (filter == invalidSceneObjectId || filter == id);
    };
    for (const auto& item : state.annotations) {
        if (wanted(item.objectId) && visitor(SceneObjectInfo{item.objectId, SceneObjectKind::annotation, item.settings.name, {}, 0})) { return; }
    }
    for (const auto& comparison : state.comparisons) {
        if (wanted(comparison.objectId)
            && visitor(SceneObjectInfo{comparison.objectId, SceneObjectKind::comparison,
                comparison.name, {}, invalidSceneObjectId})) { return; }
    }
    if (visitFolders(state.sceneNodes, visitor, filter)) {
        return;
    }
    for (const auto& file : state.files) {
        if (wanted(file.objectId)) {
            const auto filename = pathToUtf8(file.path.filename());
            if (visitor(SceneObjectInfo{file.objectId, SceneObjectKind::file,
                    modelDisplayName(file), file.path, invalidSceneObjectId,
                    filename.empty() ? pathToUtf8(file.path) : filename})) { return; }
        }
        for (size_t index = 0; index < file.groupSettings.size() && index < meshGeometry(file.mesh).nodes.size(); ++index) {
            const auto id = file.groupSettings[index].objectId;
            if (wanted(id)
                && visitor(SceneObjectInfo{id, SceneObjectKind::group, groupDisplayName(file, index), {}, file.objectId,
                    meshNodeDisplayName(meshGeometry(file.mesh).nodes[index])})) {
                return;
            }
        }
    }
}

} // namespace

void assignSceneObjectIds(UiState& state)
{
    for (auto& item : state.annotations) { if (item.objectId == 0) { item.objectId = allocateObjectId(state); } }
    for (auto& comparison : state.comparisons) {
        if (comparison.objectId == invalidSceneObjectId) {
            comparison.objectId = allocateObjectId(state);
        }
    }
    for (auto& file : state.files) {
        if (file.objectId == invalidSceneObjectId) {
            file.objectId = allocateObjectId(state);
        }
        for (auto& group : file.groupSettings) {
            if (group.objectId == invalidSceneObjectId) {
                group.objectId = allocateObjectId(state);
            }
        }
    }
    for (auto& node : state.sceneNodes) {
        assignNodeIds(state, node);
    }
}

std::vector<SceneObjectInfo> sceneObjects(const UiState& state)
{
    std::vector<SceneObjectInfo> result;
    visitObjects(state, [&](const SceneObjectInfo& object) {
        result.push_back(object);
        return false;
    });
    return result;
}

std::optional<SceneObjectInfo> findSceneObject(const UiState& state, SceneObjectId id)
{
    std::optional<SceneObjectInfo> result;
    if (id != invalidSceneObjectId) {
        visitObjects(state, [&](const SceneObjectInfo& object) {
            result = object;
            return true;
        }, id);
    }
    return result;
}

} // namespace woby
