#include "scene_objects.h"
#include "ui_state.h"

#include <stdexcept>

namespace woby {
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
bool visitFolders(const std::vector<UiSceneNode>& nodes, const Visitor& visitor)
{
    for (const auto& node : nodes) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId != invalidSceneObjectId
            && visitor(SceneObjectInfo{node.objectId, SceneObjectKind::folder, node.name, {}, invalidSceneObjectId})) {
            return true;
        }
        if (visitFolders(node.children, visitor)) {
            return true;
        }
    }
    return false;
}

template <typename Visitor>
void visitObjects(const UiState& state, const Visitor& visitor)
{
    for (const auto& item : state.annotations) {
        if (item.objectId != 0 && visitor(SceneObjectInfo{item.objectId, SceneObjectKind::annotation, item.settings.name, {}, 0})) { return; }
    }
    for (const auto& comparison : state.comparisons) {
        if (comparison.objectId != invalidSceneObjectId
            && visitor(SceneObjectInfo{comparison.objectId, SceneObjectKind::comparison,
                comparison.name, {}, invalidSceneObjectId})) { return; }
    }
    if (visitFolders(state.sceneNodes, visitor)) {
        return;
    }
    for (const auto& file : state.files) {
        const auto filename = file.path.filename().u8string();
        if (file.objectId != invalidSceneObjectId
            && visitor(SceneObjectInfo{file.objectId, SceneObjectKind::file, std::string(filename.begin(), filename.end()), file.path, invalidSceneObjectId})) {
            return;
        }
        for (size_t index = 0; index < file.groupSettings.size() && index < file.mesh.nodes.size(); ++index) {
            const auto id = file.groupSettings[index].objectId;
            if (id != invalidSceneObjectId
                && visitor(SceneObjectInfo{id, SceneObjectKind::group, file.mesh.nodes[index].name, {}, file.objectId})) {
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
            if (object.id != id) {
                return false;
            }
            result = object;
            return true;
        });
    }
    return result;
}

} // namespace woby
