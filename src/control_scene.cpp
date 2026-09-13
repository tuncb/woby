#include "control_scene.h"
#include "ui_operations.h"
#include "utf8_path.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace woby {
using Json = nlohmann::json;
namespace {
template <typename Settings>
Json settingsInfo(const Settings& settings)
{
    return {{"visible", settings.visible}, {"translation", settings.translation},
        {"rotationDegrees", settings.rotationDegrees}, {"scale", settings.scale},
        {"opacity", settings.opacity}, {"center", settings.center}};
}
Json groupInfo(const UiGroupState& group)
{
    auto result = settingsInfo(group);
    result.update({{"solid", group.showSolidMesh}, {"triangles", group.showTriangles},
        {"vertices", group.showVertices}, {"color", group.color}, {"vertexSizeScale", group.vertexSizeScale}});
    return result;
}
Json boundsInfo(const Bounds& bounds)
{
    return {{"min", bounds.min}, {"max", bounds.max}, {"center", bounds.center}, {"radius", bounds.radius}};
}
const UiSceneNode* findFolder(const std::vector<UiSceneNode>& nodes, SceneObjectId id)
{
    for (const auto& node : nodes) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId == id) { return &node; }
        if (const auto* found = findFolder(node.children, id)) { return found; }
    }
    return nullptr;
}
UiSceneNode* findFolder(std::vector<UiSceneNode>& nodes, SceneObjectId id)
{
    for (auto& node : nodes) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId == id) { return &node; }
        if (auto* found = findFolder(node.children, id)) { return found; }
    }
    return nullptr;
}
Json modeCount(size_t enabled, size_t total)
{
    return {{"enabled", enabled}, {"total", total}, {"state", enabled == 0 ? "off" : enabled == total ? "on" : "mixed"}};
}
Json fileInfo(const UiFileState& file)
{
    auto result = settingsInfo(file.fileSettings);
    result["vertexSizeScale"] = file.vertexSizeScale;
    Json modes = Json::object();
    for (auto mode : {UiRenderMode::solidMesh, UiRenderMode::triangles, UiRenderMode::vertices}) {
        const char* name = mode == UiRenderMode::solidMesh ? "solid" : mode == UiRenderMode::triangles ? "triangles" : "vertices";
        modes[name] = modeCount(countEnabledGroupRenderMode(file.groupSettings, mode), file.groupSettings.size());
    }
    result["renderModes"] = modes;
    return result;
}
Json localObjectDetails(const UiState& state, SceneObjectId id)
{
    if (const auto* item = findAnnotation(state, id)) { return controlAnnotationDetails(state, *item); }
    if (id != invalidSceneObjectId) {
        if (const auto* comparison = findComparison(state, id)) {
            const auto& settings = comparison->settings;
            const char* mode = settings.mode == ComparisonMode::distance ? "distance"
                : settings.mode == ComparisonMode::original ? "a" : settings.mode == ComparisonMode::repaired ? "b" : settings.mode == ComparisonMode::surfaceQuality ? "surface_quality" : "overlay";
            return {{"settings", {{"visible", settings.enabled}, {"translation", comparison->translation},
                {"mode", mode}, {"distanceOnA", settings.distanceOnOriginal}, {"tolerance", settings.tolerance},
                {"colorRange", settings.colorRange}, {"showEdges", settings.showEdges},
                {"nonManifoldVertices", settings.topologyInspection.nonManifoldVertices},
                {"showNonManifoldVertices", settings.topologyInspection.showNonManifoldVertices},
                {"holes", settings.topologyInspection.holes},
                {"showHoles", settings.topologyInspection.showHoles},
                {"holeSizeRatioTolerance", settings.topologyInspection.holeSizeRatioTolerance},
                {"degenerateTriangles", settings.degenerates.enabled}, {"showDegenerateTriangles", settings.degenerates.show},
                {"needleThresholdRatio", settings.degenerates.needleThresholdRatio}, {"capMinAngleDegrees", settings.degenerates.capMinAngleDegrees},
                {"duplicatePoints", settings.duplicates.points}, {"duplicateTriangles", settings.duplicates.triangles}, {"showDuplicatePoints", settings.duplicates.showPoints}, {"showDuplicateTriangles", settings.duplicates.showTriangles},
                {"qualityMetric", surfaceQualityMetricKey(settings.quality.metric)}, {"qualityOnA", settings.quality.onOriginal},
                {"qualityMinimumEnabled", settings.quality.minimumEnabled}, {"qualityMaximumEnabled", settings.quality.maximumEnabled},
                {"qualityMinimumSize", settings.quality.minimumSize}, {"qualityMaximumSize", settings.quality.maximumSize},
                {"showBoundaries", settings.showBoundaries}, {"showNonManifold", settings.showNonManifold}, {"showWinding", settings.showWinding}, {"topologyMode", topologyModeName(settings.topologyMode)}}},
                {"valid", canInspectComparison(state, id)}, {"missingPartCount", missingComparisonPartCount(state, id)},
                {"aPartCount", comparison->a.size()}, {"bPartCount", comparison->b.size()}};
        }
    }
    if (const auto* folder = findFolder(state.sceneNodes, id)) {
        auto settings = settingsInfo(folder->settings);
        Json modes = Json::object();
        const auto total = countSceneNodeGroups(state, *folder);
        modes["solid"] = modeCount(countEnabledSceneNodeRenderMode(state, *folder, UiRenderMode::solidMesh), total);
        modes["triangles"] = modeCount(countEnabledSceneNodeRenderMode(state, *folder, UiRenderMode::triangles), total);
        modes["vertices"] = modeCount(countEnabledSceneNodeRenderMode(state, *folder, UiRenderMode::vertices), total);
        settings["renderModes"] = modes;
        return {{"settings", settings}, {"groupCount", total}};
    }
    for (const auto& file : state.files) {
        if (file.objectId == id) {
            return {{"settings", fileInfo(file)}, {"importerId", file.importerId},
                {"vertexCount", file.mesh.vertices.size()}, {"triangleCount", file.mesh.indices.size() / 3},
                {"groupCount", file.groupSettings.size()}, {"localBounds", boundsInfo(file.mesh.bounds)}};
        }
        for (size_t index = 0; index < file.groupSettings.size(); ++index) {
            const auto& group = file.groupSettings[index];
            if (group.objectId == id) {
                return {{"settings", groupInfo(group)}, {"triangleCount", file.mesh.nodes.at(index).indexCount / 3},
                    {"localBounds", group.localBoundsValid ? boundsInfo(group.localBounds) : Json(nullptr)}};
            }
        }
    }
    throw std::invalid_argument("Unknown or stale object ID.");
}

Json treeNode(const UiState& state, const UiSceneNode& node, const ObjectIdFormatter& formatId,
    const std::array<float, 16>& parent, bool parentVisible, float parentOpacity,
    std::vector<size_t> path, bool implicit = false)
{
    std::array<float, 16> local{}, world{};
    bool visible = true;
    float opacity = 1;
    Json result = {{"id", formatId(node.objectId)}, {"name", node.name}, {"occurrence", path}, {"implicit", implicit}};
    if (node.kind == UiSceneNodeKind::folder) {
        result["kind"] = "folder";
        result["settings"] = settingsInfo(node.settings);
        sceneNodeTransformMatrix(node.settings, local.data());
        visible = node.settings.visible;
        opacity = node.settings.opacity;
    } else {
        const auto& file = state.files.at(node.fileIndex);
        if (node.kind == UiSceneNodeKind::file) {
            result["kind"] = "file";
            result["path"] = pathToUtf8(file.path);
            result["importerId"] = file.importerId;
            result["settings"] = fileInfo(file);
            fileTransformMatrix(file.fileSettings, local.data());
            visible = file.fileSettings.visible;
            opacity = file.fileSettings.opacity;
        } else {
            const auto& group = file.groupSettings.at(node.groupIndex);
            result["kind"] = "group";
            result["fileId"] = formatId(file.objectId);
            result["settings"] = groupInfo(group);
            groupTransformMatrix(group, local.data());
            visible = group.visible;
            opacity = group.opacity;
            result["effectiveVertexSizePixels"] = std::lround(std::clamp(
                state.masterVertexPointSize * (file.vertexSizeScale * group.vertexSizeScale), minVertexPointSize, maxVertexPointSize));
        }
    }
    bx::mtxMul(world.data(), parent.data(), local.data());
    result["effective"] = {{"visible", parentVisible && visible}, {"opacity", parentOpacity * opacity}, {"worldMatrix", world}};
    result["children"] = Json::array();
    if (node.kind == UiSceneNodeKind::group) { return result; }
    std::vector<UiSceneNode> generatedChildren;
    const bool implicitChildren = node.kind == UiSceneNodeKind::file && node.children.empty();
    if (implicitChildren) { generatedChildren = createFileSceneNode(state.files.at(node.fileIndex), node.fileIndex).children; }
    const auto& children = implicitChildren ? generatedChildren : node.children;
    for (size_t index = 0; index < children.size(); ++index) {
        auto childPath = path;
        childPath.push_back(index);
        result["children"].push_back(treeNode(state, children[index], formatId, world,
            parentVisible && visible, parentOpacity * opacity, std::move(childPath), implicitChildren));
    }
    return result;
}
void collectOccurrences(const Json& nodes, const std::string& id, Json& result)
{
    for (const auto& node : nodes) {
        if (node["id"] == id) { result.push_back({{"occurrence", node["occurrence"]}, {"implicit", node["implicit"]}, {"effective", node["effective"]}}); }
        collectOccurrences(node["children"], id, result);
    }
}

struct Target {
    UiSceneNode* folder = nullptr;
    UiFileState* file = nullptr;
    UiGroupState* group = nullptr;
    size_t colorIndex = 0;
};
Target resolveTarget(UiState& state, SceneObjectId id)
{
    if (auto* folder = findFolder(state.sceneNodes, id)) { return {folder}; }
    size_t colorIndex = 0;
    for (auto& file : state.files) {
        if (file.objectId == id) { return {nullptr, &file}; }
        for (auto& group : file.groupSettings) {
            if (group.objectId == id) { return {nullptr, &file, &group, colorIndex}; }
            ++colorIndex;
        }
    }
    throw std::invalid_argument("Unknown or stale object ID.");
}

void editTransform(Target& target, const ControlOperation& command)
{
    const bool reset = command.action == ControlAction::transformReset;
    if (target.group) {
        if (reset) { resetGroupTransform(*target.group); }
        if (command.translation) { setGroupTranslation(*target.group, *command.translation); }
        if (command.rotationDegrees) { setGroupRotationDegrees(*target.group, *command.rotationDegrees); }
        if (command.scale) { setGroupScale(*target.group, *command.scale); }
        if (command.value) { setGroupOpacity(*target.group, *command.value); }
    } else if (target.file) {
        auto& settings = target.file->fileSettings;
        if (reset) { resetFileTransform(settings); }
        if (command.translation) { setFileTranslation(settings, *command.translation); }
        if (command.rotationDegrees) { setFileRotationDegrees(settings, *command.rotationDegrees); }
        if (command.scale) { setFileScale(settings, *command.scale); }
        if (command.value) { setFileOpacity(settings, *command.value); }
    } else if (target.folder) {
        auto& settings = target.folder->settings;
        if (reset) { resetSceneNodeTransform(settings); }
        if (command.translation) { setSceneNodeTranslation(settings, *command.translation); }
        if (command.rotationDegrees) { setSceneNodeRotationDegrees(settings, *command.rotationDegrees); }
        if (command.scale) { setSceneNodeScale(settings, *command.scale); }
        if (command.value) { setSceneNodeOpacity(settings, *command.value); }
    }
}
}

Json controlSceneInfo(const UiState& state)
{
    size_t vertices = 0, triangles = 0;
    for (const auto& file : state.files) { vertices += file.mesh.vertices.size(); triangles += file.mesh.indices.size() / 3; }
    Json modes = Json::object();
    const size_t groups = totalGroupCount(state);
    modes["solid"] = modeCount(countEnabledSceneRenderMode(state, UiRenderMode::solidMesh), groups);
    modes["triangles"] = modeCount(countEnabledSceneRenderMode(state, UiRenderMode::triangles), groups);
    modes["vertices"] = modeCount(countEnabledSceneRenderMode(state, UiRenderMode::vertices), groups);
    return {{"dirty", state.isDirty}, {"fileCount", state.files.size()}, {"analysisCount", state.comparisons.size()}, {"annotationCount", state.annotations.size()}, {"groupCount", groups},
        {"visibleGroupCount", countVisibleSceneGroups(state)}, {"vertexCount", vertices}, {"triangleCount", triangles},
        {"showGrid", state.showGrid}, {"showDimensions", state.showDimensions},
        {"showOrigin", state.showOrigin}, {"upAxis", state.upAxis == SceneUpAxis::y ? "y" : "z"},
        {"masterVertexPointSize", state.masterVertexPointSize}, {"renderModes", modes}, {"bounds", boundsInfo(state.sceneBounds)}};
}

Json controlCameraInfo(const UiState& state)
{
    const auto& camera = state.camera;
    const auto eye = cameraEye(camera, state.upAxis);
    const auto up = cameraUp(camera, state.upAxis);
    constexpr float radiansToDegrees = 57.29577951308232f;
    return {{"target", camera.target}, {"eye", {eye.x, eye.y, eye.z}}, {"up", {up.x, up.y, up.z}},
        {"yawDegrees", camera.yawRadians * radiansToDegrees}, {"pitchDegrees", camera.pitchRadians * radiansToDegrees},
        {"rollDegrees", camera.rollRadians * radiansToDegrees}, {"distance", camera.distance},
        {"verticalFovDegrees", camera.verticalFovDegrees}, {"nearPlane", camera.nearPlane},
        {"farPlane", cameraFarPlane(camera, state.sceneBounds)}, {"upAxis", state.upAxis == SceneUpAxis::y ? "y" : "z"}};
}

Json controlSceneTree(const UiState& state, const ObjectIdFormatter& formatId)
{
    std::array<float, 16> identity{};
    bx::mtxIdentity(identity.data());
    Json result = Json::array();
    if (state.sceneNodes.empty()) {
        for (size_t index = 0; index < state.files.size(); ++index) {
            result.push_back(treeNode(state, createFileSceneNode(state.files[index], index), formatId, identity, true, 1, {index}, true));
        }
    } else {
        for (size_t index = 0; index < state.sceneNodes.size(); ++index) {
            result.push_back(treeNode(state, state.sceneNodes[index], formatId, identity, true, 1, {index}));
        }
    }
    for (const auto& comparison : state.comparisons) {
        std::array<float, 16> world{};
        bx::mtxTranslate(world.data(), comparison.translation[0], comparison.translation[1], comparison.translation[2]);
        result.push_back({{"id", formatId(comparison.objectId)}, {"name", comparison.name}, {"kind", "analysis"},
            {"occurrence", {result.size()}}, {"implicit", false}, {"children", Json::array()},
            {"settings", localObjectDetails(state, comparison.objectId)["settings"]},
            {"effective", {{"visible", comparison.settings.enabled && canInspectComparison(state, comparison.objectId)},
                {"opacity", 1}, {"worldMatrix", world}}}});
    }
    for (const auto& item : state.annotations) {
        const auto details = controlAnnotationDetails(state, item);
        result.push_back({{"id", formatId(item.objectId)}, {"name", item.settings.name}, {"kind", "annotation"},
            {"occurrence", {result.size()}}, {"implicit", false}, {"children", Json::array()},
            {"settings", details["settings"]}, {"effective", {{"visible", details["effectiveVisible"]}}}});
    }
    return result;
}

Json controlObjectDetails(const UiState& state, SceneObjectId id, const ObjectIdFormatter& formatId)
{
    auto result = localObjectDetails(state, id);
    if (const auto* item = findAnnotation(state, id)) {
        result["sourceId"] = findSceneObject(state, item->targetId) ? Json(formatId(item->targetId)) : Json(nullptr);
    }
    if (id != invalidSceneObjectId) {
        if (const auto* comparison = findComparison(state, id)) {
            const auto inputs = [&](const std::vector<UiComparisonPart>& members) {
                auto list = Json::array();
                for (const auto& part : members) {
                    const bool missing = comparisonObjectParts(state, {part.objectId}).empty();
                    list.push_back({{"id", missing ? Json(nullptr) : Json(formatId(part.objectId))},
                        {"name", part.name}, {"missing", missing}, {"enabled", part.enabled}});
                }
                return list;
            };
            result["a"] = inputs(comparison->a);
            result["b"] = inputs(comparison->b);
        }
    }
    result["occurrences"] = Json::array();
    collectOccurrences(controlSceneTree(state, formatId), formatId(id), result["occurrences"]);
    return result;
}

Json applyControlSceneOperation(UiState& state, const SceneDocument& cleanDocument,
    const ControlOperation& command, const ObjectIdFormatter& formatId, float minPaneWidth, float maxPaneWidth)
{
    using A = ControlAction;
    if (command.action == A::annotationList || command.action == A::annotationGet
        || command.action == A::annotationCreate || command.action == A::annotationSet
        || command.action == A::annotationMove || command.action == A::annotationReshape || command.action == A::annotationDelete
        || findAnnotation(state, command.objectId)) {
        return applyControlAnnotationOperation(state, cleanDocument, command, formatId);
    }
    if (command.action == A::comparisonCreate || command.action == A::comparisonDelete
        || command.action == A::comparisonSet || command.action == A::comparisonAdd
        || command.action == A::comparisonRemove || command.action == A::comparisonClear
        || command.action == A::comparisonSwap || command.action == A::comparisonEnable) {
        // Validate all inputs before any mutation, including creation of an empty object.
        if (command.action != A::comparisonCreate
            && (command.objectId == invalidSceneObjectId || !findComparison(state, command.objectId))) {
            throw std::invalid_argument("This command requires an analysis ID.");
        }
        for (const auto& [supplied, id] : {std::pair{command.a.has_value(), command.aId},
            std::pair{command.b.has_value(), command.bId}, std::pair{command.object.has_value(), command.memberId}}) {
            if (supplied && comparisonObjectParts(state, {id}).empty()) {
                throw std::invalid_argument("Analysis inputs require a file, folder, or group containing triangles.");
            }
        }
        auto id = command.objectId;
        if (command.action == A::comparisonCreate) {
            id = createComparison(state);
            if (command.name) { renameComparison(state, id, *command.name); }
            if (command.a) { setComparisonObjects(state, {command.aId}, ComparisonSide::a, true, id); }
            if (command.b) { setComparisonObjects(state, {command.bId}, ComparisonSide::b, true, id); }
        } else if (command.action == A::comparisonDelete) {
            removeComparison(state, id);
            updateSceneDirty(state, cleanDocument);
            return {{"removed", command.target}, {"dirty", state.isDirty}};
        } else if (command.action == A::comparisonSet) {
            auto settings = comparisonSettings(state, id);
            if (command.visible) { settings.enabled = *command.visible; }
            if (command.mode) {
                settings.mode = *command.mode == "distance" ? ComparisonMode::distance : *command.mode == "a"
                    ? ComparisonMode::original : *command.mode == "b" ? ComparisonMode::repaired :
                    *command.mode == "surface_quality" ? ComparisonMode::surfaceQuality : ComparisonMode::overlay;
            }
            if (command.nonManifoldVertices) { settings.topologyInspection.nonManifoldVertices = *command.nonManifoldVertices; }
            if (command.showNonManifoldVertices) { settings.topologyInspection.showNonManifoldVertices = *command.showNonManifoldVertices; }
            if (command.holes) { settings.topologyInspection.holes = *command.holes; }
            if (command.showHoles) { settings.topologyInspection.showHoles = *command.showHoles; }
            if (command.holeSizeRatioTolerance) { settings.topologyInspection.holeSizeRatioTolerance = *command.holeSizeRatioTolerance; }
            if (command.degenerateTriangles) { settings.degenerates.enabled = *command.degenerateTriangles; }
            if (command.showDegenerateTriangles) { settings.degenerates.show = *command.showDegenerateTriangles; }
            if (command.needleThresholdRatio) { settings.degenerates.needleThresholdRatio = *command.needleThresholdRatio; }
            if (command.capMinAngleDegrees) { settings.degenerates.capMinAngleDegrees = *command.capMinAngleDegrees; }
            if (command.duplicatePoints) { settings.duplicates.points = *command.duplicatePoints; }
            if (command.duplicateTriangles) { settings.duplicates.triangles = *command.duplicateTriangles; }
            if (command.showDuplicatePoints) { settings.duplicates.showPoints = *command.showDuplicatePoints; }
            if (command.showDuplicateTriangles) { settings.duplicates.showTriangles = *command.showDuplicateTriangles; }
            if (command.qualityMetric) { settings.quality.metric = parseSurfaceQualityMetric(*command.qualityMetric); }
            if (command.qualityOnA) { settings.quality.onOriginal = *command.qualityOnA; }
            if (command.qualityMinimumEnabled) { settings.quality.minimumEnabled = *command.qualityMinimumEnabled; }
            if (command.qualityMaximumEnabled) { settings.quality.maximumEnabled = *command.qualityMaximumEnabled; }
            if (command.qualityMinimumSize) { settings.quality.minimumSize = *command.qualityMinimumSize; }
            if (command.qualityMaximumSize) { settings.quality.maximumSize = *command.qualityMaximumSize; }
            if (command.distanceOnA) { settings.distanceOnOriginal = *command.distanceOnA; }
            if (command.tolerance) { settings.tolerance = *command.tolerance; }
            if (command.colorRange) { settings.colorRange = *command.colorRange; }
            if (command.showEdges) { settings.showEdges = *command.showEdges; }
            if (command.showBoundaries) { settings.showBoundaries = *command.showBoundaries; }
            if (command.topologyMode) { settings.topologyMode = parseTopologyMode(*command.topologyMode); }
            if (command.showWinding) { settings.showWinding = *command.showWinding; }
            if (command.showNonManifold) { settings.showNonManifold = *command.showNonManifold; }
            setComparisonSettings(state, settings, id);
            if (command.name) { renameComparison(state, id, *command.name); }
        } else if (command.action == A::comparisonSwap) {
            swapComparisonGroups(state, id);
        } else {
            const auto side = *command.side == "a" ? ComparisonSide::a : ComparisonSide::b;
            if (command.action == A::comparisonClear) { clearComparisonGroup(state, side, id); }
            else if (command.action == A::comparisonEnable) {
                setComparisonObjectsEnabled(state, command.object ? std::vector<SceneObjectId>{command.memberId}
                    : std::vector<SceneObjectId>{}, side, *command.enabled, id);
            }
            else { setComparisonObjects(state, {command.memberId}, side, command.action == A::comparisonAdd, id); }
        }
        updateSceneDirty(state, cleanDocument);
        auto object = controlObjectDetails(state, id, formatId);
        object.update({{"id", formatId(id)}, {"name", findComparison(state, id)->name}, {"kind", "analysis"}});
        return {{"target", formatId(id)}, {"object", std::move(object)}, {"dirty", state.isDirty}, {"bounds", boundsInfo(state.sceneBounds)}};
    }
    if (command.objectId != invalidSceneObjectId && findComparison(state, command.objectId)) {
        if (command.action == A::transformGet) {
            return {{"target", command.target}, {"settings", localObjectDetails(state, command.objectId)["settings"]}};
        }
        if (command.action == A::visibility) {
            auto settings = comparisonSettings(state, command.objectId);
            settings.enabled = *command.visible;
            setComparisonSettings(state, settings, command.objectId);
        } else if (command.action == A::transformSet || command.action == A::transformReset) {
            if (command.rotationDegrees || command.scale || command.value) {
                throw std::invalid_argument("Analysis transforms support display translation only.");
            }
            if (command.action == A::transformReset || command.translation) {
                setComparisonTranslation(state, command.objectId, command.translation.value_or(std::array<float, 3>{}));
            }
        } else {
            throw std::invalid_argument("This command does not apply to analysis objects.");
        }
        updateSceneDirty(state, cleanDocument);
        return {{"target", command.target}, {"applied", localObjectDetails(state, command.objectId)["settings"]},
            {"dirty", state.isDirty}, {"bounds", boundsInfo(state.sceneBounds)}};
    }
    Target target;
    if (command.objectId != invalidSceneObjectId) { target = resolveTarget(state, command.objectId); }
    const bool scene = command.target == "scene";
    switch (command.action) {
    case A::sceneInfo: case A::stats: return controlSceneInfo(state);
    case A::sceneTree: return {{"nodes", controlSceneTree(state, formatId)}};
    case A::sceneBounds: return {{"bounds", boundsInfo(state.sceneBounds)}, {"visibleOnly", true}, {"emptyFallback", boundsInfo(defaultDisplayBounds())}};
    case A::transformGet: return {{"target", command.target}, {"settings", localObjectDetails(state, command.objectId)["settings"]}};
    case A::visibility:
        if (scene) { setAllSceneVisible(state, *command.visible); }
        else if (target.group) { setGroupVisible(state, *target.file, *target.group, *command.visible); }
        else if (target.file) { setFileVisible(*target.file, *command.visible); refreshSceneTreeFolderVisibility(state); }
        else { setSceneNodeSubtreeVisible(state, *target.folder, *command.visible); }
        break;
    case A::render:
        for (const auto& [mode, enabled] : {std::pair{UiRenderMode::solidMesh, command.solid},
            std::pair{UiRenderMode::triangles, command.triangles}, std::pair{UiRenderMode::vertices, command.vertices}}) {
            if (!enabled) { continue; }
            if (scene) { setAllSceneRenderModes(state, mode, *enabled); }
            else if (target.group) { setGroupRenderMode(*target.group, mode, *enabled); }
            else if (target.file) { setAllGroupRenderModes(target.file->groupSettings, mode, *enabled); }
            else { setSceneNodeSubtreeRenderMode(state, *target.folder, mode, *enabled); }
        }
        break;
    case A::transformSet: case A::transformReset: case A::opacity: editTransform(target, command); break;
    case A::colorSet: case A::colorReset:
        if (!target.group) { throw std::invalid_argument("Color commands require a group ID."); }
        if (command.action == A::colorReset) { resetGroupColor(*target.group, target.colorIndex); }
        else { const auto& rgb = *command.rgb; setGroupColor(*target.group, {rgb[0], rgb[1], rgb[2], target.group->color[3]}); }
        break;
    case A::vertexSize:
        if (scene) { setMasterVertexPointSize(state, *command.pixels); }
        else if (target.group) { setGroupVertexSizeScale(*target.group, *command.scale); }
        else if (target.file) { setFileVertexSizeScale(*target.file, *command.scale); }
        else { throw std::invalid_argument("Vertex size supports scene, file, or group."); }
        break;
    case A::grid: setShowGrid(state, *command.visible); break;
    case A::dimensions: setShowDimensions(state, *command.visible); break;
    case A::origin: setShowOrigin(state, *command.visible); break;
    case A::upAxis: setSceneUpAxis(state, command.axis == "y" ? SceneUpAxis::y : SceneUpAxis::z); break;
    case A::viewList: case A::viewCreate: case A::viewApply: case A::viewUpdate: case A::viewRename: case A::viewDelete: {
        ViewId id = command.viewId.empty() ? 0 : std::stoull(command.viewId);
        if (id && !findView(state, id)) { throw std::invalid_argument("Unknown view ID; use view list to obtain current IDs."); }
        if (command.action == A::viewCreate) {
            id = createView(state);
            if (command.name) { renameView(state, id, *command.name); }
        } else if (command.action == A::viewApply) { applyView(state, id); }
        else if (command.action == A::viewUpdate) { updateView(state, id); }
        else if (command.action == A::viewRename) { renameView(state, id, *command.name); }
        else if (command.action == A::viewDelete) { removeView(state, id); }
        if (command.action != A::viewList) { updateSceneDirty(state, cleanDocument); }
        Json views = Json::array();
        for (const auto& view : state.views) { views.push_back({{"id", std::to_string(view.id)}, {"name", view.name}}); }
        Json result = {{"views", views}, {"activeViewId", state.activeViewId ? Json(std::to_string(state.activeViewId)) : Json(nullptr)}, {"dirty", state.isDirty}};
        if (id) { result["viewId"] = std::to_string(id); }
        return result;
    }
    case A::cameraView: {
        const std::array<std::pair<const char*, CameraView>, 7> presets = {{{"front", CameraView::front},
            {"back", CameraView::back}, {"left", CameraView::left}, {"right", CameraView::right},
            {"top", CameraView::top}, {"bottom", CameraView::bottom}, {"isometric", CameraView::isometric}}};
        for (const auto& [name, view] : presets) {
            if (command.preset == name) { setCameraView(state, view); return {{"camera", controlCameraInfo(state)}}; }
        }
        throw std::invalid_argument("Unknown camera preset.");
    }
    case A::cameraFrame:
        if (command.object) {
            (void)localObjectDetails(state, command.memberId);
            frameCameraToObject(state, command.memberId);
        } else { frameCameraToScene(state); }
        return {{"camera", controlCameraInfo(state)}};
    case A::cameraSet:
        setUiCamera(state, {command.cameraTarget, command.yawDegrees, command.pitchDegrees,
            command.rollDegrees, command.distance, command.fovDegrees, command.nearPlane});
        return {{"camera", controlCameraInfo(state)}};
    case A::cameraLookAt:
        lookAtUiCamera(state, *command.eye, *command.cameraTarget);
        return {{"camera", controlCameraInfo(state)}};
    case A::cameraGet: return {{"camera", controlCameraInfo(state)}};
    case A::cameraOrbit: case A::cameraPan: case A::cameraRoll: case A::cameraDolly: case A::cameraMove:
        navigateUiCamera(state, {command.yawDegrees.value_or(0), command.pitchDegrees.value_or(0), command.rollDegrees.value_or(0),
            command.right.value_or(0), command.up.value_or(0), command.forward.value_or(0), command.factor.value_or(1)});
        return {{"camera", controlCameraInfo(state)}};
    case A::pane:
        if (command.visible) { setViewerPaneVisible(state, *command.visible); }
        if (command.width) { setViewerPaneWidth(state, *command.width, minPaneWidth, maxPaneWidth); }
        return {{"pane", {{"visible", state.viewerPaneVisible}, {"width", state.viewerPaneWidth}}}};
    case A::status: case A::capabilities: case A::modelAdd: case A::modelRemove: case A::folderAdd:
    case A::importersList: case A::importersAdd: case A::importersScan: case A::importersForget: case A::performance:
    case A::annotationList: case A::annotationGet: case A::annotationCreate: case A::annotationSet:
    case A::annotationReshape: case A::annotationMove: case A::annotationDelete:
    case A::comparisonResults: case A::sceneUndo: case A::sceneRedo:
        throw std::invalid_argument("Command requires a runtime adapter.");
    case A::comparisonCreate: case A::comparisonDelete: case A::comparisonSet: case A::comparisonAdd:
    case A::comparisonRemove: case A::comparisonClear: case A::comparisonSwap: case A::comparisonEnable:
        throw std::invalid_argument("Analysis command was not dispatched.");
    }
    // Read-only and session-only operations return above. Struct-level setters
    // in this batch need one scene-edit notification for history recording.
    markSceneDirty(state);
    recalculateSceneBounds(state);
    updateSceneDirty(state, cleanDocument);
    if (command.objectId != invalidSceneObjectId) {
        return {{"target", command.target}, {"applied", localObjectDetails(state, command.objectId)["settings"]},
            {"dirty", state.isDirty}, {"bounds", boundsInfo(state.sceneBounds)}};
    }
    return controlSceneInfo(state);
}

namespace {
Json topologyFaceJson(const TopologyFaceReference& face)
{
    return {{"sourceId", std::to_string(face.fileId)}, {"partId", std::to_string(face.partId)}, {"triangleId", face.triangleId+1}};
}
Json topologyResultJson(const MeshTopology& topology, const std::vector<TopologyEdgeFinding>& edges, bool winding = false)
{
    constexpr size_t limit = 100;
    Json findings = Json::array(), sources = Json::array(), affected = Json::array();
    for (size_t i = 0; i < std::min(limit, topology.sources.size()); ++i) {
        const auto& source = topology.sources[i];
        sources.push_back({{"sourceId", std::to_string(source.fileId)}, {"source", source.source},
            {"provenance", triangleProvenanceName(source.provenance)}, {"topologyMode", topologyModeName(source.mode)},
            {"status", source.available ? "complete" : "unavailable"}, {"excludedCollapsedFaces", source.excludedCollapsedFaces}});
    }
    for (size_t i = 0; i < std::min(limit, edges.size()); ++i) {
        const auto& finding = edges[i];
        const auto& source = topology.sources[finding.source];
        const auto& edge = source.edges[finding.edge];
        Json faces = Json::array(), endpoints = Json::array();
        for (size_t k = 0; k < std::min(limit, edge.incidentFaces.size()); ++k) {
            const auto& use = edge.incidentFaces[k];
            auto face = topologyFaceJson(source.faces[use.face].reference);
            face["forward"] = use.forward;
            faces.push_back(std::move(face));
        }
        for (const auto vertexId : edge.vertices) {
            const auto& vertex = source.vertices[vertexId];
            Json refs = Json::array();
            for (size_t k = 0; k < std::min(limit, vertex.references.size()); ++k) {
                const auto& ref = vertex.references[k];
                refs.push_back({{"partId", std::to_string(ref.partId)}, {"pointId", ref.pointId+1}});
            }
            endpoints.push_back({{"position", vertex.position}, {"sourcePoints", refs},
                {"sourcePointCount", vertex.references.size()}, {"sourcePointsTruncated", vertex.references.size() > limit}});
        }
        findings.push_back({{"sourceId", std::to_string(source.fileId)}, {"source", source.source},
            {"edgeId", finding.edge+1}, {"topologyMode", topologyModeName(source.mode)},
            {"provenance", triangleProvenanceName(source.provenance)}, {"endpoints", endpoints},
            {"incidentFaceCount", edge.incidentFaces.size()}, {"incidentFaces", faces},
            {"incidentFacesTruncated", edge.incidentFaces.size() > limit},
            {"sameDirection", edge.windingConflict}, {"orientationContradiction", edge.orientationContradiction}});
    }
    const size_t count = winding ? topology.windingFaces.size() : edges.size();
    Json result = {{"status", topologyStatus(topology)}, {"algorithm", "woby-source-topology-v1"},
        {"topologyMode", topologyModeName(topology.mode)}, {"scope", "per-source; selected parts"},
        {"count", topology.unavailableSources ? Json(nullptr) : Json(count)}, {"knownCount", count},
        {"excludedCollapsedFaces", topology.excludedCollapsedFaces}, {"unavailableSources", topology.unavailableSources},
        {"sources", sources}, {"sourceCount", topology.sources.size()}, {"sourcesTruncated", topology.sources.size() > limit},
        {"findings", findings}, {"findingCount", edges.size()}, {"findingsTruncated", edges.size() > limit}};
    if (winding) {
        for (size_t i = 0; i < std::min(limit, topology.windingFaces.size()); ++i) { affected.push_back(topologyFaceJson(topology.windingFaces[i])); }
        result["affectedFaces"] = affected;
        result["affectedFacesTruncated"] = topology.windingFaces.size() > limit;
        result["orientationContradictions"] = topology.orientationContradictions;
        result["countUnit"] = "unique triangle instances";
    } else { result["countUnit"] = "edges"; }
    return result;
}
Json topologyVertexJson(const SourceTopology& source, size_t index)
{
    constexpr size_t limit = 100;
    const auto& vertex = source.vertices[index];
    Json references = Json::array(), faces = Json::array();
    for (size_t i = 0; i < std::min(limit, vertex.references.size()); ++i) {
        const auto& ref = vertex.references[i];
        references.push_back({{"partId", std::to_string(ref.partId)}, {"pointId", ref.pointId+1}});
    }
    for (size_t i = 0; i < std::min(limit, vertex.faces.size()); ++i) {
        faces.push_back(topologyFaceJson(source.faces[vertex.faces[i]].reference));
    }
    return {{"sourceId", std::to_string(source.fileId)}, {"source", source.source}, {"vertexId", index+1},
        {"position", vertex.position}, {"topologyMode", topologyModeName(source.mode)},
        {"pointReferences", references}, {"pointReferenceCount", vertex.references.size()}, {"pointReferencesTruncated", vertex.references.size() > limit},
        {"incidentFaces", faces}, {"incidentFaceCount", vertex.faces.size()}, {"incidentFacesTruncated", vertex.faces.size() > limit}};
}
Json topologyBoundaryJson(const MeshTopology& topology, size_t index)
{
    constexpr size_t limit = 100;
    const auto& boundary = topology.boundaryRegions[index];
    const auto& source = topology.sources[boundary.source];
    Json vertices = Json::array(), edges = Json::array(), faces = Json::array();
    for (size_t i = 0; i < std::min(limit, boundary.vertices.size()); ++i) {
        // One representative point reference per loop vertex avoids a third multiplicative array limit.
        const auto v = boundary.vertices[i];
        Json refs = Json::array();
        const auto& vertex = source.vertices[v];
        for (size_t j = 0; j < std::min(size_t{1}, vertex.references.size()); ++j) {
            const auto& ref = vertex.references[j];
            refs.push_back({{"partId", std::to_string(ref.partId)}, {"pointId", ref.pointId+1}});
        }
        vertices.push_back({{"vertexId", v+1}, {"position", vertex.position}, {"pointReferences", refs},
            {"pointReferenceCount", vertex.references.size()}, {"pointReferencesTruncated", vertex.references.size() > 1}});
    }
    for (size_t i = 0; i < std::min(limit, boundary.edges.size()); ++i) {
        const auto e = boundary.edges[i];
        edges.push_back(e+1);
        faces.push_back(topologyFaceJson(source.faces[source.edges[e].incidentFaces[0].face].reference));
    }
    return {{"sourceId", std::to_string(source.fileId)}, {"source", source.source}, {"boundaryId", index+1},
        {"componentId", boundary.component+1}, {"topologyMode", topologyModeName(source.mode)},
        {"kind", boundaryKindName(boundary.kind)}, {"diagonal", boundary.diagonal}, {"componentDiagonal", boundary.componentDiagonal},
        {"sizeRatio", boundary.ratioAvailable ? Json(boundary.sizeRatio) : Json(nullptr)},
        {"vertices", vertices}, {"vertexCount", boundary.vertices.size()}, {"verticesTruncated", boundary.vertices.size() > limit},
        {"edgeIds", edges}, {"edgeCount", boundary.edges.size()}, {"edgesTruncated", boundary.edges.size() > limit},
        {"incidentFacesByEdge", faces}};
}
Json topologyInspectionJson(const MeshTopology& topology, bool holes)
{
    constexpr size_t limit = 100;
    const bool enabled = holes ? topology.inspection.holes : topology.inspection.nonManifoldVertices;
    const size_t count = enabled ? (holes ? topology.holes.size() : topology.nonManifoldVertices.size()) : 0;
    Json result = topologyResultJson(topology, {});
    result["algorithm"] = holes ? "woby-boundary-loops-v1" : "woby-vertex-links-v1";
    result["status"] = enabled ? topologyStatus(topology) : "disabled";
    result["count"] = !enabled || topology.unavailableSources ? Json(nullptr) : Json(count);
    result["knownCount"] = count; result["findingCount"] = count;
    result["findingsTruncated"] = count > limit;
    Json findings = Json::array();
    for (size_t i = 0; i < std::min(limit, count); ++i) {
        if (holes) { findings.push_back(topologyBoundaryJson(topology, topology.holes[i])); }
        else {
            const auto& finding = topology.nonManifoldVertices[i];
            auto value = topologyVertexJson(topology.sources[finding.source], finding.vertex);
            value["linkComponents"] = finding.linkComponents;
            findings.push_back(std::move(value));
        }
    }
    result["findings"] = std::move(findings);
    if (holes) {
        result["holeSizeRatioTolerance"] = topology.inspection.holeSizeRatioTolerance;
        Json boundaries = Json::array();
        size_t loops = 0, open = 0, branched = 0, unavailableRatios = 0;
        if (enabled) {
            for (size_t i = 0; i < topology.boundaryRegions.size(); ++i) {
                const auto& boundary = topology.boundaryRegions[i];
                loops += boundary.kind == BoundaryKind::loop;
                open += boundary.kind == BoundaryKind::open;
                branched += boundary.kind == BoundaryKind::branched;
                unavailableRatios += !boundary.ratioAvailable;
                if (i < limit) { boundaries.push_back(topologyBoundaryJson(topology, i)); }
            }
        }
        result["boundaryRegions"] = std::move(boundaries);
        result["boundaryRegionCount"] = loops + open + branched;
        result["boundaryRegionsTruncated"] = loops + open + branched > limit;
        result["loopCount"] = loops; result["openBoundaryCount"] = open; result["branchedBoundaryCount"] = branched;
        result["unavailableSizeRatioCount"] = unavailableRatios;
    } else { result["excludedNonManifoldEdgeVertices"] = enabled ? topology.excludedNonManifoldEdgeVertices : 0; }
    return result;
}

Json degenerateResultJson(const MeshDegenerates& result)
{
    const bool enabled = result.settings.enabled;
    Json findings = Json::array();
    constexpr size_t limit = 100;
    const auto number = [](double value) { return std::isfinite(value) ? Json(value) : Json(nullptr); };
    for (size_t i = 0; enabled && i < std::min(limit, result.findings.size()); ++i) {
        const auto& f = result.findings[i];
        findings.push_back({{"sourceId", std::to_string(f.fileId)}, {"partId", std::to_string(f.partId)},
            {"source", f.source}, {"provenance", triangleProvenanceName(f.provenance)}, {"triangleId", f.triangleId+1},
            {"reasons", {{"collapsed", f.reasons.collapsed}, {"needle", f.reasons.needle}, {"cap", f.reasons.cap}}},
            {"edgeRatio", number(f.reasons.edgeRatio)}, {"maximumAngleDegrees", number(f.reasons.maximumAngleDegrees)}});
    }
    return {{"status", degenerateStatus(result)}, {"algorithm", "woby-degenerate-triangles-v1"},
        {"scope", "per-source generated triangle / transformed part instance; world coordinates; no display offset"},
        {"needleThresholdRatio", result.settings.needleThresholdRatio}, {"capMinAngleDegrees", result.settings.capMinAngleDegrees},
        {"count", enabled && !result.unavailableSources ? Json(result.findings.size()) : Json(nullptr)},
        {"knownCount", enabled ? result.findings.size() : 0}, {"unavailableSources", enabled ? result.unavailableSources : 0},
        {"reasonCounts", {{"collapsed", enabled ? result.collapsedCount : 0}, {"needle", enabled ? result.needleCount : 0}, {"cap", enabled ? result.capCount : 0}}},
        {"findings", findings}, {"findingsTruncated", enabled && result.findings.size() > limit}};
}
Json duplicateResultJson(const DuplicateResult& result)
{
    if (!result.enabled) {
        return {{"status", "disabled"}, {"count", nullptr}, {"knownDuplicateCount", 0}, {"informationalCount", 0},
            {"unavailableSources", 0}, {"groupCount", 0}, {"findings", Json::array()}, {"findingsTruncated", false}};
    }
    constexpr size_t limit = 100;
    Json findings = Json::array();
    for (size_t i = 0; i < std::min(limit, result.findings.size()); ++i) {
        const auto& finding = result.findings[i];
        Json members = Json::array();
        for (size_t k = 0; k < std::min(limit, finding.members.size()); ++k) {
            members.push_back({{"id", finding.members[k].id+1}, {"reversed", finding.members[k].reversed}});
        }
        findings.push_back({{"sourceId", std::to_string(finding.fileId)}, {"source", finding.source},
            {"provenance", sourceProvenanceName(finding.provenance)}, {"representativeId", finding.members.front().id+1},
            {"memberCount", finding.members.size()}, {"members", members}, {"membersTruncated", finding.members.size() > limit}});
    }
    const bool complete = result.enabled && result.unavailableSources == 0;
    return {{"status", duplicateStatus(result)}, {"count", complete ? Json(result.duplicateCount) : Json(nullptr)},
        {"knownDuplicateCount", result.duplicateCount}, {"informationalCount", result.informationalCount},
        {"unavailableSources", result.unavailableSources}, {"groupCount", result.findings.size()},
        {"findings", findings}, {"findingsTruncated", result.findings.size() > limit}};
}
}

Json controlComparisonResults(const MeshComparison& result, double tolerance)
{
    const auto surface = [tolerance](const SurfaceComparison& value) {
        const auto& diagnostics = value.diagnostics;
        if (value.source.indices.empty()) { return Json(nullptr); }
        const bool measured = !value.distances.empty();
        Json quality = {{"degenerateTriangles", value.quality.degenerateTriangles}};
        for (size_t i = 0; i < surfaceQualityMetricCount; ++i) {
            const auto& stats = value.quality.statistics[i];
            quality[surfaceQualityMetricKey(static_cast<SurfaceQualityMetric>(i))] = {
                {"count", stats.count}, {"minimum", stats.count ? Json(stats.minimum) : Json(nullptr)},
                {"percentile5", stats.count ? Json(stats.percentile5) : Json(nullptr)},
                {"median", stats.count ? Json(stats.median) : Json(nullptr)},
                {"percentile95", stats.count ? Json(stats.percentile95) : Json(nullptr)},
                {"maximum", stats.count ? Json(stats.maximum) : Json(nullptr)}};
        }
        return Json{{"maximum", measured ? Json(value.maximum) : Json(nullptr)},
            {"mean", measured ? Json(value.mean) : Json(nullptr)},
            {"percentile95", measured ? Json(value.percentile95) : Json(nullptr)},
            {"percentAboveTolerance", measured ? Json(surfacePercentAboveTolerance(value, tolerance)) : Json(nullptr)},
            {"detectors", {{"schemaVersion", 1}, {"idBase", 1}, {"scope", "per-source; selected parts, including unused points when whole file selected"},
                {"duplicate_points", duplicateResultJson(value.duplicates.points)}, {"duplicate_tris", duplicateResultJson(value.duplicates.triangles)}, {"degenerate_tris", degenerateResultJson(value.degenerates)},
                {"non_manifold_vertices", topologyInspectionJson(value.topology, false)},
                {"holes", topologyInspectionJson(value.topology, true)},
                {"boundary_edges", topologyResultJson(value.topology, value.topology.boundaries)},
                {"non_manifold_edges", topologyResultJson(value.topology, value.topology.nonManifoldEdges)},
                {"inconsistently_oriented_tris", topologyResultJson(value.topology, value.topology.windingEdges, true)}}},
            {"surfaceMeshQuality", quality}, {"sampleCount", value.distances.size()}, {"triangleCount", value.source.indices.size() / 3},
            {"diagnostics", {{"boundaryEdges", diagnostics.boundaryEdges.size()},
                {"nonManifoldEdges", diagnostics.nonManifoldEdges.size()},
                {"inconsistentWindingEdges", diagnostics.inconsistentWindingEdges.size()},
                {"degenerateTriangles", diagnostics.degenerateTriangles}, {"duplicateTriangles", diagnostics.duplicateTriangles}}}};
    };
    return {{"tolerance", tolerance}, {"aToB", surface(result.original)}, {"bToA", surface(result.repaired)}};
}
} // namespace woby
