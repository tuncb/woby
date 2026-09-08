#include "ui_operations.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <unordered_set>

namespace woby {
namespace {

template <typename Nodes>
auto* findFolderNode(Nodes& nodes, SceneObjectId id)
{
    using Node = std::remove_reference_t<decltype(nodes.front())>;
    for (auto& node : nodes) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId == id) { return &node; }
        if (auto* found = findFolderNode(node.children, id)) { return found; }
    }
    return static_cast<Node*>(nullptr);
}

template <typename Nodes, typename Visitor>
bool visitFolderSettings(Nodes& nodes, SceneObjectId id, const Visitor& visitor)
{
    for (auto& node : nodes) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId == id) {
            visitor(node.settings);
            return true;
        }
        if (visitFolderSettings(node.children, id, visitor)) { return true; }
    }
    return false;
}

template <typename State, typename Visitor>
bool visitObjectSettings(State& state, SceneObjectId id, const Visitor& visitor)
{
    if (id == invalidSceneObjectId) { return false; }
    for (auto& file : state.files) {
        if (file.objectId == id) { visitor(file.fileSettings); return true; }
        for (auto& part : file.groupSettings) {
            if (part.objectId == id) { visitor(part); return true; }
        }
    }
    return visitFolderSettings(state.sceneNodes, id, visitor);
}

template <typename Settings>
std::optional<float> propertyValue(const Settings& settings, UiObjectProperty property)
{
    using P = UiObjectProperty;
    switch (property) {
    case P::translationX: return settings.translation[0];
    case P::translationY: return settings.translation[1];
    case P::translationZ: return settings.translation[2];
    case P::rotationX: return settings.rotationDegrees[0];
    case P::rotationY: return settings.rotationDegrees[1];
    case P::rotationZ: return settings.rotationDegrees[2];
    case P::scale: return settings.scale;
    case P::opacity: return settings.opacity;
    case P::vertexSize: case P::solidMesh: case P::triangles: case P::vertices:
    case P::red: case P::green: case P::blue: break;
    }
    if constexpr (std::is_same_v<Settings, UiGroupState>) {
        switch (property) {
        case P::vertexSize: return settings.vertexSizeScale;
        case P::solidMesh: return settings.showSolidMesh ? 1.0f : 0.0f;
        case P::triangles: return settings.showTriangles ? 1.0f : 0.0f;
        case P::vertices: return settings.showVertices ? 1.0f : 0.0f;
        case P::red: return settings.color[0];
        case P::green: return settings.color[1];
        case P::blue: return settings.color[2];
        case P::translationX: case P::translationY: case P::translationZ:
        case P::rotationX: case P::rotationY: case P::rotationZ:
        case P::scale: case P::opacity: break;
        }
    }
    return std::nullopt;
}

bool isPartAppearanceProperty(UiObjectProperty property)
{
    using P = UiObjectProperty;
    return property == P::solidMesh || property == P::triangles || property == P::vertices
        || property == P::red || property == P::green || property == P::blue;
}

void appendAppearanceTargets(const UiState& state, const UiSceneNode& node, bool parts,
    std::vector<SceneObjectId>& targets)
{
    if (node.kind == UiSceneNodeKind::folder) {
        for (const auto& child : node.children) { appendAppearanceTargets(state, child, parts, targets); }
    } else if (node.fileIndex < state.files.size()) {
        const auto& file = state.files[node.fileIndex];
        if (node.kind == UiSceneNodeKind::group) {
            if (node.groupIndex < file.groupSettings.size()) { targets.push_back(file.groupSettings[node.groupIndex].objectId); }
        } else if (parts) {
            for (const auto& part : file.groupSettings) { targets.push_back(part.objectId); }
        } else {
            targets.push_back(file.objectId);
        }
    }
}

std::vector<SceneObjectId> propertyTargets(const UiState& state, UiObjectProperty property)
{
    std::vector<SceneObjectId> targets;
    const bool parts = isPartAppearanceProperty(property);
    for (const auto id : state.selectedSceneObjects) {
        const auto object = findSceneObject(state, id);
        if (!object || object->kind == SceneObjectKind::comparison) { return {}; }
        if (object->kind == SceneObjectKind::folder && (parts || property == UiObjectProperty::vertexSize)) {
            if (const auto* folder = findFolderNode(state.sceneNodes, id)) {
                appendAppearanceTargets(state, *folder, parts, targets);
            }
        } else if (object->kind == SceneObjectKind::file && parts) {
            for (const auto& file : state.files) {
                if (file.objectId == id) {
                    for (const auto& part : file.groupSettings) { targets.push_back(part.objectId); }
                    break;
                }
            }
        } else {
            targets.push_back(id);
        }
    }
    // Preserve the first target's displayed value while removing overlapping
    // parent/child selections and repeated tree occurrences.
    std::unordered_set<SceneObjectId> seen;
    std::erase_if(targets, [&](SceneObjectId id) { return !seen.insert(id).second; });
    return targets;
}

std::optional<float> objectProperty(const UiState& state, SceneObjectId id, UiObjectProperty property)
{
    std::optional<float> result;
    visitObjectSettings(state, id, [&](const auto& settings) { result = propertyValue(settings, property); });
    if (property == UiObjectProperty::vertexSize) {
        for (const auto& file : state.files) {
            if (id != invalidSceneObjectId && file.objectId == id) { return file.vertexSizeScale; }
        }
    }
    return result;
}

template <typename Settings>
void setProperty(Settings& settings, UiObjectProperty property, float value)
{
    using P = UiObjectProperty;
    switch (property) {
    case P::translationX: settings.translation[0] = value; return;
    case P::translationY: settings.translation[1] = value; return;
    case P::translationZ: settings.translation[2] = value; return;
    case P::rotationX: settings.rotationDegrees[0] = std::clamp(value, minRotationDegrees, maxRotationDegrees); return;
    case P::rotationY: settings.rotationDegrees[1] = std::clamp(value, minRotationDegrees, maxRotationDegrees); return;
    case P::rotationZ: settings.rotationDegrees[2] = std::clamp(value, minRotationDegrees, maxRotationDegrees); return;
    case P::scale: settings.scale = std::clamp(value, minGroupScale, maxGroupScale); return;
    case P::opacity: settings.opacity = std::clamp(value, minGroupOpacity, maxGroupOpacity); return;
    case P::vertexSize: case P::solidMesh: case P::triangles: case P::vertices:
    case P::red: case P::green: case P::blue: break;
    }
    if constexpr (std::is_same_v<Settings, UiGroupState>) {
        switch (property) {
        case P::vertexSize: setGroupVertexSizeScale(settings, value); return;
        case P::solidMesh: setGroupRenderMode(settings, UiRenderMode::solidMesh, value != 0.0f); return;
        case P::triangles: setGroupRenderMode(settings, UiRenderMode::triangles, value != 0.0f); return;
        case P::vertices: setGroupRenderMode(settings, UiRenderMode::vertices, value != 0.0f); return;
        case P::red: settings.color[0] = std::clamp(value, 0.0f, 1.0f); return;
        case P::green: settings.color[1] = std::clamp(value, 0.0f, 1.0f); return;
        case P::blue: settings.color[2] = std::clamp(value, 0.0f, 1.0f); return;
        case P::translationX: case P::translationY: case P::translationZ:
        case P::rotationX: case P::rotationY: case P::rotationZ:
        case P::scale: case P::opacity: break;
        }
    }
}

} // namespace

UiPropertyValue selectedObjectProperty(const UiState& state, UiObjectProperty property)
{
    UiPropertyValue result;
    for (const auto id : propertyTargets(state, property)) {
        const auto value = objectProperty(state, id, property);
        if (!value) { return {}; }
        if (result.available) { result.mixed = result.mixed || result.value != *value; }
        else { result.value = *value; result.available = true; }
    }
    return result;
}

void setSelectedObjectProperty(UiState& state, UiObjectProperty property, float value)
{
    if (!std::isfinite(value) || !selectedObjectProperty(state, property).available) { return; }
    bool changed = false;
    for (const auto id : propertyTargets(state, property)) {
        const auto before = objectProperty(state, id, property);
        visitObjectSettings(state, id, [&](auto& settings) { setProperty(settings, property, value); });
        if (property == UiObjectProperty::vertexSize) {
            for (auto& file : state.files) {
                if (file.objectId == id) { setFileVertexSizeScale(file, value); }
            }
        }
        changed = changed || before != objectProperty(state, id, property);
    }
    if (changed) { recalculateSceneBounds(state); markSceneDirty(state); }
}

UiPropertyValue selectedObjectVisibility(const UiState& state)
{
    UiPropertyValue result;
    const auto include = [&](size_t visible, size_t total) {
        const float value = visible > 0u ? 1.0f : 0.0f;
        result.mixed = result.mixed || (visible > 0u && visible < total)
            || (result.available && result.value != value);
        if (!result.available) { result.value = value; }
        result.available = true;
    };
    for (const auto id : state.selectedSceneObjects) {
        const auto object = findSceneObject(state, id);
        if (!object || object->kind == SceneObjectKind::comparison) { return {}; }
        if (const auto* folder = findFolderNode(state.sceneNodes, id)) {
            include(countVisibleSceneNodeGroups(state, *folder), countSceneNodeGroups(state, *folder));
            continue;
        }
        for (const auto& file : state.files) {
            if (file.objectId == id) {
                include(countVisibleFileGroups(file), file.groupSettings.size());
                break;
            }
            for (const auto& part : file.groupSettings) {
                if (part.objectId == id) { include(part.visible ? 1u : 0u, 1u); }
            }
        }
    }
    return result;
}

void setSelectedObjectsVisible(UiState& state, bool visible)
{
    if (!selectedObjectVisibility(state).available) { return; }
    const auto before = createSceneDocument(state);
    for (const auto id : state.selectedSceneObjects) {
        if (auto* folder = findFolderNode(state.sceneNodes, id)) {
            setSceneNodeSubtreeVisible(state, *folder, visible);
            continue;
        }
        for (auto& file : state.files) {
            if (file.objectId == id) {
                setFileVisible(file, visible);
                break;
            }
            for (auto& part : file.groupSettings) {
                if (part.objectId == id) { setGroupVisible(file, part, visible); }
            }
        }
    }
    refreshSceneTreeFolderVisibility(state);
    if (before != createSceneDocument(state)) { recalculateSceneBounds(state); markSceneDirty(state); }
}

void resetSelectedObjectProperties(UiState& state, UiPropertyGroup group)
{
    // An unsupported/missing target disables the complete reset, as in the UI.
    if (!selectedObjectProperty(state, UiObjectProperty::opacity).available) { return; }
    const auto before = createSceneDocument(state);
    for (const auto id : state.selectedSceneObjects) {
        visitObjectSettings(state, id, [&](auto& settings) {
            if (group == UiPropertyGroup::translation || group == UiPropertyGroup::transform) { settings.translation = {}; }
            if (group == UiPropertyGroup::rotation || group == UiPropertyGroup::transform) { settings.rotationDegrees = {}; }
            if (group == UiPropertyGroup::scale || group == UiPropertyGroup::transform) { settings.scale = 1.0f; }
            if (group == UiPropertyGroup::appearance) { settings.opacity = 1.0f; }
        });
    }
    if (group == UiPropertyGroup::appearance) {
        const auto partTargets = propertyTargets(state, UiObjectProperty::red);
        const auto vertexTargets = propertyTargets(state, UiObjectProperty::vertexSize);
        const std::unordered_set<SceneObjectId> parts(partTargets.begin(), partTargets.end());
        const std::unordered_set<SceneObjectId> vertices(vertexTargets.begin(), vertexTargets.end());
        size_t colorIndex = 0;
        for (auto& file : state.files) {
            if (vertices.contains(file.objectId)) { setFileVertexSizeScale(file, 1.0f); }
            for (auto& part : file.groupSettings) {
                if (parts.contains(part.objectId)) {
                    resetGroupColor(part, colorIndex);
                    setGroupVertexSizeScale(part, 1.0f);
                    setGroupRenderMode(part, UiRenderMode::solidMesh, true);
                    setGroupRenderMode(part, UiRenderMode::triangles, false);
                    setGroupRenderMode(part, UiRenderMode::vertices, false);
                }
                ++colorIndex;
            }
        }
    }
    if (before != createSceneDocument(state)) { recalculateSceneBounds(state); markSceneDirty(state); }
}

} // namespace woby
