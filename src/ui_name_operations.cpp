#include "ui_operations.h"
#include <boost/unordered/unordered_flat_map.hpp>

namespace woby {
namespace {
std::string* customName(UiState& state, SceneObjectId id)
{
    if (!id) { return nullptr; }
    for (auto& file : state.files) {
        if (file.objectId == id) { return &file.customName; }
        for (auto& group : file.groupSettings) {
            if (group.objectId == id) { return &group.customName; }
        }
    }
    const auto folder = [&](auto&& self, std::vector<UiSceneNode>& nodes) -> std::string* {
        for (auto& node : nodes) {
            if (node.kind == UiSceneNodeKind::folder && node.objectId == id) { return &node.customName; }
            if (auto* found = self(self, node.children)) { return found; }
        }
        return nullptr;
    };
    return folder(folder, state.sceneNodes);
}

void refreshReferenceNames(UiState& state)
{
    if (state.comparisons.empty() && state.annotations.empty()) { return; }
    const auto objects = sceneObjects(state);
    boost::unordered_flat_map<SceneObjectId, const SceneObjectInfo*> byId;
    byId.reserve(objects.size());
    for (const auto& object : objects) { byId.emplace(object.id, &object); }
    const auto refresh = [&](SceneObjectId id, std::string& name) {
        const auto object = byId.find(id);
        if (object == byId.end()) { return; } // Keep the last known label for missing sources.
        name = object->second->name;
        if (const auto file = byId.find(object->second->fileId); file != byId.end()) {
            name = file->second->name + " / " + name;
        }
    };
    for (auto& comparison : state.comparisons) {
        for (auto& part : comparison.a) { refresh(part.objectId, part.name); }
        for (auto& part : comparison.b) { refresh(part.objectId, part.name); }
    }
    for (auto& annotation : state.annotations) {
        if (!annotation.section) { refresh(annotation.targetId, annotation.targetName); }
    }
}

bool setName(UiState& state, SceneObjectId id, std::string name)
{
    auto* destination = customName(state, id);
    if (!destination || *destination == name) { return false; }
    *destination = std::move(name);
    refreshReferenceNames(state);
    markSceneDirty(state, SceneChange::labels);
    return true;
}
} // namespace

bool renameSceneObject(UiState& state, SceneObjectId id, std::string_view name)
{
    const auto object = findSceneObject(state, id);
    if (!object || !object->originalName) { return false; }
    // Exact source text can contain whitespace that is not allowed in a custom label.
    if (name == *object->originalName) { return restoreSceneObjectName(state, id); }
    if (normalizedSceneName(name).empty()) { return false; }
    return setName(state, id, sceneCustomName(name, *object->originalName));
}

bool restoreSceneObjectName(UiState& state, SceneObjectId id)
{
    return setName(state, id, {});
}
} // namespace woby
