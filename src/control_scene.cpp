#include "control_scene.h"
#include "uv_quality.h"
#include "analysis_results.h"
#include "comparison_scene.h"
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
    result.update({{"uvGrid", group.uvGrid.enabled}, {"uvDensityU", group.uvGrid.densityU}, {"uvDensityV", group.uvGrid.densityV}, {"uvColor", uvColorModeKey(group.uvGrid.mode)}, {"uvMinimum",group.uvGrid.minimum}, {"uvMaximum",group.uvGrid.maximum}});
    result.update({{"lineWidth", group.lines.width}, {"lineDepthTest", group.lines.depthTest}});
    return result;
}
Json boundsInfo(const Bounds& bounds, const Coordinate& origin = {})
{
    const auto original = [&](const auto& p) { return originalPosition({p[0],p[1],p[2]},origin); };
    return {{"min", original(bounds.min)}, {"max", original(bounds.max)}, {"center", original(bounds.center)}, {"radius", bounds.radius}};
}
Json originalBoundsInfo(const Mesh& mesh, float radius, const MeshNode* node = nullptr)
{
    const auto bounds = originalMeshBounds(mesh, node);
    Coordinate center{};
    for (size_t k = 0; k < 3; ++k) { center[k] = bounds[0][k]*.5 + bounds[1][k]*.5; }
    return {{"min", bounds[0]}, {"max", bounds[1]}, {"center", center}, {"radius", radius}};
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
                {"type", analysisTypeKey(settings.type)},
                {"uvSeparated",settings.uvSeparated}, {"uvLinkedSelection",settings.uvLinkedSelection},
                {"uvMetric",settings.uvMetric == UvQualityMetric::area ? "area" : settings.uvMetric == UvQualityMetric::orientation ? "orientation" : "angle"},
                {"uvNormalization",settings.uvNormalization == UvAreaNormalization::absolute ? "absolute" : "per_patch"},
                {"uvColor",uvColorModeKey(settings.uvGrid.mode)}, {"uvMinimum",settings.uvGrid.minimum}, {"uvMaximum",settings.uvGrid.maximum},
                {"uvView", settings.uvView == UvView::layout ? "layout" : "surface"},
                {"uvGrid", settings.uvGrid.enabled}, {"uvDensityU", settings.uvGrid.densityU}, {"uvDensityV", settings.uvGrid.densityV},
                {"mode", mode}, {"distanceOnA", settings.distanceOnOriginal}, {"tolerance", settings.tolerance},
                {"colorRange", settings.colorRange}, {"showEdges", settings.showEdges},
                {"nonManifoldVertices", settings.topologyInspection.nonManifoldVertices},
                {"showNonManifoldVertices", settings.topologyInspection.showNonManifoldVertices},
                {"fins", settings.topologyInspection.fins},
                {"showFins", settings.topologyInspection.showFins},
                {"finMaxAreaRatio", settings.topologyInspection.finMaxAreaRatio},
                {"holes", settings.topologyInspection.holes},
                {"showHoles", settings.topologyInspection.showHoles},
                {"holeSizeRatioTolerance", settings.topologyInspection.holeSizeRatioTolerance},
                {"autoUpdateBoundaries", settings.autoUpdateBoundaries}, {"autoUpdateNonManifold", settings.autoUpdateNonManifold}, {"autoUpdateWinding", settings.autoUpdateWinding},
                {"intersectionPairLimit", settings.intersections.limits.pairs}, {"intersectionCandidateLimit", settings.intersections.limits.candidateTests}, {"autoUpdateSelfIntersections", settings.intersections.autoUpdate}, {"selfIntersections", settings.intersections.autoUpdate}, {"showSelfIntersections", settings.intersections.show},
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
                {"lineSegmentCount", file.mesh.lineIndices.size() / 2},
                {"groupCount", file.groupSettings.size()}, {"localBounds", originalBoundsInfo(file.mesh, file.mesh.bounds.radius)}};
        }
        for (size_t index = 0; index < file.groupSettings.size(); ++index) {
            const auto& group = file.groupSettings[index];
            if (group.objectId == id) {
                return {{"settings", groupInfo(group)}, {"triangleCount", file.mesh.nodes.at(index).indexCount / 3},
                    {"primitive", file.mesh.nodes[index].lineIndexCount ? "lines" : "triangles"},
                    {"lineSegmentCount", file.mesh.nodes[index].lineIndexCount / 2},
                    {"hasTexcoords", file.mesh.nodes.at(index).hasTexcoords},
                    {"localBounds", group.localBoundsValid ? originalBoundsInfo(file.mesh, group.localBounds.radius, &file.mesh.nodes[index]) : Json(nullptr)}};
            }
        }
    }
    throw std::invalid_argument("Unknown or stale object ID.");
}

Json treeNode(const UiState& state, const UiSceneNode& node, const ObjectIdFormatter& formatId,
    const std::array<float, 16>& parent, bool parentVisible, float parentOpacity,
    std::vector<size_t> path, bool implicit = false, SceneObjectId parentId = invalidSceneObjectId)
{
    std::array<float, 16> local{}, world{};
    bool visible = true;
    float opacity = 1;
    Json result = {{"id", formatId(node.objectId)}, {"name", node.name}, {"occurrence", path}, {"implicit", implicit}};
    result["parentId"] = parentId == invalidSceneObjectId ? Json(nullptr) : Json(formatId(parentId));
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
            result["primitive"] = file.mesh.nodes.at(node.groupIndex).lineIndexCount ? "lines" : "triangles";
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
            parentVisible && visible, parentOpacity * opacity, std::move(childPath), implicit || implicitChildren, node.objectId));
    }
    return result;
}
void collectOccurrences(const Json& nodes, const std::string& id, Json& result)
{
    for (const auto& node : nodes) {
        if (node["id"] == id) {
            result.push_back({{"occurrence", node["occurrence"]}, {"parentId", node["parentId"]},
                {"implicit", node["implicit"]}, {"effective", node["effective"]}});
        }
        collectOccurrences(node["children"], id, result);
    }
}

struct Target {
    UiSceneNode* folder = nullptr;
    UiFileState* file = nullptr;
    UiGroupState* group = nullptr;
};
Target resolveTarget(UiState& state, SceneObjectId id)
{
    if (auto* folder = findFolder(state.sceneNodes, id)) { return {folder}; }
    for (auto& file : state.files) {
        if (file.objectId == id) { return {nullptr, &file}; }
        for (auto& group : file.groupSettings) {
            if (group.objectId == id) { return {nullptr, &file, &group}; }
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
    size_t vertices = 0, triangles = 0, lineSegments = 0;
    for (const auto& file : state.files) {
        vertices += file.mesh.vertices.size(); triangles += file.mesh.indices.size() / 3;
        lineSegments += file.mesh.lineIndices.size() / 2;
    }
    Json modes = Json::object();
    const size_t groups = totalGroupCount(state);
    modes["solid"] = modeCount(countEnabledSceneRenderMode(state, UiRenderMode::solidMesh), groups);
    modes["triangles"] = modeCount(countEnabledSceneRenderMode(state, UiRenderMode::triangles), groups);
    modes["vertices"] = modeCount(countEnabledSceneRenderMode(state, UiRenderMode::vertices), groups);
    return {{"dirty", state.isDirty}, {"fileCount", state.files.size()}, {"analysisCount", state.comparisons.size()}, {"annotationCount", state.annotations.size()}, {"groupCount", groups},
        {"visibleGroupCount", countVisibleSceneGroups(state)}, {"vertexCount", vertices}, {"triangleCount", triangles}, {"lineSegmentCount", lineSegments},
        {"showGrid", state.showGrid}, {"showDimensions", state.showDimensions},
        {"showOrigin", state.showOrigin}, {"upAxis", state.upAxis == SceneUpAxis::y ? "y" : "z"},
        {"coordinateOrigin", state.coordinateOrigin.value_or(Coordinate{})},
        {"masterVertexPointSize", state.masterVertexPointSize}, {"renderModes", modes}, {"bounds", boundsInfo(state.sceneBounds, state.coordinateOrigin.value_or(Coordinate{}))}};
}

Json controlCameraInfo(const UiState& state)
{
    const auto& camera = state.camera;
    const auto eye = cameraEye(camera, state.upAxis);
    const auto up = cameraUp(camera, state.upAxis);
    const auto depth = cameraDepthRange(camera, state.sceneBounds, state.upAxis);
    constexpr float radiansToDegrees = 57.29577951308232f;
    return {{"target", camera.target}, {"eye", {eye.x, eye.y, eye.z}}, {"up", {up.x, up.y, up.z}},
        {"yawDegrees", camera.yawRadians * radiansToDegrees}, {"pitchDegrees", camera.pitchRadians * radiansToDegrees},
        {"rollDegrees", camera.rollRadians * radiansToDegrees}, {"distance", camera.distance},
        {"verticalFovDegrees", camera.verticalFovDegrees}, {"nearPlane", camera.nearPlane},
        {"effectiveNearPlane", depth.nearPlane}, {"farPlane", depth.farPlane},
        {"upAxis", state.upAxis == SceneUpAxis::y ? "y" : "z"}};
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
            {"parentId", nullptr},
            {"occurrence", {result.size()}}, {"implicit", false}, {"children", Json::array()},
            {"settings", localObjectDetails(state, comparison.objectId)["settings"]},
            {"effective", {{"visible", comparison.settings.enabled && canInspectComparison(state, comparison.objectId)},
                {"opacity", 1}, {"worldMatrix", world}}}});
    }
    for (const auto& item : state.annotations) {
        const auto details = controlAnnotationDetails(state, item);
        result.push_back({{"id", formatId(item.objectId)}, {"name", item.settings.name}, {"kind", "annotation"},
            {"parentId", nullptr},
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
        auto sources = Json::array();
        const auto ids = item->targetIds.empty() ? std::vector<SceneObjectId>{item->targetId} : item->targetIds;
        for (const auto source : ids) { sources.push_back(findSceneObject(state, source) ? Json(formatId(source)) : Json(nullptr)); }
        result["sourceIds"] = std::move(sources);
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
        || command.action == A::comparisonSwap || command.action == A::comparisonEnable
        || command.action == A::comparisonRun || command.action == A::comparisonCancel) {
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
        if (command.action != A::comparisonCreate && isUvAnalysis(comparisonSettings(state, id).type)
            && (command.action == A::comparisonRun || command.action == A::comparisonCancel
                || command.action == A::comparisonSwap || command.side == "b")) {
            throw std::invalid_argument("UV analysis has one source input and no mesh detectors.");
        }
        if (command.action == A::comparisonRun || command.action == A::comparisonCancel) {
            if (command.action == A::comparisonRun && !canInspectComparison(state, id)) { throw std::invalid_argument("Analysis needs valid input before running a check."); }
            const auto detector = std::find(diagnosticCategoryKeys.begin(), diagnosticCategoryKeys.end(), command.detector.value_or(""));
            if (detector == diagnosticCategoryKeys.end()) { throw std::invalid_argument("Unknown detector."); }
            requestComparisonDetector(state, id, static_cast<DiagnosticCategory>(detector - diagnosticCategoryKeys.begin()), command.action == A::comparisonCancel);
            return {{"target", formatId(id)}, {"detector", *detector}, {"status", command.action == A::comparisonCancel ? "cancel_requested" : "queued"}};
        }
        if (command.action == A::comparisonCreate) {
            id = createComparison(state, command.type == "uv_quality" ? AnalysisType::uvQuality : command.type == "uv" ? AnalysisType::uv : AnalysisType::mesh);
            if (command.name) { renameComparison(state, id, *command.name); }
            if (command.a) { setComparisonObjects(state, {command.aId}, ComparisonSide::a, true, id); }
            if (command.b) { setComparisonObjects(state, {command.bId}, ComparisonSide::b, true, id); }
        } else if (command.action == A::comparisonDelete) {
            removeComparison(state, id);
            updateSceneDirty(state, cleanDocument);
            return {{"removed", command.target}, {"dirty", state.isDirty}};
        } else if (command.action == A::comparisonSet) {
            auto settings = comparisonSettings(state, id);
            if (isUvAnalysis(settings.type)) {
                const auto params = controlOperationParams(command);
                for (auto it = params.begin(); it != params.end(); ++it) {
                    if (it.key() != "target" && it.key() != "name" && it.key() != "visible" && it.key() != "showEdges"
                        && it.key() != "uvSeparated" && it.key() != "uvLinkedSelection" && it.key() != "uvColor" && it.key() != "uvMinimum" && it.key() != "uvMaximum" && it.key() != "uvMetric" && it.key() != "uvNormalization"
                        && it.key() != "uvView" && it.key() != "uvGrid" && it.key() != "uvDensityU" && it.key() != "uvDensityV") {
                        throw std::invalid_argument("UV analysis supports UV display, quality and source controls only.");
                    }
                }
            } else if (command.uvSeparated || command.uvLinkedSelection || command.uvMetric || command.uvNormalization || command.uvColor || command.uvMinimum || command.uvMaximum || command.uvView || command.uvGrid || command.uvDensityU || command.uvDensityV) {
                throw std::invalid_argument("UV controls require --type uv or --type uv_quality.");
            }
            if ((command.uvMetric || command.uvNormalization) && settings.type != AnalysisType::uvQuality) { throw std::invalid_argument("UV quality settings require --type uv_quality."); }
            if (settings.type == AnalysisType::uvQuality && (command.uvColor || command.uvMinimum || command.uvMaximum || command.uvGrid || command.uvDensityU || command.uvDensityV)) { throw std::invalid_argument("UV quality uses metric heatmaps; use --type uv for grid and parameter colors."); }
            if (command.uvSeparated) { settings.uvSeparated = *command.uvSeparated; }
            if (command.uvLinkedSelection) { settings.uvLinkedSelection = *command.uvLinkedSelection; }
            if (command.uvMetric) { settings.uvMetric = *command.uvMetric == "area" ? UvQualityMetric::area : *command.uvMetric == "orientation" ? UvQualityMetric::orientation : UvQualityMetric::angle; }
            if (command.uvNormalization) { settings.uvNormalization = *command.uvNormalization == "absolute" ? UvAreaNormalization::absolute : UvAreaNormalization::perPatch; }
            if (command.uvColor) { settings.uvGrid.mode = *command.uvColor == "u" ? UvColorMode::u : *command.uvColor == "v" ? UvColorMode::v : UvColorMode::grid; }
            if (command.uvMinimum) { settings.uvGrid.minimum = *command.uvMinimum; }
            if (command.uvMaximum) { settings.uvGrid.maximum = *command.uvMaximum; }
            if (settings.uvGrid.maximum <= settings.uvGrid.minimum) { throw std::invalid_argument("UV maximum must exceed minimum."); }
            if (command.uvView) { settings.uvView = *command.uvView == "layout" ? UvView::layout : UvView::surface; }
            if (command.uvGrid) { settings.uvGrid.enabled = *command.uvGrid; }
            if (command.uvDensityU) { settings.uvGrid.densityU = *command.uvDensityU; }
            if (command.uvDensityV) { settings.uvGrid.densityV = *command.uvDensityV; }
            if (command.visible) { settings.enabled = *command.visible; }
            if (command.mode) {
                settings.mode = *command.mode == "distance" ? ComparisonMode::distance : *command.mode == "a"
                    ? ComparisonMode::original : *command.mode == "b" ? ComparisonMode::repaired :
                    *command.mode == "surface_quality" ? ComparisonMode::surfaceQuality : ComparisonMode::overlay;
            }
            if (command.nonManifoldVertices) { settings.topologyInspection.nonManifoldVertices = *command.nonManifoldVertices; }
            if (command.showNonManifoldVertices) { settings.topologyInspection.showNonManifoldVertices = *command.showNonManifoldVertices; }
            if (command.fins) { settings.topologyInspection.fins = *command.fins; }
            if (command.showFins) { settings.topologyInspection.showFins = *command.showFins; }
            if (command.finMaxAreaRatio) { settings.topologyInspection.finMaxAreaRatio = *command.finMaxAreaRatio; }
            if (command.holes) { settings.topologyInspection.holes = *command.holes; }
            if (command.showHoles) { settings.topologyInspection.showHoles = *command.showHoles; }
            if (command.holeSizeRatioTolerance) { settings.topologyInspection.holeSizeRatioTolerance = *command.holeSizeRatioTolerance; }
            if (command.autoUpdateBoundaries) { settings.autoUpdateBoundaries = *command.autoUpdateBoundaries; }
            if (command.autoUpdateNonManifold) { settings.autoUpdateNonManifold = *command.autoUpdateNonManifold; }
            if (command.autoUpdateWinding) { settings.autoUpdateWinding = *command.autoUpdateWinding; }
            if (command.selfIntersections) { settings.intersections.autoUpdate = *command.selfIntersections; }
            if (command.autoUpdateSelfIntersections) { settings.intersections.autoUpdate = *command.autoUpdateSelfIntersections; }
            if (command.intersectionPairLimit) { settings.intersections.limits.pairs = static_cast<size_t>(*command.intersectionPairLimit); }
            if (command.intersectionCandidateLimit) { settings.intersections.limits.candidateTests = static_cast<size_t>(*command.intersectionCandidateLimit); }
            if (command.showSelfIntersections) { settings.intersections.show = *command.showSelfIntersections; }
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
                if (command.isolate.value_or(false)) {
                    const auto members = comparisonMemberIds(state,side,id,false);
                    const auto candidates = comparisonObjectParts(state,{command.memberId});
                    if (std::none_of(candidates.begin(),candidates.end(),[&](auto p) { return std::binary_search(members.begin(),members.end(),p); })) { throw std::invalid_argument("Object is not a member of the selected analysis side."); }
                    isolateComparisonObjects(state,{command.memberId},side,id);
                } else setComparisonObjectsEnabled(state, command.object ? std::vector<SceneObjectId>{command.memberId}
                    : std::vector<SceneObjectId>{}, side, *command.enabled, id);
            }
            else { setComparisonObjects(state, {command.memberId}, side, command.action == A::comparisonAdd, id); }
        }
        updateSceneDirty(state, cleanDocument);
        auto object = controlObjectDetails(state, id, formatId);
        object.update({{"id", formatId(id)}, {"name", findComparison(state, id)->name}, {"kind", "analysis"}});
        return {{"target", formatId(id)}, {"object", std::move(object)}, {"dirty", state.isDirty}, {"bounds", boundsInfo(state.sceneBounds, state.coordinateOrigin.value_or(Coordinate{}))}};
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
            {"dirty", state.isDirty}, {"bounds", boundsInfo(state.sceneBounds, state.coordinateOrigin.value_or(Coordinate{}))}};
    }
    Target target;
    if (command.objectId != invalidSceneObjectId) { target = resolveTarget(state, command.objectId); }
    const bool scene = command.target == "scene";
    switch (command.action) {
    case A::sceneInfo: case A::stats: return controlSceneInfo(state);
    case A::sceneTree: return {{"nodes", controlSceneTree(state, formatId)}};
    case A::sceneBounds: return {{"bounds", boundsInfo(state.sceneBounds, state.coordinateOrigin.value_or(Coordinate{}))}, {"visibleOnly", true}, {"emptyFallback", boundsInfo(defaultDisplayBounds())}};
    case A::transformGet: return {{"target", command.target}, {"settings", localObjectDetails(state, command.objectId)["settings"]}};
    case A::visibility:
        if (scene) { setAllSceneVisible(state, *command.visible); }
        else if (target.group) { setGroupVisible(state, *target.file, *target.group, *command.visible); }
        else if (target.file) { setFileVisible(*target.file, *command.visible); refreshSceneTreeFolderVisibility(state); }
        else { setSceneNodeSubtreeVisible(state, *target.folder, *command.visible); }
        break;
    case A::render:
        if ((command.lineWidth || command.lineDepthTest)
            && !setObjectLineStyle(state, scene ? std::vector<SceneObjectId>{} : std::vector<SceneObjectId>{command.objectId}, {}, {})) {
            throw std::invalid_argument("Line style requires an imported line group.");
        }
        // Validate UV eligibility before changing any of the other render flags.
        if (command.uvColor || command.uvMinimum || command.uvMaximum || command.uvGrid || command.uvDensityU || command.uvDensityV) {
            if (!setObjectUvGrid(state, scene ? std::vector<SceneObjectId>{} : std::vector<SceneObjectId>{command.objectId},
                    command.uvGrid, command.uvDensityU, command.uvDensityV,
                    command.uvColor ? std::optional<UvColorMode>(*command.uvColor == "u" ? UvColorMode::u : *command.uvColor == "v" ? UvColorMode::v : UvColorMode::grid) : std::nullopt,
                    command.uvMinimum, command.uvMaximum)) {
                throw std::invalid_argument("UV coloring requires complete UV coordinates and a maximum greater than minimum.");
            }
        }
        if (command.lineWidth || command.lineDepthTest) {
            setObjectLineStyle(state, scene ? std::vector<SceneObjectId>{} : std::vector<SceneObjectId>{command.objectId},
                command.lineWidth, command.lineDepthTest);
        }
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
        if (!setObjectColor(state, {command.objectId}, command.action == A::colorSet ? command.rgb : std::nullopt)) {
            throw std::invalid_argument("Color target has no mesh or line groups.");
        }
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
        } else { fitCameraToScene(state); }
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
    case A::comparisonFindings: case A::comparisonExport: case A::comparisonExportStatus: case A::comparisonExportCancel:
    case A::comparisonResults: case A::comparisonFocus: case A::sceneUndo: case A::sceneRedo:
        throw std::invalid_argument("Command requires a runtime adapter.");
    case A::comparisonCreate: case A::comparisonDelete: case A::comparisonSet: case A::comparisonAdd:
    case A::comparisonRemove: case A::comparisonClear: case A::comparisonSwap: case A::comparisonEnable: case A::comparisonRun: case A::comparisonCancel:
        throw std::invalid_argument("Analysis command was not dispatched.");
    }
    // Read-only and session-only operations return above. Struct-level setters
    // in this batch need one scene-edit notification for history recording.
    markSceneDirty(state);
    recalculateSceneBounds(state);
    updateSceneDirty(state, cleanDocument);
    if (command.objectId != invalidSceneObjectId) {
        return {{"target", command.target}, {"applied", localObjectDetails(state, command.objectId)["settings"]},
            {"dirty", state.isDirty}, {"bounds", boundsInfo(state.sceneBounds, state.coordinateOrigin.value_or(Coordinate{}))}};
    }
    return controlSceneInfo(state);
}

Json controlFocusComparisonDiagnostic(UiState& state, const MeshComparison& result,
    uint64_t resultSignature, const ControlOperation& command)
{
    if (command.action != ControlAction::comparisonFocus || !command.side || !command.detector || !command.index) {
        throw std::invalid_argument("Expected an analysis.focus command.");
    }
    const auto* comparison = findComparison(state, command.objectId);
    if (!comparison || !comparison->settings.enabled) {
        throw std::invalid_argument("analysis.focus requires a visible analysis ID.");
    }
    if (resultSignature == 0 || resultSignature != comparisonGeometrySignature(state, command.objectId)) {
        throw std::invalid_argument("Analysis findings are out of date.");
    }
    const auto detector = std::find(diagnosticCategoryKeys.begin(), diagnosticCategoryKeys.end(), *command.detector);
    if (detector == diagnosticCategoryKeys.end()) { throw std::invalid_argument("Unknown detector."); }
    const auto category = static_cast<DiagnosticCategory>(detector - diagnosticCategoryKeys.begin());
    const auto side = *command.side == "a" ? ComparisonSide::a : ComparisonSide::b;
    if (comparisonDetectorStatus(result, category).phase != IntersectionPhase::complete) {
        throw std::invalid_argument("Detector findings are not current; run the detector first.");
    }
    const auto& findings = comparisonDiagnosticEdges(result, side, category);
    if (*command.index == 0 || *command.index > findings.size()) {
        throw std::invalid_argument("Finding index is outside the selected detector's results.");
    }
    auto settings = comparison->settings;
    if (settings.diagnosticSide != side || settings.diagnosticCategory != category) {
        settings.diagnosticSide = side;
        settings.diagnosticCategory = category;
        setComparisonSettings(state, settings, command.objectId);
    }
    selectComparisonDiagnostic(state, result, resultSignature,
        static_cast<size_t>(*command.index - 1), command.objectId);
    const auto* focused = findComparison(state, command.objectId);
    if (!focused->diagnosticFocus) { throw std::invalid_argument("Detector findings are out of date."); }
    return {{"target", command.target}, {"side", *command.side}, {"detector", *command.detector},
        {"index", focused->diagnosticFocus->index + 1}, {"count", findings.size()},
        {"camera", controlCameraInfo(state)}};
}

Json controlComparisonResults(const MeshComparison& result, double tolerance, bool includeDetectors)
{
    const auto surface = [&result, tolerance, includeDetectors](const SurfaceComparison& value) {
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
            {"detectors", [&] {
                Json detectors = {{"schemaVersion", 1}, {"idBase", 1}, {"scope", "per-source; selected parts, including unused points when whole file selected"}};
                if (includeDetectors) {
                    const auto side = &value == &result.original ? ComparisonSide::a : ComparisonSide::b;
                    for (const auto* key : diagnosticCategoryKeys) { detectors[key] = analysisDetectorSummary(result, side, key); }
                }
                return detectors;
            }()},
            {"surfaceMeshQuality", quality}, {"sampleCount", value.distances.size()}, {"triangleCount", value.source.indices.size() / 3},
            {"diagnostics", {{"boundaryEdges", diagnostics.boundaryEdges.size()},
                {"nonManifoldEdges", diagnostics.nonManifoldEdges.size()},
                {"inconsistentWindingEdges", diagnostics.inconsistentWindingEdges.size()},
                {"degenerateTriangles", diagnostics.degenerateTriangles}, {"duplicateTriangles", diagnostics.duplicateTriangles}}}};
    };
    auto a = surface(result.original), b = surface(result.repaired);
    Json answer = {{"tolerance", tolerance}, {"aToB", std::move(a)}, {"bToA", std::move(b)}};
    if (result.original.source.uvQuality) {
        const auto& q = *result.original.source.uvQuality;
        Json findings = Json::array();
        size_t count = 0, valid = 0;
        double angleMax = 0, areaMin = 0, areaMax = 0;
        for (const auto& t : q.triangles) {
            if (!t.missing && !t.collapsed && !t.degenerateSurface) {
                angleMax = std::max(angleMax,t.angleDegrees);
                if (valid++ == 0) { areaMin = areaMax = t.areaLog2; }
                areaMin = std::min(areaMin,t.areaLog2); areaMax = std::max(areaMax,t.areaLog2);
            }
            if (!t.collapsed && !t.degenerateSurface && !t.mixedOrientation) { continue; }
            if (++count > 1000) { continue; }
            findings.push_back({{"sourcePartId",t.partId}, {"triangle",t.triangle}, {"collapsedUv",t.collapsed},
                {"degenerateSurface",t.degenerateSurface}, {"mixedOrientation",t.mixedOrientation}, {"orientation",t.orientation}});
        }
        answer["uvQuality"] = {{"normalization",q.normalization == UvAreaNormalization::perPatch ? "per_patch" : "absolute"},
            {"collapsedTriangles",q.collapsed}, {"missingUvTriangles",q.missing}, {"degenerateSurfaceTriangles",q.degenerateSurface},
            {"mixedOrientationPatches",q.mixedOrientationPatches}, {"validTriangles",valid},
            {"maximumAngleDegrees",valid ? Json(angleMax) : Json(nullptr)},
            {"minimumAreaLog2",valid ? Json(areaMin) : Json(nullptr)}, {"maximumAreaLog2",valid ? Json(areaMax) : Json(nullptr)},
            {"findings",std::move(findings)}, {"findingCount",count}, {"findingsTruncated",count > 1000}};
    }
    return answer;

}
} // namespace woby
