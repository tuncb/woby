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
    if (id != invalidSceneObjectId) {
        if (const auto* comparison = findComparison(state, id)) {
            const auto& settings = comparison->settings;
            const char* mode = settings.mode == ComparisonMode::distance ? "distance"
                : settings.mode == ComparisonMode::original ? "a" : settings.mode == ComparisonMode::repaired ? "b" : "overlay";
            return {{"settings", {{"visible", settings.enabled}, {"translation", comparison->translation},
                {"mode", mode}, {"distanceOnA", settings.distanceOnOriginal}, {"tolerance", settings.tolerance},
                {"colorRange", settings.colorRange}, {"showEdges", settings.showEdges},
                {"showBoundaries", settings.showBoundaries}, {"showNonManifold", settings.showNonManifold}}},
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
    return {{"dirty", state.isDirty}, {"fileCount", state.files.size()}, {"comparisonCount", state.comparisons.size()}, {"groupCount", groups},
        {"visibleGroupCount", countVisibleSceneGroups(state)}, {"vertexCount", vertices}, {"triangleCount", triangles},
        {"showGrid", state.showGrid}, {"showOrigin", state.showOrigin}, {"upAxis", state.upAxis == SceneUpAxis::y ? "y" : "z"},
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
        result.push_back({{"id", formatId(comparison.objectId)}, {"name", comparison.name}, {"kind", "comparison"},
            {"occurrence", {result.size()}}, {"implicit", false}, {"children", Json::array()},
            {"settings", localObjectDetails(state, comparison.objectId)["settings"]},
            {"effective", {{"visible", comparison.settings.enabled && canInspectComparison(state, comparison.objectId)},
                {"opacity", 1}, {"worldMatrix", world}}}});
    }
    return result;
}

Json controlObjectDetails(const UiState& state, SceneObjectId id, const ObjectIdFormatter& formatId)
{
    auto result = localObjectDetails(state, id);
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
    if (command.action == A::comparisonCreate || command.action == A::comparisonDelete
        || command.action == A::comparisonSet || command.action == A::comparisonAdd
        || command.action == A::comparisonRemove || command.action == A::comparisonClear
        || command.action == A::comparisonSwap || command.action == A::comparisonEnable) {
        // Validate all inputs before any mutation, including creation of an empty object.
        if (command.action != A::comparisonCreate
            && (command.objectId == invalidSceneObjectId || !findComparison(state, command.objectId))) {
            throw std::invalid_argument("This command requires a comparison ID.");
        }
        for (const auto& [supplied, id] : {std::pair{command.a.has_value(), command.aId},
            std::pair{command.b.has_value(), command.bId}, std::pair{command.object.has_value(), command.memberId}}) {
            if (supplied && comparisonObjectParts(state, {id}).empty()) {
                throw std::invalid_argument("Comparison inputs require a file, folder, or group containing triangles.");
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
                    ? ComparisonMode::original : *command.mode == "b" ? ComparisonMode::repaired : ComparisonMode::overlay;
            }
            if (command.distanceOnA) { settings.distanceOnOriginal = *command.distanceOnA; }
            if (command.tolerance) { settings.tolerance = *command.tolerance; }
            if (command.colorRange) { settings.colorRange = *command.colorRange; }
            if (command.showEdges) { settings.showEdges = *command.showEdges; }
            if (command.showBoundaries) { settings.showBoundaries = *command.showBoundaries; }
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
        object.update({{"id", formatId(id)}, {"name", findComparison(state, id)->name}, {"kind", "comparison"}});
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
                throw std::invalid_argument("Comparison transforms support display translation only.");
            }
            if (command.action == A::transformReset || command.translation) {
                setComparisonTranslation(state, command.objectId, command.translation.value_or(std::array<float, 3>{}));
            }
        } else {
            throw std::invalid_argument("This command does not apply to comparison objects.");
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
    case A::origin: setShowOrigin(state, *command.visible); break;
    case A::upAxis: setSceneUpAxis(state, command.axis == "y" ? SceneUpAxis::y : SceneUpAxis::z); break;
    case A::cameraFrame: frameCameraToScene(state); return {{"camera", controlCameraInfo(state)}};
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
    case A::comparisonResults: case A::sceneUndo: case A::sceneRedo:
        throw std::invalid_argument("Command requires a runtime adapter.");
    case A::comparisonCreate: case A::comparisonDelete: case A::comparisonSet: case A::comparisonAdd:
    case A::comparisonRemove: case A::comparisonClear: case A::comparisonSwap: case A::comparisonEnable:
        throw std::invalid_argument("Comparison command was not dispatched.");
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

Json controlComparisonResults(const MeshComparison& result, double tolerance)
{
    const auto surface = [tolerance](const SurfaceComparison& value) {
        const auto& diagnostics = value.diagnostics;
        if (value.source.indices.empty()) { return Json(nullptr); }
        const bool measured = !value.distances.empty();
        return Json{{"maximum", measured ? Json(value.maximum) : Json(nullptr)},
            {"mean", measured ? Json(value.mean) : Json(nullptr)},
            {"percentile95", measured ? Json(value.percentile95) : Json(nullptr)},
            {"percentAboveTolerance", measured ? Json(surfacePercentAboveTolerance(value, tolerance)) : Json(nullptr)},
            {"sampleCount", value.distances.size()}, {"triangleCount", value.source.indices.size() / 3},
            {"diagnostics", {{"boundaryEdges", diagnostics.boundaryEdges.size()},
                {"nonManifoldEdges", diagnostics.nonManifoldEdges.size()},
                {"inconsistentWindingEdges", diagnostics.inconsistentWindingEdges.size()},
                {"degenerateTriangles", diagnostics.degenerateTriangles}, {"duplicateTriangles", diagnostics.duplicateTriangles}}}};
    };
    return {{"tolerance", tolerance}, {"aToB", surface(result.original)}, {"bToA", surface(result.repaired)}};
}
} // namespace woby
