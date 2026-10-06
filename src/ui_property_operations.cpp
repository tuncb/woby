#include "ui_operations.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace woby {
namespace {

bool isUvProperty(UiObjectProperty property)
{
    return property == UiObjectProperty::uvGrid || property == UiObjectProperty::uvDensityU
        || property == UiObjectProperty::uvDensityV || property == UiObjectProperty::uvColorMode
        || property == UiObjectProperty::uvMinimum || property == UiObjectProperty::uvMaximum;
}

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
    case P::red: case P::green: case P::blue:
    case P::uvGrid: case P::uvDensityU: case P::uvDensityV: case P::uvColorMode: case P::uvMinimum: case P::uvMaximum:
    case P::lineWidth: case P::lineDepthTest: case P::analysisMode: break;
    }
    if constexpr (std::is_same_v<Settings, UiGroupState>) {
        switch (property) {
        case P::lineWidth: return settings.lines.width;
        case P::lineDepthTest: return settings.lines.depthTest ? 1.0f : 0.0f;
        case P::uvGrid: return settings.uvGrid.enabled ? 1.0f : 0.0f;
        case P::uvDensityU: return settings.uvGrid.densityU;
        case P::uvDensityV: return settings.uvGrid.densityV;
        case P::uvColorMode: return static_cast<float>(settings.uvGrid.mode);
        case P::uvMinimum: return settings.uvGrid.minimum;
        case P::uvMaximum: return settings.uvGrid.maximum;
        case P::vertexSize: return settings.vertexSizeScale;
        case P::solidMesh: return settings.showSolidMesh ? 1.0f : 0.0f;
        case P::triangles: return settings.showTriangles ? 1.0f : 0.0f;
        case P::vertices: return settings.showVertices ? 1.0f : 0.0f;
        case P::red: return settings.color[0];
        case P::green: return settings.color[1];
        case P::blue: return settings.color[2];
        case P::translationX: case P::translationY: case P::translationZ:
        case P::rotationX: case P::rotationY: case P::rotationZ:
        case P::scale: case P::opacity: case P::analysisMode: break;
        }
    }
    if constexpr (std::is_same_v<Settings, UiFileSettings>) {
        if (property == P::analysisMode) { return static_cast<float>(settings.analysisMode); }
    }
    return std::nullopt;
}

bool isPartAppearanceProperty(UiObjectProperty property)
{
    using P = UiObjectProperty;
    return property == P::solidMesh || property == P::triangles || property == P::vertices
        || property == P::red || property == P::green || property == P::blue || isUvProperty(property)
        || property == P::lineWidth || property == P::lineDepthTest;
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

struct PropertyTargetContext {
    std::vector<SceneObjectInfo> objects;
    boost::unordered_flat_map<SceneObjectId, const SceneObjectInfo*> byId;
    std::array<boost::unordered_flat_set<SceneObjectId>, 3> eligible;
};
PropertyTargetContext propertyTargetContext(const UiState& state)
{
    PropertyTargetContext context;
    context.objects = sceneObjects(state);
    context.byId.reserve(context.objects.size());
    for (const auto& object : context.objects) { context.byId.emplace(object.id, &object); }
    for (const auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
            const auto id = file.groupSettings[i].objectId;
            const auto& node = file.mesh.nodes[i];
            if (node.hasTexcoords) { context.eligible[0].insert(id); }
            if (node.lineIndexCount) { context.eligible[1].insert(id); }
            if (!node.lineIndexCount && !node.pointIndexCount) { context.eligible[2].insert(id); }
        }
    }
    return context;
}

std::vector<SceneObjectId> propertyTargets(const UiState& state, UiObjectProperty property,
    const std::vector<SceneObjectId>* objectsOverride = nullptr, const PropertyTargetContext* context = nullptr)
{
    const auto& selection = objectsOverride ? *objectsOverride : state.selectedSceneObjects;
    std::vector<SceneObjectId> targets;
    const bool parts = isPartAppearanceProperty(property);
    // A large selection must not rescan every scene object for each selected ID.
    // Small selections retain direct lookup without building a scene index.
    const auto objects = !context && selection.size() > 8 ? sceneObjects(state) : std::vector<SceneObjectInfo>{};
    boost::unordered_flat_map<SceneObjectId, const SceneObjectInfo*> byId;
    byId.reserve(objects.size());
    for (const auto& object : objects) { byId.emplace(object.id, &object); }
    for (const auto id : selection) {
        std::optional<SceneObjectInfo> single;
        const SceneObjectInfo* object = nullptr;
        if (context || selection.size() > 8) {
            const auto& lookup = context ? context->byId : byId;
            if (const auto found = lookup.find(id); found != lookup.end()) { object = found->second; }
        } else {
            single = findSceneObject(state, id);
            if (single) { object = &*single; }
        }
        if (!object || object->kind == SceneObjectKind::comparison || object->kind == SceneObjectKind::annotation) { return {}; }
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
    boost::unordered_flat_set<SceneObjectId> seen;
    seen.reserve(targets.size());
    std::erase_if(targets, [&](SceneObjectId id) { return !seen.insert(id).second; });
    const bool lines = property == UiObjectProperty::lineWidth || property == UiObjectProperty::lineDepthTest;
    const bool triangles = property == UiObjectProperty::solidMesh || property == UiObjectProperty::triangles;
    if (context && (isUvProperty(property) || lines || triangles)) {
        const auto& eligible = context->eligible[isUvProperty(property) ? 0 : lines ? 1 : 2];
        std::erase_if(targets, [&](SceneObjectId id) { return !eligible.contains(id); });
    } else if (isUvProperty(property) || lines || triangles) {
        boost::unordered_flat_set<SceneObjectId> eligible;
        for (const auto& file : state.files) {
            for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
                if (isUvProperty(property) ? file.mesh.nodes[i].hasTexcoords
                    : (lines ? file.mesh.nodes[i].lineIndexCount != 0
                        : file.mesh.nodes[i].lineIndexCount == 0 && file.mesh.nodes[i].pointIndexCount == 0)) { eligible.insert(file.groupSettings[i].objectId); }
            }
        }
        std::erase_if(targets, [&](SceneObjectId id) { return !eligible.contains(id); });
    }
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
    case P::red: case P::green: case P::blue:
    case P::uvGrid: case P::uvDensityU: case P::uvDensityV: case P::uvColorMode: case P::uvMinimum: case P::uvMaximum:
    case P::lineWidth: case P::lineDepthTest: case P::analysisMode: break;
    }
    if constexpr (std::is_same_v<Settings, UiGroupState>) {
        switch (property) {
        case P::lineWidth: setGroupLineStyle(settings, {value, settings.lines.depthTest}); return;
        case P::lineDepthTest: setGroupLineStyle(settings, {settings.lines.width, value != 0.0f}); return;
        case P::uvGrid: case P::uvDensityU: case P::uvDensityV: case P::uvColorMode: case P::uvMinimum: case P::uvMaximum: {
            auto uv = settings.uvGrid;
            if (property == P::uvGrid) { uv.enabled = value != 0; }
            if (property == P::uvDensityU) { uv.densityU = value; }
            if (property == P::uvDensityV) { uv.densityV = value; }
            if (property == P::uvColorMode) { uv.mode = value == 1 ? UvColorMode::u : value == 2 ? UvColorMode::v : UvColorMode::grid; }
            if (property == P::uvMinimum) { uv.minimum = value; }
            if (property == P::uvMaximum) { uv.maximum = value; }
            setGroupUvGrid(settings, uv); return;
        }
        case P::vertexSize: setGroupVertexSizeScale(settings, value); return;
        case P::solidMesh: setGroupRenderMode(settings, UiRenderMode::solidMesh, value != 0.0f); return;
        case P::triangles: setGroupRenderMode(settings, UiRenderMode::triangles, value != 0.0f); return;
        case P::vertices: setGroupRenderMode(settings, UiRenderMode::vertices, value != 0.0f); return;
        case P::red: settings.color[0] = std::clamp(value, 0.0f, 1.0f); return;
        case P::green: settings.color[1] = std::clamp(value, 0.0f, 1.0f); return;
        case P::blue: settings.color[2] = std::clamp(value, 0.0f, 1.0f); return;
        case P::translationX: case P::translationY: case P::translationZ:
        case P::rotationX: case P::rotationY: case P::rotationZ:
        case P::scale: case P::opacity: case P::analysisMode: break;
        }
    }
    if constexpr (std::is_same_v<Settings, UiFileSettings>) {
        if (property == P::analysisMode) { settings.analysisMode = value >= 2.0f ? AnalysisMode::whole : AnalysisMode::perVolume; }
    }
}

} // namespace

void setGroupUvGrid(UiGroupState& group, UvGridSettings settings)
{
    group.uvGrid = normalizedUvGrid(settings);
}

void setGroupLineStyle(UiGroupState& group, LineStyle settings)
{
    group.lines = normalizedLineStyle(settings);
}

bool setObjectColor(UiState& state, const std::vector<SceneObjectId>& objects,
    std::optional<std::array<float, 3>> color)
{
    if (color && std::any_of(color->begin(), color->end(), [](float value) { return !std::isfinite(value); })) { return false; }
    const auto targets = propertyTargets(state, UiObjectProperty::red, &objects);
    if (targets.empty()) { return false; }
    const boost::unordered_flat_set<SceneObjectId> included(targets.begin(), targets.end());
    bool changed = false;
    size_t colorIndex = 0;
    for (auto& file : state.files) {
        for (auto& part : file.groupSettings) {
            if (included.contains(part.objectId)) {
                const auto before = part.color;
                if (color) { setGroupColor(part, {(*color)[0], (*color)[1], (*color)[2], part.color[3]}); }
                else { resetGroupColor(part, colorIndex); }
                changed = changed || part.color != before;
            }
            ++colorIndex;
        }
    }
    if (changed) { markSceneDirty(state, SceneChange::appearance); }
    return true;
}

bool setObjectLineStyle(UiState& state, const std::vector<SceneObjectId>& objects,
    std::optional<float> width, std::optional<bool> depthTest)
{
    if (width && !std::isfinite(*width)) { return false; }
    const auto targets = propertyTargets(state, UiObjectProperty::lineWidth, &objects);
    const boost::unordered_flat_set<SceneObjectId> included(targets.begin(), targets.end());
    bool available = false, changed = false;
    for (auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
            auto& part = file.groupSettings[i];
            if (!file.mesh.nodes[i].lineIndexCount || (!objects.empty() && !included.contains(part.objectId))) { continue; }
            available = true;
            auto settings = part.lines;
            if (width) { settings.width = *width; }
            if (depthTest) { settings.depthTest = *depthTest; }
            settings = normalizedLineStyle(settings);
            changed = changed || settings != part.lines;
            setGroupLineStyle(part, settings);
        }
    }
    if (changed) { markSceneDirty(state, SceneChange::appearance | SceneChange::picking); }
    return available;
}

bool setObjectUvGrid(UiState& state, const std::vector<SceneObjectId>& objects,
    std::optional<bool> enabled, std::optional<float> densityU, std::optional<float> densityV,
    std::optional<UvColorMode> mode, std::optional<float> minimum, std::optional<float> maximum)
{
    if ((minimum && !std::isfinite(*minimum)) || (maximum && !std::isfinite(*maximum))
        || (densityU && !std::isfinite(*densityU)) || (densityV && !std::isfinite(*densityV))) { return false; }
    const auto targets = comparisonObjectParts(state, objects);
    const boost::unordered_flat_set<SceneObjectId> included(targets.begin(), targets.end());
    // Validate every eligible child before applying a multi-object edit.
    for (const auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
            const auto& part = file.groupSettings[i];
            if (!file.mesh.nodes[i].hasTexcoords || (!objects.empty() && !included.contains(part.objectId))) { continue; }
            if (maximum.value_or(part.uvGrid.maximum) <= minimum.value_or(part.uvGrid.minimum)) { return false; }
        }
    }
    bool available = false, changed = false;
    for (auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
            auto& part = file.groupSettings[i];
            if (!file.mesh.nodes[i].hasTexcoords || (!objects.empty() && !included.contains(part.objectId))) { continue; }
            available = true;
            auto settings = part.uvGrid;
            if (enabled) { settings.enabled = *enabled; }
            if (densityU) { settings.densityU = *densityU; }
            if (densityV) { settings.densityV = *densityV; }
            if (mode) { settings.mode = *mode; }
            if (minimum) { settings.minimum = *minimum; }
            if (maximum) { settings.maximum = *maximum; }
            settings = normalizedUvGrid(settings);
            changed = changed || settings != part.uvGrid;
            setGroupUvGrid(part, settings);
        }
    }
    if (changed) { markSceneDirty(state, SceneChange::appearance); }
    return available;
}

UiPropertyValue selectedObjectProperty(const UiState& state, UiObjectProperty property)
{
    UiPropertyValue result;
    const auto targets = propertyTargets(state, property);
    std::vector<std::optional<float>> values;
    if (targets.size() > 8) {
        // Resolve a whole selection in one traversal. Preserve target order for
        // the displayed value, including overlapping parent/child selections.
        boost::unordered_flat_map<SceneObjectId, size_t> locations;
        locations.reserve(targets.size());
        for (size_t i = 0; i < targets.size(); ++i) { locations.emplace(targets[i], i); }
        values.resize(targets.size());
        const auto include = [&](SceneObjectId id, std::optional<float> value) {
            if (id == invalidSceneObjectId) { return; }
            if (const auto found = locations.find(id); found != locations.end() && !values[found->second]) {
                values[found->second] = value;
            }
        };
        for (const auto& file : state.files) {
            include(file.objectId, property == UiObjectProperty::vertexSize
                ? std::optional<float>{file.vertexSizeScale} : propertyValue(file.fileSettings, property));
            for (const auto& part : file.groupSettings) { include(part.objectId, propertyValue(part, property)); }
        }
        const auto folders = [&](auto&& self, const std::vector<UiSceneNode>& nodes) -> void {
            for (const auto& node : nodes) {
                if (node.kind == UiSceneNodeKind::folder) { include(node.objectId, propertyValue(node.settings, property)); }
                self(self, node.children);
            }
        };
        folders(folders, state.sceneNodes);
    }
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto value = values.empty() ? objectProperty(state, targets[i], property) : values[i];
        if (!value) { return {}; }
        if (result.available) { result.mixed = result.mixed || result.value != *value; }
        else { result.value = *value; result.available = true; }
    }
    return result;
}

std::array<UiPropertyValue, uiObjectPropertyCount> selectedObjectProperties(const UiState& state)
{
    using P = UiObjectProperty;
    const auto context = propertyTargetContext(state);
    const auto family = [](P property) -> size_t {
        if (property == P::vertexSize) { return 1; }
        if (isUvProperty(property)) { return 3; }
        if (property == P::lineWidth || property == P::lineDepthTest) { return 4; }
        if (property == P::solidMesh || property == P::triangles) { return 5; }
        return isPartAppearanceProperty(property) ? 2 : 0;
    };
    constexpr std::array representatives{P::translationX, P::vertexSize, P::red, P::uvGrid, P::lineWidth, P::solidMesh};
    std::array<std::vector<SceneObjectId>, representatives.size()> targets;
    using Values = std::array<std::optional<float>, uiObjectPropertyCount>;
    boost::unordered_flat_map<SceneObjectId, Values> values;
    for (size_t i = 0; i < targets.size(); ++i) {
        targets[i] = propertyTargets(state, representatives[i], nullptr, &context);
        for (const auto id : targets[i]) { values.try_emplace(id); }
    }
    const auto include = [&](SceneObjectId id, const auto& settings, std::optional<float> vertexSize = {}) {
        if (const auto it = values.find(id); it != values.end()) {
            for (size_t p = 0; p < uiObjectPropertyCount; ++p) {
                it->second[p] = propertyValue(settings, static_cast<P>(p));
            }
            if (vertexSize) { it->second[static_cast<size_t>(P::vertexSize)] = vertexSize; }
        }
    };
    for (const auto& file : state.files) {
        include(file.objectId, file.fileSettings, file.vertexSizeScale);
        for (const auto& part : file.groupSettings) { include(part.objectId, part); }
    }
    const auto folders = [&](auto&& self, const std::vector<UiSceneNode>& nodes) -> void {
        for (const auto& node : nodes) {
            if (node.kind == UiSceneNodeKind::folder) { include(node.objectId, node.settings); }
            self(self, node.children);
        }
    };
    folders(folders, state.sceneNodes);
    std::array<UiPropertyValue, uiObjectPropertyCount> result{};
    for (size_t p = 0; p < result.size(); ++p) {
        auto& value = result[p];
        for (const auto id : targets[family(static_cast<P>(p))]) {
            const auto item = values.at(id)[p];
            if (!item) { value = {}; break; }
            if (value.available) { value.mixed = value.mixed || value.value != *item; }
            else { value.value = *item; value.available = true; }
        }
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
    if (changed) {
        const bool transform = property <= UiObjectProperty::scale;
        const bool visibility = property == UiObjectProperty::opacity || property == UiObjectProperty::solidMesh
            || property == UiObjectProperty::triangles || property == UiObjectProperty::vertices;
        if (transform || visibility) { recalculateSceneBounds(state); }
        markSceneDirty(state, property == UiObjectProperty::analysisMode ? SceneChange::analysis : transform ? SceneChange::geometry : visibility
            ? SceneChange::appearance | SceneChange::visibility
            : (property == UiObjectProperty::vertexSize || property == UiObjectProperty::lineWidth || property == UiObjectProperty::lineDepthTest)
                ? SceneChange::appearance | SceneChange::picking : SceneChange::appearance);
    }
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
    struct Counts { size_t visible = 0, total = 0; };
    boost::unordered_flat_map<SceneObjectId, Counts> counts;
    for (const auto& file : state.files) {
        counts.emplace(file.objectId, Counts{countVisibleFileGroups(file), file.groupSettings.size()});
        for (const auto& part : file.groupSettings) { counts.emplace(part.objectId, Counts{part.visible ? 1u : 0u, 1}); }
    }
    const auto visit = [&](auto&& self, const UiSceneNode& node) -> Counts {
        Counts count;
        if (node.kind == UiSceneNodeKind::group) {
            if (node.fileIndex < state.files.size() && node.groupIndex < state.files[node.fileIndex].groupSettings.size()) {
                const auto& file = state.files[node.fileIndex];
                count = {file.fileSettings.visible && file.groupSettings[node.groupIndex].visible ? 1u : 0u, 1};
            }
            return count;
        }
        if (node.kind == UiSceneNodeKind::file && node.children.empty() && node.fileIndex < state.files.size()) {
            const auto& file = state.files[node.fileIndex];
            return {countVisibleFileGroups(file), file.groupSettings.size()};
        }
        for (const auto& child : node.children) {
            const auto childCount = self(self, child);
            count.visible += childCount.visible; count.total += childCount.total;
        }
        if (node.kind == UiSceneNodeKind::folder) {
            if (!node.settings.visible) { count.visible = 0; }
            counts.emplace(node.objectId, count);
        } else if (node.fileIndex >= state.files.size() || !state.files[node.fileIndex].fileSettings.visible) { count.visible = 0; }
        return count;
    };
    for (const auto& node : state.sceneNodes) { visit(visit, node); }
    for (const auto id : state.selectedSceneObjects) {
        const auto found = counts.find(id);
        if (found == counts.end()) { return {}; }
        include(found->second.visible, found->second.total);
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
    if (before != createSceneDocument(state)) { recalculateSceneBounds(state); markSceneDirty(state, SceneChange::visibility); }
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
        const boost::unordered_flat_set<SceneObjectId> parts(partTargets.begin(), partTargets.end());
        const boost::unordered_flat_set<SceneObjectId> vertices(vertexTargets.begin(), vertexTargets.end());
        size_t colorIndex = 0;
        for (auto& file : state.files) {
            if (vertices.contains(file.objectId)) { setFileVertexSizeScale(file, 1.0f); }
            for (size_t i = 0; i < file.groupSettings.size(); ++i) {
                auto& part = file.groupSettings[i];
                if (parts.contains(part.objectId)) {
                    const bool points = i < file.mesh.nodes.size() && file.mesh.nodes[i].pointIndexCount != 0;
                    resetGroupColor(part, colorIndex);
                    setGroupVertexSizeScale(part, 1.0f);
                    setGroupRenderMode(part, UiRenderMode::solidMesh, !points);
                    setGroupRenderMode(part, UiRenderMode::triangles, false);
                    setGroupRenderMode(part, UiRenderMode::vertices, points);
                    setGroupUvGrid(part, {});
                    setGroupLineStyle(part, {});
                }
                ++colorIndex;
            }
        }
    }
    if (before != createSceneDocument(state)) { recalculateSceneBounds(state); markSceneDirty(state, group == UiPropertyGroup::appearance ? SceneChange::appearance | SceneChange::visibility | SceneChange::picking : SceneChange::geometry); }
}

} // namespace woby
