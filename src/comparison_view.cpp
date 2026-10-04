#include "uv_quality_view.h"
#include "marker_pick.h"
#include "comparison_view.h"
#include "analysis_presentation.h"
#include "comparison_scene.h"
#include "comparison_legend.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "utf8_path.h"
#include "uv_quality.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <climits>
#include <limits>
#include <stdexcept>

namespace woby
{
namespace
{
constexpr size_t diagnosticPageSize = 25;

enum class ComparisonActivity { idle, queued, calculating, failed };

const ComparisonInspectorCache& inspectorQueries(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id)
{
    return updateComparisonInspectorCache(runtime.inspector, state, id);
}

bool inspectorStagesReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id, uint32_t stages)
{
    return comparisonStagesReady(runtime, comparisonSettings(state, id), inspectorQueries(runtime, state, id).signature, stages);
}

bool inspectorDetectorReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id, DiagnosticCategory category)
{
    return comparisonDetectorStatus(runtime.result, category).phase == IntersectionPhase::complete
        && inspectorStagesReady(runtime, state, id, comparisonDiagnosticStage(category));
}

size_t inspectorEnabledPartCount(const ComparisonRuntime& runtime, const UiState& state, ComparisonSide side, SceneObjectId id)
{
    return inspectorQueries(runtime, state, id).inputs[side == ComparisonSide::a ? 0 : 1].summary.enabledPartCount;
}

bool resultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    uint64_t signature, bool both, bool fullResults);

ComparisonActivity comparisonActivity(const UiState& state, const ComparisonRuntime& runtime, SceneObjectId id)
{
    const auto& queries = inspectorQueries(runtime, state, id);
    const auto signature = queries.signature;
    const bool both = queries.inputs[0].summary.enabledPartCount && queries.inputs[1].summary.enabledPartCount;
    if (!signature || !comparisonSettings(state, id).enabled
        || resultsReady(runtime, state, id, signature, both, false)) { return ComparisonActivity::idle; }
    if (!runtime.error.empty() && runtime.attemptedSignature == signature) { return ComparisonActivity::failed; }
    return ((runtime.preparationWorker.valid() && runtime.preparationSignature == signature && !runtime.preparationStop.stop_requested())
        || (runtime.worker.valid() && runtime.workerSignature == signature && !runtime.stop.stop_requested()))
        ? ComparisonActivity::calculating : ComparisonActivity::queued;
}

void drawCalculationSpinner()
{
    const float size = ImGui::GetTextLineHeight();
    const auto position = ImGui::GetCursorScreenPos();
    const float radius = size * 0.35f;
    const float angle = static_cast<float>(std::fmod(ImGui::GetTime() * 5.0, 6.283185307));
    auto* draw = ImGui::GetWindowDrawList();
    draw->PathArcTo({position.x + size * 0.5f, position.y + size * 0.5f}, radius,
        angle, angle + 4.7f, 24);
    draw->PathStroke(ImGui::GetColorU32(ImGuiCol_Text), 0, std::max(1.0f, size * 0.1f));
    ImGui::Dummy({size, size});
}

void drawComparisonActivity(const UiState& state, ComparisonRuntime& runtime, SceneObjectId id)
{
    const auto activity = comparisonActivity(state, runtime, id);
    if (activity == ComparisonActivity::idle) { return; }
    if (ImGui::BeginChild("comparison_activity", {0, ImGui::GetFrameHeight()}, ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (activity == ComparisonActivity::calculating || activity == ComparisonActivity::queued) {
            ImGui::AlignTextToFramePadding();
            drawCalculationSpinner();
            ImGui::SameLine();
            ImGui::TextUnformatted(activity == ComparisonActivity::calculating ? "Calculating analysis..." : "Queued...");
        } else if (activity == ComparisonActivity::failed) {
            if (ImGui::Button("Retry")) {
                runtime.attemptedSignature = 0;
                runtime.failedStages = 0;
                runtime.error.clear();
            }
            setLastItemTooltip("Run this analysis again after the previous attempt failed.");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1, .65f, .25f, 1), "Analysis failed");
            setLastItemTooltip(runtime.error.c_str());
        }
    }
    ImGui::EndChild();
}

void destroySurface(ComparisonGpuSurface &gpu)
{
    for (const auto handle : {gpu.vertices, gpu.samples, gpu.quality, gpu.boundaries, gpu.nonManifold, gpu.winding,
        gpu.nonManifoldVertices, gpu.holes, gpu.finEdges, gpu.finFill, gpu.duplicatePoints, gpu.duplicateTriangleEdges, gpu.duplicateTriangleFill, gpu.degenerateEdges, gpu.degenerateFill, gpu.intersectionEdges, gpu.intersectionFill})
    {
        if (woby::graphics::isValid(handle))
        {
            woby::graphics::destroy(handle);
        }
    }
    for (const auto handle : {gpu.triangles, gpu.lines})
    {
        if (woby::graphics::isValid(handle))
        {
            woby::graphics::destroy(handle);
        }
    }
    gpu = {};
}
woby::graphics::VertexBufferHandle uploadEdges(const std::vector<DiagnosticEdge> &edges)
{
    if (edges.empty())
    {
        return WOBY_GPU_INVALID_HANDLE;
    }
    const auto bytes = comparisonBufferBytes(edges.size(), 2 * sizeof(std::array<float, 3>));
    std::vector<std::array<float, 3>> points;
    points.reserve(edges.size() * 2);
    for (const auto &edge : edges)
    {
        points.push_back(edge.a);
        points.push_back(edge.b);
    }
    const auto handle = woby::graphics::createVertexBuffer(
        woby::graphics::copy(points.data(), bytes), helperLineVertexLayout());
    if (!woby::graphics::isValid(handle))
    {
        throw std::runtime_error("Cannot allocate analysis edge buffer.");
    }
    return handle;
}
woby::graphics::VertexBufferHandle uploadPositions(const std::vector<std::array<float, 3>>& positions)
{
    if (positions.empty()) { return WOBY_GPU_INVALID_HANDLE; }
    const auto handle = woby::graphics::createVertexBuffer(woby::graphics::copy(positions.data(), comparisonBufferBytes(positions.size(), sizeof(positions[0]))), helperLineVertexLayout());
    if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Cannot allocate duplicate overlay buffer."); }
    return handle;
}
void appendCross(std::vector<std::array<float, 3>>& lines, const std::array<float, 3>& point, float radius)
{
    for (size_t axis = 0; axis < 3; ++axis) {
        auto a = point, b = point; a[axis] -= radius; b[axis] += radius;
        lines.push_back(a); lines.push_back(b);
    }
}
void uploadDuplicateOverlays(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages)
{
    if (stages & comparisonDuplicatePoints) {
        std::vector<std::array<float, 3>> points;
        for (const auto& finding : surface.duplicates.points.findings) {
            for (const auto& point : finding.geometry) { appendCross(points, point, surface.source.bounds.radius*.008f); }
        }
        gpu.duplicatePoints = uploadPositions(points);
    }
    if (stages & comparisonDuplicateTriangles) {
        std::vector<std::array<float, 3>> edges, fill;
        for (const auto& finding : surface.duplicates.triangles.findings) {
            fill.insert(fill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t i = 0; i < finding.geometry.size(); i += 3) {
                for (size_t k = 0; k < 3; ++k) { edges.push_back(finding.geometry[i+k]); edges.push_back(finding.geometry[i+(k+1)%3]); }
            }
        }
        gpu.duplicateTriangleEdges = uploadPositions(edges);
        gpu.duplicateTriangleFill = uploadPositions(fill);
    }
}

void uploadSurface(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages,
    const PreparedComparisonSource* prepared = nullptr)
{
    if (surface.source.indices.empty()) { return; }
    if (stages & comparisonSource) {
        const auto vertexBytes = comparisonBufferBytes(surface.source.vertices.size(), sizeof(Vertex));
        const auto indexBytes = comparisonBufferBytes(surface.source.indices.size(), sizeof(uint32_t));
        const auto lineBytes = comparisonBufferBytes(surface.source.indices.size(), 2 * sizeof(uint32_t));
        if (surface.source.uvQuality) {
            const auto qualityBytes = comparisonBufferBytes(surface.source.indices.size(),sizeof(Vertex));
            const auto values = prepared ? std::vector<Vertex>{} : uvQualityVertices(surface.source);
            gpu.quality = woby::graphics::createVertexBuffer(woby::graphics::copy(prepared ? prepared->quality.data() : values.data(), qualityBytes),meshVertexLayout());
            if (!woby::graphics::isValid(gpu.quality)) { throw std::runtime_error("Cannot allocate UV quality buffer."); }
        }
        auto vertices = prepared ? std::vector<Vertex>{} : surface.source.vertices;
        if (!prepared) { generateSmoothNormals(vertices, surface.source.indices); }
        gpu.vertices = woby::graphics::createVertexBuffer(woby::graphics::copy(prepared ? surface.source.vertices.data() : vertices.data(), vertexBytes), meshVertexLayout());
        const auto& indices = surface.source.indices;
        gpu.triangles = woby::graphics::createIndexBuffer(woby::graphics::copy(indices.data(), indexBytes), WOBY_GPU_BUFFER_INDEX32);
        std::vector<uint32_t> lines;
        if (!prepared) { lines.reserve(indices.size() * 2); }
        for (size_t i = 0; !prepared && i < indices.size(); i += 3) {
            for (size_t k = 0; k < 3; ++k) {
                lines.push_back(indices[i + k]);
                lines.push_back(indices[i + (k + 1) % 3]);
            }
        }
        gpu.lines = woby::graphics::createIndexBuffer(woby::graphics::copy(prepared ? prepared->lines.data() : lines.data(), lineBytes), WOBY_GPU_BUFFER_INDEX32);
        if (!woby::graphics::isValid(gpu.vertices) || !woby::graphics::isValid(gpu.triangles) || !woby::graphics::isValid(gpu.lines)) {
            throw std::runtime_error("Cannot allocate analysis surface buffers.");
        }
    }
    if ((stages & comparisonDistance) && !surface.sampled.vertices.empty()) {
        const auto& samples = surface.sampled.vertices;
        gpu.samples = woby::graphics::createVertexBuffer(woby::graphics::copy(samples.data(), comparisonBufferBytes(samples.size(), sizeof(Vertex))), meshVertexLayout());
        if (!woby::graphics::isValid(gpu.samples)) { throw std::runtime_error("Cannot allocate analysis distance buffer."); }
    }
    if (stages & comparisonTopology) {
        gpu.boundaries = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.boundaryEdges : surface.topologyBoundaries);
        gpu.nonManifold = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.nonManifoldEdges : surface.topologyNonManifold);
        gpu.nonManifoldVertices = uploadEdges(surface.nonManifoldVertexMarkers);
        gpu.holes = uploadEdges(surface.holeEdges);
        gpu.finEdges = uploadEdges(surface.finEdges);
        gpu.finFill = uploadPositions(surface.finFill);
        gpu.winding = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.inconsistentWindingEdges : surface.topologyWinding);
    }
    if (stages & comparisonDegenerates) {
        std::vector<std::array<float, 3>> edges, fill;
        for (const auto& finding : surface.degenerates.findings) {
            fill.insert(fill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t k = 0; k < 3; ++k) { edges.push_back(finding.geometry[k]); edges.push_back(finding.geometry[(k+1)%3]); }
            if (finding.reasons.collapsed) { appendCross(edges, finding.geometry[0], std::max(.00001f, surface.source.bounds.radius*.008f)); }
        }
        gpu.degenerateEdges = uploadPositions(edges);
        gpu.degenerateFill = uploadPositions(fill);
    }
    if (stages & comparisonIntersections) {
        std::vector<std::array<float, 3>> edges, fill;
        for (const auto& finding : surface.intersections.findings) {
            fill.insert(fill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t i = 0; i < 6; i += 3) {
                for (size_t k = 0; k < 3; ++k) { edges.push_back(finding.geometry[i+k]); edges.push_back(finding.geometry[i+(k+1)%3]); }
            }
        }
        gpu.intersectionEdges = uploadPositions(edges);
        gpu.intersectionFill = uploadPositions(fill);
    }
    uploadDuplicateOverlays(gpu, surface, stages);
}
bool originalActive(const ComparisonSettings &settings)
{
    return settings.mode == ComparisonMode::original ||
           (settings.mode == ComparisonMode::distance && settings.distanceOnOriginal) ||
           (settings.mode == ComparisonMode::surfaceQuality && settings.quality.onOriginal);
}
void uploadQuality(ComparisonGpuSurface& gpu, const SurfaceComparison& surface,
    SurfaceQualityMetric metric, const QualityDistribution& distribution)
{
    if (surface.source.indices.empty()) { return; }
    const auto bytes = comparisonBufferBytes(surface.source.indices.size(), sizeof(Vertex));
    const auto vertices = surfaceQualityVertices(surface.source, surface.quality, metric, distribution);
    const auto handle = woby::graphics::createVertexBuffer(woby::graphics::copy(vertices.data(), bytes), meshVertexLayout());
    if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Cannot allocate surface mesh quality buffer."); }
    if (woby::graphics::isValid(gpu.quality)) { woby::graphics::destroy(gpu.quality); }
    gpu.quality = handle;
}
void comparisonEnabledCheckbox(UiState& state, ComparisonSide side, SceneObjectId id,
    const std::vector<SceneObjectId>& objects, size_t total, size_t checked)
{
    bool enabled = total != 0 && checked == total;
    ImGui::BeginDisabled(total == 0);
    ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, checked != 0 && checked != total);
    if (ImGui::Checkbox("##enabled", &enabled)) {
        setComparisonObjectsEnabled(state, objects, side, enabled, id);
    }
    ImGui::PopItemFlag();
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Include in analysis (toggles all children)"); }
    ImGui::EndDisabled();
    ImGui::SameLine();
}

void drawComparisonTreeNode(UiState& state, ComparisonSide side, const ComparisonTreeNode& node, SceneObjectId id)
{
    const auto revision = state.sceneEditRevision;
    const auto nodeId = std::to_string(node.objectId);
    ImGui::PushID(nodeId.c_str());
    comparisonEnabledCheckbox(state, side, id, {node.objectId}, node.partCount, node.enabledPartCount);
    const bool leaf = node.children.empty();
    const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding
        | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick
        | (leaf ? ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen : ImGuiTreeNodeFlags_DefaultOpen);
    const bool open = ImGui::TreeNodeEx("node", flags, "%s", node.name.c_str());
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("%zu parts, %zu triangles", node.partCount, node.triangleCount);
        if (const auto object = findSceneObject(state, node.objectId); object && !object->path.empty()) {
            ImGui::TextUnformatted(pathToUtf8(object->path).c_str());
        }
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopupContextItem("membership")) {
        if (isUvAnalysis(comparisonSettings(state,id).type)) {
            if (ImGui::MenuItem("Select source patch")) { selectSceneObject(state,node.objectId); }
            if (ImGui::MenuItem("Isolate patch in analysis")) { isolateUvObjects(state,{node.objectId},id); }
            if (ImGui::MenuItem("Show all patches")) { isolateUvObjects(state,{},id); }
        } else {
            if (ImGui::MenuItem("Enable only this object")) { isolateComparisonObjects(state, {node.objectId}, side, id); }
            if (ImGui::MenuItem(isSingleSourceMeshTask(comparisonTask(state, id)) ? "Enable all sources" : "Enable all on this side")) {
                setComparisonObjectsEnabled(state, {}, side, true, id);
            }
        }
        const bool sources = isUvAnalysis(comparisonSettings(state, id).type) || isSingleSourceMeshTask(comparisonTask(state, id));
        const char* label = sources ? "Remove from sources" : side == ComparisonSide::a ? "Remove from group A" : "Remove from group B";
        if (ImGui::MenuItem(label)) { setComparisonObjects(state, {node.objectId}, side, false, id); }
        ImGui::EndPopup();
    }
    if (open && !leaf) {
        for (const auto& child : node.children) {
            if (state.sceneEditRevision != revision) { break; }
            drawComparisonTreeNode(state, side, child, id);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void membershipTree(UiState& state, const ComparisonRuntime& runtime, ComparisonSide side, SceneObjectId id)
{
    const bool uv = isUvAnalysis(comparisonSettings(state, id).type);
    const bool checks = isSingleSourceMeshTask(comparisonTask(state, id));
    const char* label = checks ? "Sources" : uv ? "Source" : side == ComparisonSide::a ? "Group A" : "Group B";
    ImGui::PushID(label);
    const auto sideIndex = side == ComparisonSide::a ? 0 : 1;
    const auto& before = inspectorQueries(runtime, state, id).inputs[sideIndex];
    comparisonEnabledCheckbox(state, side, id, {}, before.memberCount, before.enabledMemberCount);
    const auto& input = inspectorQueries(runtime, state, id).inputs[sideIndex];
    const auto& summary = input.summary;
    const auto& roots = input.roots;
    const auto revision = state.sceneEditRevision;
    const bool open = ImGui::TreeNodeEx("root", ImGuiTreeNodeFlags_FramePadding,
        "%s (%zu/%zu %s on) %zu triangles", label, summary.enabledPartCount, summary.partCount,
        summary.partCount == 1 ? "part" : "parts", input.triangleCount);
    if (ImGui::BeginDragDropTarget()) {
        if (const auto* payload = ImGui::AcceptDragDropPayload(comparisonSourcePayload)) {
            if (payload->DataSize > 0 && payload->DataSize % sizeof(SceneObjectId) == 0) {
                std::vector<SceneObjectId> parts(static_cast<size_t>(payload->DataSize) / sizeof(SceneObjectId));
                std::memcpy(parts.data(), payload->Data, static_cast<size_t>(payload->DataSize));
                setComparisonObjects(state, parts, side, true, id);
            }
        }
        ImGui::EndDragDropTarget();
    }
    const auto* comparison = findComparison(state, id);
    const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    if (ImGui::BeginPopupContextItem("group_actions")) {
        if (ImGui::MenuItem(checks ? "Clear sources" : uv ? "Clear source" : side == ComparisonSide::a ? "Clear group A" : "Clear group B",
                nullptr, false, !members.empty())) {
            clearComparisonGroup(state, side, id);
        }
        ImGui::EndPopup();
    }
    drawObjectIdentityRow("Sources", summary.sourceNames.c_str());
    if (!open) {
        if (!summary.issue.empty() && !members.empty()) { ImGui::TextWrapped("%s", summary.issue.c_str()); }
        ImGui::PopID();
        return;
    }
    const auto& current = summary;
    if (members.empty()) {
        ImGui::TextDisabled("No input");
    } else if (!current.issue.empty()) { ImGui::TextWrapped("%s", current.issue.c_str()); }
    if (members.size() > current.partCount) {
        if (ImGui::SmallButton("Remove missing references")) { removeMissingComparisonParts(state, side, id); }
        setLastItemTooltip("Remove references to parts that are no longer in the scene from this input.");
    }
    for (const auto& root : roots) {
        if (state.sceneEditRevision != revision) { break; }
        drawComparisonTreeNode(state, side, root, id);
    }
    ImGui::TreePop();
    ImGui::PopID();
}
void drawDiagnosticSettingsPopup(UiState& state, SceneObjectId id, DiagnosticCategory category, const char* name)
{
    const std::string hint = std::string(name) + " settings";
    if (drawRenderModeIconButton("settings", "\xef\x80\x93", hint.c_str(), RenderModeState::off, false)) {
        ImGui::OpenPopup("diagnostic_settings");
    }
    const auto buttonMax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos({buttonMax.x, buttonMax.y + ImGui::GetStyle().ItemSpacing.y}, ImGuiCond_Appearing, {1, 0});
    ImGui::SetNextWindowSize({uiSize(360), 0}, ImGuiCond_Always);
    if (!ImGui::BeginPopup("diagnostic_settings", ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) { return; }
    ImGui::TextUnformatted(name);
    ImGui::Separator();
    auto settings = comparisonSettings(state, id);
    bool automatic = diagnosticAutoUpdate(settings, category);
    if (ImGui::Checkbox("Automatic updates", &automatic)) {
        setComparisonAutomaticUpdate(state, id, category, automatic);
        settings = comparisonSettings(state, id);
    }
    const auto initial = settings;
    ImGui::TextWrapped("Analysis: %s", findComparison(state, id)->name.c_str());
    if (category == DiagnosticCategory::degenerateTriangles) {
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(ImGui::GetFontSize()*8);
        ImGui::InputFloat("Needle edge ratio", &settings.degenerates.needleThresholdRatio, 0, 0, "%.6g", ImGuiInputTextFlags_AutoSelectAll);
        setLastItemTooltip("Longest / shortest edge must strictly exceed this ratio (minimum 1).");
        ImGui::SetNextItemWidth(ImGui::GetFontSize()*8);
        ImGui::InputFloat("Cap angle (degrees)", &settings.degenerates.capMinAngleDegrees, 0, 0, "%.6g", ImGuiInputTextFlags_AutoSelectAll);
        setLastItemTooltip("Largest angle must strictly exceed this threshold (90 to 180 degrees).");
        ImGui::TextWrapped("Collapsed and collinear triangles are always included.");
    } else if (category == DiagnosticCategory::selfIntersections) {
        int pairs = static_cast<int>(settings.intersections.limits.pairs);
        int candidates = static_cast<int>(settings.intersections.limits.candidateTests);
        if (ImGui::InputInt("Pair budget", &pairs, 0, 0) && pairs >= 0) { settings.intersections.limits.pairs = static_cast<size_t>(pairs); }
        if (ImGui::InputInt("Candidate budget", &candidates, 0, 0) && candidates >= 0) { settings.intersections.limits.candidateTests = static_cast<size_t>(candidates); }
        ImGui::TextWrapped("0 means unlimited. Dense intersections can require quadratic time and storage. Cancel a running check from the Update column.");
    } else if (category == DiagnosticCategory::fins) {
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(ImGui::GetFontSize()*8);
        ImGui::InputFloat("Maximum area ratio", &settings.topologyInspection.finMaxAreaRatio, 0, 0, "%.6g", ImGuiInputTextFlags_AutoSelectAll);
        ImGui::TextWrapped("Patch area / largest physical-boundary-bearing split patch in each source, inclusive. Woby heuristic candidates; no GGM/GMM parity claimed.");
    } else if (category == DiagnosticCategory::holes) {
        if (ImGui::IsWindowAppearing()) { ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(ImGui::GetFontSize()*8);
        ImGui::InputFloat("Maximum size ratio", &settings.topologyInspection.holeSizeRatioTolerance, 0, 0, "%.6g", ImGuiInputTextFlags_AutoSelectAll);
        ImGui::TextWrapped("Loop bounding-box diagonal / edge-connected component diagonal, inclusive. Larger openings remain boundary findings. Ratios may exceed 1.");
    }
    if (settings != initial) { setComparisonSettings(state, settings, id); }
    ImGui::TextWrapped("Automatic updates run initially and after relevant input or detector settings change. Turn off to run only from the Update column. Cancel or failure waits for a retry or a relevant change.");
    if (ImGui::Button("Close")) { ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

void drawDetectorUpdate(UiState& state, const DetectorStatus& result, bool hasInput, SceneObjectId id, DiagnosticCategory category)
{
    const bool busy = result.phase == IntersectionPhase::queued || result.phase == IntersectionPhase::running;
    const char* icon = "\xef\x80\x9e"; // Repeat.
    const char* tooltip = "";
    switch (result.phase) {
    case IntersectionPhase::notChecked:
        icon = "\xef\x81\x8b"; // Play.
        tooltip = "Run detection for the current geometry.";
        break;
    case IntersectionPhase::queued:
        icon = "\xef\x81\x8d"; // Stop.
        tooltip = "Queued. Cancel this check; other checks keep running.";
        break;
    case IntersectionPhase::running:
        icon = "\xef\x81\x8d";
        tooltip = "Running. Cancel this check; other checks keep running.";
        break;
    case IntersectionPhase::complete:
        tooltip = "Complete. Rerun detection for the current geometry.";
        break;
    case IntersectionPhase::outdated:
        icon = "\xef\x80\xa1"; // Refresh.
        tooltip = "Out of date. Update detection for the current geometry.";
        break;
    case IntersectionPhase::canceled:
        tooltip = "Canceled. Retry detection.";
        break;
    case IntersectionPhase::failed:
        tooltip = "Failed. Retry detection.";
        break;
    }
    if (!hasInput) { icon = "\xef\x81\x8b"; }
    const bool failed = hasInput && result.phase == IntersectionPhase::failed;
    const auto color = failed ? ImVec4(1.0f, 0.38f, 0.35f, 1.0f)
        : hasInput && result.phase == IntersectionPhase::outdated ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f)
        : ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImGui::BeginDisabled(!hasInput);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    const std::string label = std::string(icon) + "###detector_update";
    if (ImGui::Button(label.c_str(), {renderModeButtonSize(), renderModeButtonSize()})) {
        requestComparisonDetector(state, id, category, busy);
    }
    if (failed) {
        // Keep failure recognizable without relying on color alone.
        const auto high = ImGui::GetItemRectMax();
        const float unit = uiSize(1.0f);
        const ImVec2 center(high.x - 5 * unit, high.y - 6 * unit);
        auto* draw = ImGui::GetWindowDrawList();
        const auto ink = ImGui::GetColorU32(ImGuiCol_Text);
        draw->AddCircleFilled(center, 4 * unit, ImGui::GetColorU32(ImGuiCol_WindowBg));
        draw->AddLine({center.x, center.y - 3 * unit}, {center.x, center.y}, ink, unit);
        draw->AddCircleFilled({center.x, center.y + 2 * unit}, unit, ink);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    std::string hint = hasInput ? tooltip : "No input. Add an enabled input to run this check.";
    if (hasInput && failed && !result.error.empty()) { hint += "\n" + result.error; }
    setLastItemTooltip(hint.c_str());
}

const char* diagnosticHint(DiagnosticCategory category)
{
    switch (category) {
    case DiagnosticCategory::boundary:
        return "Finds edges used by only one triangle, marking open borders of the mesh.";
    case DiagnosticCategory::nonManifoldVertices:
        return "Finds vertices whose surrounding triangles do not form one connected fan or ring. "
            "Vertices on non-manifold edges are excluded.";
    case DiagnosticCategory::holes:
        return "Finds simple closed boundary loops small enough to meet the hole-size setting, "
            "relative to their connected component. Larger openings remain boundary findings.";
    case DiagnosticCategory::fins:
        return "Finds candidate patches after splitting at non-manifold edges whose physical boundary "
            "is not one simple loop. The area-ratio setting filters these candidates.";
    case DiagnosticCategory::nonManifold:
        return "Finds edges shared by more than two triangles, where the mesh branches instead of forming a single surface.";
    case DiagnosticCategory::winding:
        return "Finds adjacent triangles with conflicting winding along a shared edge. "
            "Both incident triangles are reported; this does not identify which one should be flipped.";
    case DiagnosticCategory::duplicatePoints:
        return "Finds separate point records with exactly equal source coordinates within a file. "
            "Copies marked with the same original point ID by an importer count as one point.";
    case DiagnosticCategory::duplicateTriangles:
        return "Finds triangles that reuse the same three source point indices within a file, "
            "including reversed winding.";
    case DiagnosticCategory::degenerateTriangles:
        return "Finds collapsed or collinear triangles, thin needles with a large edge-length ratio, "
            "and flat caps with a large maximum angle. Settings control the needle and cap thresholds.";
    case DiagnosticCategory::selfIntersections:
        return "Finds triangle pairs that intersect or overlap in the same plane. "
            "Valid shared edges and vertices are excluded, as are collapsed triangles.";
    }
    return "";
}

void diagnosticRow(UiState& state, const ComparisonRuntime& runtime,
    const char* name, DiagnosticCategory category, bool hasA, bool hasB, SceneObjectId id)
{
    ImGui::PushID(name);
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    const bool duplicate = category == DiagnosticCategory::duplicatePoints || category == DiagnosticCategory::duplicateTriangles;
    const bool vertex = category == DiagnosticCategory::nonManifoldVertices;
    const bool holes = category == DiagnosticCategory::holes;
    const bool fins = category == DiagnosticCategory::fins;
    const bool intersection = category == DiagnosticCategory::selfIntersections;
    const bool degenerate = category == DiagnosticCategory::degenerateTriangles;
    const auto detector = comparisonDetectorStatus(runtime.result, category);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    drawDetectorUpdate(state, detector, hasA || hasB, id, category);
    ImGui::TableNextColumn();
    // Keep the selectable and its text within the Finding column.
    const auto position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = std::max(ImGui::GetFrameHeight(), ImGui::CalcTextSize(name, nullptr, false, width).y);
    if (ImGui::Selectable("##category", settings.diagnosticCategory == category, 0, {0, height})) {
        settings.diagnosticCategory = category;
    }
    ImGui::GetWindowDrawList()->AddText(nullptr, 0, position, ImGui::GetColorU32(ImGuiCol_Text), name, nullptr, width);
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(diagnosticHint(category));
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    const bool current = inspectorDetectorReady(runtime, state, id, category);
    if (!hasA && !hasB) { ImGui::TableNextColumn(); ImGui::TextDisabled("--"); }
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        if ((side == ComparisonSide::a && !hasA) || (side == ComparisonSide::b && !hasB)) { continue; }
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        const auto& surface = side == ComparisonSide::a ? runtime.result.original : runtime.result.repaired;
        const auto summary = diagnosticSummary(surface, category, detector.phase, current);
        const auto text = diagnosticSummaryText(summary);
        ImGui::TextWrapped("%s", text.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
            ImGui::Text("Status: %s", analysisResultStateLabel(summary.state));
            if (!current && detector.hasResult) {
                ImGui::Text("Previous result: %zu known findings.", detector.knownCounts[side == ComparisonSide::a ? 0 : 1]);
            }
            if (!detector.error.empty()) { ImGui::TextWrapped("%s", detector.error.c_str()); }
            if (current && duplicate) {
                const auto& result = comparisonDuplicates(runtime.result, side, category);
                ImGui::Text("%zu extra records in %zu groups; %zu unavailable sources.",
                    result.duplicateCount, result.findings.size(), result.unavailableSources);
            } else if (current && intersection) {
                ImGui::Text("%zu known face pairs; %zu affected faces.", surface.intersections.findings.size(), surface.intersections.affectedFaces);
                if (surface.intersections.truncated) { ImGui::TextWrapped("%s", surface.intersections.truncationReason.c_str()); }
            } else if (current && degenerate) {
                ImGui::Text("%zu collapsed / collinear; %zu needles; %zu caps.", surface.degenerates.collapsedCount,
                    surface.degenerates.needleCount, surface.degenerates.capCount);
                ImGui::TextWrapped("Reason counts may overlap. Each triangle is counted once.");
            }
            ImGui::TextWrapped("%s", diagnosticHint(category));
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }
    ImGui::TableNextColumn();
    bool visible = fins ? settings.topologyInspection.showFins : intersection ? settings.intersections.show : vertex ? settings.topologyInspection.showNonManifoldVertices : holes ? settings.topologyInspection.showHoles : degenerate ? settings.degenerates.show : category == DiagnosticCategory::boundary ? settings.showBoundaries
        : category == DiagnosticCategory::duplicatePoints ? settings.duplicates.showPoints
        : category == DiagnosticCategory::duplicateTriangles ? settings.duplicates.showTriangles
        : category == DiagnosticCategory::winding ? settings.showWinding : settings.showNonManifold;
    if (drawVisibilityIconField(name, visible)) {
        if (intersection) { settings.intersections.show = visible; }
        else if (vertex) { settings.topologyInspection.showNonManifoldVertices = visible; }
        else if (holes) { settings.topologyInspection.showHoles = visible; }
        else if (fins) { settings.topologyInspection.showFins = visible; }
        else if (degenerate) { settings.degenerates.show = visible; }
        else if (category == DiagnosticCategory::boundary) { settings.showBoundaries = visible; }
        else if (category == DiagnosticCategory::duplicatePoints) { settings.duplicates.showPoints = visible; }
        else if (category == DiagnosticCategory::duplicateTriangles) { settings.duplicates.showTriangles = visible; }
        else if (category == DiagnosticCategory::winding) { settings.showWinding = visible; }
        else { settings.showNonManifold = visible; }
    }
    if (settings != initial) { setComparisonSettings(state, settings, id); }
    ImGui::TableNextColumn();
    drawDiagnosticSettingsPopup(state, id, category, name);
    ImGui::PopID();
}

void drawDuplicateFindings(UiState& state, const ComparisonRuntime& runtime, bool current, SceneObjectId id)
{
    const auto settings = comparisonSettings(state, id);
    const bool hasTarget = inspectorEnabledPartCount(runtime, state, settings.diagnosticSide, id) != 0;
    const bool points = settings.diagnosticCategory == DiagnosticCategory::duplicatePoints;
    if (!points && settings.diagnosticCategory != DiagnosticCategory::duplicateTriangles) { return; }
    const auto& result = comparisonDuplicates(runtime.result, settings.diagnosticSide, settings.diagnosticCategory);
    current = inspectorDetectorReady(runtime, state, id, settings.diagnosticCategory);
    if (!current || !hasTarget) { return; }
    if (result.unavailableSources) {
        ImGui::TextWrapped("%zu source(s) unavailable: original source records were not retained.", result.unavailableSources);
    }
    ImGui::TextWrapped("%zu extra %s in %zu group%s", result.duplicateCount, points ? "points" : "triangles",
        result.findings.size(), result.findings.size() == 1 ? "" : "s");
    if (result.findings.empty()) { return; }
    const auto* comparison = findComparison(state, id);
    const auto selected = comparison->diagnosticFocus ? comparison->diagnosticFocus->index : size_t{0};
    const size_t page = selected / diagnosticPageSize, pages = (result.findings.size() + diagnosticPageSize - 1) / diagnosticPageSize;
    const auto choose = [&](size_t index) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, index, id); };
    if (ImGui::BeginTable("duplicate_findings", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source"); ImGui::TableSetupColumn("Representative"); ImGui::TableSetupColumn("Extra records");
        ImGui::TableHeadersRow();
        for (size_t i = page*diagnosticPageSize; i < std::min((page+1)*diagnosticPageSize, result.findings.size()); ++i) {
            const auto& finding = result.findings[i];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Text("%zu", i+1); ImGui::TableNextColumn();
            if (ImGui::Selectable(finding.source.c_str(), comparison->diagnosticFocus && selected == i, ImGuiSelectableFlags_SpanAllColumns)) { choose(i); }
            ImGui::TableNextColumn(); ImGui::Text("%zu", finding.members.front().id+1);
            ImGui::TableNextColumn(); ImGui::Text("%zu", finding.members.size()-1); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(page == 0);
    if (ImGui::Button("Previous page")) { choose((page-1)*diagnosticPageSize); }
    setLastItemTooltip("Show the previous page and select its first duplicate group.");
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::Text("%zu / %zu", page+1, pages); ImGui::SameLine();
    ImGui::BeginDisabled(page+1 == pages);
    if (ImGui::Button("Next page")) { choose((page+1)*diagnosticPageSize); }
    setLastItemTooltip("Show the next page and select its first duplicate group.");
    ImGui::EndDisabled();
    if (comparison->diagnosticFocus && comparison->diagnosticFocus->index < result.findings.size()) {
        const auto& finding = result.findings[comparison->diagnosticFocus->index];
        ImGui::TextWrapped("%s - %s", finding.source.c_str(), sourceProvenanceName(finding.provenance));
        ImGui::TextWrapped("Source IDs (1-based); representative: %zu", finding.members.front().id+1);
        if (ImGui::BeginChild("duplicate_members", {0, ImGui::GetTextLineHeightWithSpacing()*4}, ImGuiChildFlags_Borders)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(std::min(finding.members.size(), static_cast<size_t>(INT_MAX))));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const auto& member = finding.members[static_cast<size_t>(i)];
                    ImGui::Text("%zu%s", member.id+1, points ? "" : member.reversed ? " - reversed order" : " - same cyclic order");
                }
            }
        }
        ImGui::EndChild();
    }
}

void drawIntersectionFindings(UiState& state, const ComparisonRuntime& runtime, bool current, SceneObjectId id)
{
    const auto settings = comparisonSettings(state, id);
    if (settings.diagnosticCategory != DiagnosticCategory::selfIntersections) { return; }
    ImGui::TextWrapped("Exact triangle intersections within each source file. Valid shared vertices and edges are excluded; coplanar overlap is included. Run checks once. Update checks changed geometry. The settings menu offers automatic updates.");
    (void)current;
    const auto& inspection = (settings.diagnosticSide == ComparisonSide::a ? runtime.result.original : runtime.result.repaired).intersections;
    if (inspection.phase == IntersectionPhase::running) {
        ImGui::Text("Checking... %.1f s", std::chrono::duration<double>(std::chrono::steady_clock::now()-runtime.intersection.started).count());
    }
    if (!inspection.error.empty()) { ImGui::TextWrapped("%s", inspection.error.c_str()); }
    if (!inspectorStagesReady(runtime, state, id, comparisonIntersections)) {
        if (inspection.hasResult) { ImGui::TextWrapped("Previous result: %zu known pairs. Update to inspect current geometry.", inspection.findings.size()); }
        return;
    }
    const auto& result = (settings.diagnosticSide == ComparisonSide::a ? runtime.result.original : runtime.result.repaired).intersections;
    ImGui::TextWrapped("%zu known face pairs; %zu known affected faces (%s). %zu collapsed faces excluded.", result.findings.size(), result.affectedFaces, intersectionStatus(result), result.excludedCollapsedFaces);
    if (result.truncated) { ImGui::TextWrapped("Detection limit reached. These findings are incomplete; totals are unknown."); }
    const auto* comparison = findComparison(state, id);
    const size_t selected = comparison->diagnosticFocus ? comparison->diagnosticFocus->index : 0;
    const size_t begin = selected/diagnosticPageSize*diagnosticPageSize, end = std::min(begin+diagnosticPageSize, result.findings.size());
    if (ImGui::BeginTable("intersection_findings", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source"); ImGui::TableSetupColumn("First triangle (part)"); ImGui::TableSetupColumn("Second triangle (part)");
        ImGui::TableHeadersRow();
        for (size_t i = begin; i < end; ++i) {
            const auto& finding = result.findings[i];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Text("%zu", i+1); ImGui::TableNextColumn();
            if (ImGui::Selectable(finding.source.c_str(), comparison->diagnosticFocus && selected == i, ImGuiSelectableFlags_SpanAllColumns)) {
                selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, i, id);
            }
            for (const auto& face : finding.faces) {
                ImGui::TableNextColumn(); ImGui::Text("%zu (%llu)", face.triangleId+1, static_cast<unsigned long long>(face.partId));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(begin == 0);
    if (ImGui::Button("Previous pairs")) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, begin-diagnosticPageSize, id); }
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(end >= result.findings.size());
    if (ImGui::Button("Next pairs")) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, end, id); }
    ImGui::EndDisabled();
}

void drawDegenerateFindings(UiState& state, const ComparisonRuntime& runtime, bool current, SceneObjectId id)
{
    auto settings = comparisonSettings(state, id);
    if (settings.diagnosticCategory != DiagnosticCategory::degenerateTriangles) { return; }
    ImGui::TextWrapped("Collapsed or collinear triangles, needles, and caps. Each source triangle / transformed part is counted once; reason counts can overlap.");
    current = inspectorDetectorReady(runtime, state, id, settings.diagnosticCategory);
    if (!current
        || inspectorEnabledPartCount(runtime, state, settings.diagnosticSide, id) == 0) { return; }
    const auto& result = (settings.diagnosticSide == ComparisonSide::a ? runtime.result.original : runtime.result.repaired).degenerates;
    if (result.unavailableSources) { ImGui::TextWrapped("%zu source(s) unavailable: retained source records are missing.", result.unavailableSources); }
    ImGui::TextWrapped("%zu affected triangles: %zu collapsed / collinear, %zu needles, %zu caps", result.findings.size(), result.collapsedCount, result.needleCount, result.capCount);
    if (result.findings.empty()) { return; }
    const auto* comparison = findComparison(state, id);
    const size_t selected = comparison->diagnosticFocus ? comparison->diagnosticFocus->index : 0;
    const size_t page = selected/diagnosticPageSize, pages = (result.findings.size()+diagnosticPageSize-1)/diagnosticPageSize;
    const auto choose = [&](size_t index) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, index, id); };
    if (ImGui::BeginTable("degenerate_findings", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source"); ImGui::TableSetupColumn("Triangle"); ImGui::TableSetupColumn("Reasons");
        ImGui::TableHeadersRow();
        for (size_t i = page*diagnosticPageSize; i < std::min((page+1)*diagnosticPageSize, result.findings.size()); ++i) {
            const auto& finding = result.findings[i];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Text("%zu", i+1); ImGui::TableNextColumn();
            if (ImGui::Selectable(finding.source.c_str(), comparison->diagnosticFocus && selected == i, ImGuiSelectableFlags_SpanAllColumns)) { choose(i); }
            ImGui::TableNextColumn(); ImGui::Text("%zu", finding.triangleId+1);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s%s%s", finding.reasons.collapsed ? "Collapsed " : "",
                finding.reasons.needle ? "Needle " : "", finding.reasons.cap ? "Cap" : ""); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(page == 0);
    if (ImGui::Button("Previous page")) { choose((page-1)*diagnosticPageSize); }
    setLastItemTooltip("Show the previous page and select its first degenerate triangle.");
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::Text("%zu / %zu", page+1, pages); ImGui::SameLine();
    ImGui::BeginDisabled(page+1 == pages);
    if (ImGui::Button("Next page")) { choose((page+1)*diagnosticPageSize); }
    setLastItemTooltip("Show the next page and select its first degenerate triangle.");
    ImGui::EndDisabled();
    if (comparison->diagnosticFocus && comparison->diagnosticFocus->index < result.findings.size()) {
        const auto& finding = result.findings[comparison->diagnosticFocus->index];
        ImGui::TextWrapped("%s - %s", finding.source.c_str(), sourceProvenanceName(finding.provenance));
        ImGui::TextWrapped("Generated triangle %zu (1-based), part %llu", finding.triangleId+1, static_cast<unsigned long long>(finding.partId));
        if (std::isfinite(finding.reasons.edgeRatio)) { ImGui::Text("Edge ratio: %.8g", finding.reasons.edgeRatio); }
        else { ImGui::TextUnformatted("Edge ratio: unavailable or exceeds numeric range"); }
        if (std::isfinite(finding.reasons.maximumAngleDegrees)) { ImGui::Text("Maximum angle: %.8g degrees", finding.reasons.maximumAngleDegrees); }
        else { ImGui::TextUnformatted("Maximum angle: unavailable (zero-length edge)"); }
    }
}

const std::vector<TopologyEdgeFinding>& topologyFindings(const MeshTopology& topology, DiagnosticCategory category)
{
    if (category == DiagnosticCategory::boundary) { return topology.boundaries; }
    if (category == DiagnosticCategory::nonManifold) { return topology.nonManifoldEdges; }
    return topology.windingEdges;
}

void drawTopologyInspectionFindings(UiState& state, const ComparisonRuntime& runtime, bool current, SceneObjectId id)
{
    const auto settings = comparisonSettings(state, id);
    const bool holes = settings.diagnosticCategory == DiagnosticCategory::holes;
    const bool fins = settings.diagnosticCategory == DiagnosticCategory::fins;
    if (!holes && !fins && settings.diagnosticCategory != DiagnosticCategory::nonManifoldVertices) { return; }
    current = inspectorDetectorReady(runtime, state, id, settings.diagnosticCategory);
    if (!current) { return; }
    const auto& topology = (settings.diagnosticSide == ComparisonSide::a ? runtime.result.original : runtime.result.repaired).topology;
    ImGui::TextWrapped("%s; per source; status: %s. %zu collapsed faces excluded.", topologyModeName(topology.mode), fins ? finStatus(topology) : topologyStatus(topology), topology.excludedCollapsedFaces);
    if (fins) {
        if (topology.unavailableFinAreaSources) { ImGui::TextWrapped("Area ratios are unavailable for %zu sources because their boundary patch areas cannot be represented as positive finite doubles.", topology.unavailableFinAreaSources); }
        ImGui::TextWrapped("Woby fin candidates (heuristic): %zu patches pass area ratio <= %.6g. Physical boundary must not be one simple loop; at least two split components per source required. Cut boundaries are kept separate.", topology.fins.size(), settings.topologyInspection.finMaxAreaRatio);
    } else if (holes) {
        size_t loops = 0, open = 0, branched = 0;
        for (const auto& boundary : topology.boundaryRegions) {
            loops += boundary.kind == BoundaryKind::loop; open += boundary.kind == BoundaryKind::open; branched += boundary.kind == BoundaryKind::branched;
        }
        ImGui::TextWrapped("%zu simple loops; %zu pass size ratio <= %.6g. %zu open and %zu branched boundary regions are excluded. All edges remain available under Boundary edges.", loops, topology.holes.size(), settings.topologyInspection.holeSizeRatioTolerance, open, branched);
    } else {
        ImGui::TextWrapped("Vertex links must form one interior cycle or one boundary path. %zu vertices on non-manifold edges excluded; unused points are not defects here.", topology.excludedNonManifoldEdgeVertices);
    }
    const size_t count = fins ? topology.fins.size() : holes ? topology.holes.size() : topology.nonManifoldVertices.size();
    if (!count) { return; }
    const auto* comparison = findComparison(state, id);
    const auto* focused = focusedComparisonDiagnostic(state, runtime.result, runtime.resultSignature, id, inspectorQueries(runtime, state, id).signature);
    const size_t selected = focused ? comparison->diagnosticFocus->index : 0;
    const size_t page = selected / diagnosticPageSize;
    ImGui::PushID("topology_inspection_findings");
    ImGui::BeginDisabled(page == 0);
    if (ImGui::SmallButton("Previous page")) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, (page-1)*diagnosticPageSize, id); }
    setLastItemTooltip("Show the previous page and select its first finding.");
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled((page+1)*diagnosticPageSize >= count);
    if (ImGui::SmallButton("Next page")) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, (page+1)*diagnosticPageSize, id); }
    setLastItemTooltip("Show the next page and select its first finding.");
    ImGui::EndDisabled();
    if (ImGui::BeginTable("findings", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source");
        ImGui::TableSetupColumn(fins ? "Patch" : holes ? "Loop" : "Point", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(fins ? "Boundary" : holes ? "Component" : "Part");
        ImGui::TableHeadersRow();
        for (size_t i = page*diagnosticPageSize; i < std::min(count, (page+1)*diagnosticPageSize); ++i) {
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Text("%zu", i+1); ImGui::TableNextColumn();
            const auto sourceIndex = fins ? topology.finPatches[topology.fins[i]].source
                : holes ? topology.boundaryRegions[topology.holes[i]].source : topology.nonManifoldVertices[i].source;
            const auto& source = topology.sources[sourceIndex];
            if (ImGui::Selectable(source.source.c_str(), focused && selected == i, ImGuiSelectableFlags_SpanAllColumns)) {
                selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, i, id);
            }
            ImGui::TableNextColumn();
            if (fins) {
                const auto& patch = topology.finPatches[topology.fins[i]];
                ImGui::Text("%zu", patch.patch+1);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(finBoundaryKindName(patch.boundary));
            } else if (holes) {
                const auto& boundary = topology.boundaryRegions[topology.holes[i]];
                ImGui::Text("%zu", topology.holes[i]+1);
                ImGui::TableNextColumn(); ImGui::Text("%zu", boundary.component+1);
            } else {
                const auto& finding = topology.nonManifoldVertices[i];
                const auto& ref = source.vertices[finding.vertex].references.front();
                ImGui::Text("%zu", ref.pointId+1);
                ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(ref.partId));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (fins) {
        const auto& patch = topology.finPatches[topology.fins[selected]];
        ImGui::TextWrapped("%zu faces; area %.9g / largest boundary-bearing patch %.9g = %.9g. %zu physical boundary edges; %zu cut edges; %zu split components.", patch.faces.size(), patch.area, patch.denominatorArea, patch.areaRatio, patch.physicalBoundaryEdges.size(), patch.cutBoundaryEdges.size(), patch.splitComponentCount);
    } else if (holes) {
        const auto& boundary = topology.boundaryRegions[topology.holes[selected]];
        ImGui::TextWrapped("%zu edges; loop diagonal %.9g / component diagonal %.9g = %.9g. Bounds use world axes; display offset is excluded.", boundary.edges.size(), boundary.diagonal, boundary.componentDiagonal, boundary.sizeRatio);
    } else {
        const auto& finding = topology.nonManifoldVertices[selected];
        const auto& source = topology.sources[finding.source];
        const auto& vertex = source.vertices[finding.vertex];
        ImGui::TextWrapped("%zu link components; %zu incident faces; %zu source point references. %s", finding.linkComponents, vertex.faces.size(), vertex.references.size(), sourceProvenanceName(source.provenance));
        for (size_t i = 0; i < std::min(size_t{100}, vertex.faces.size()); ++i) {
            const auto& ref = source.faces[vertex.faces[i]].reference;
            ImGui::Text("Triangle %zu, part %llu", ref.triangleId+1, static_cast<unsigned long long>(ref.partId));
        }
        if (vertex.faces.size() > 100) { ImGui::TextDisabled("Showing first 100 incident faces."); }
    }
    ImGui::PopID();
}

void drawTopologyFindings(UiState& state, const ComparisonRuntime& runtime, bool current, SceneObjectId id)
{
    const auto settings = comparisonSettings(state, id);
    const auto category = settings.diagnosticCategory;
    if (category != DiagnosticCategory::boundary && category != DiagnosticCategory::nonManifold && category != DiagnosticCategory::winding) { return; }
    current = inspectorDetectorReady(runtime, state, id, settings.diagnosticCategory);
    if (!current) { return; }
    const auto& surface = settings.diagnosticSide == ComparisonSide::a ? runtime.result.original : runtime.result.repaired;
    const auto& topology = surface.topology;
    ImGui::TextWrapped("Topology: %s; files inspected separately. Status: %s. Collapsed faces excluded: %zu.",
        topologyModeName(topology.mode), topologyStatus(topology), topology.excludedCollapsedFaces);
    if (topology.unavailableSources) { ImGui::TextWrapped("%zu sources unavailable. Original source records were not retained.", topology.unavailableSources); }
    const auto& findings = topologyFindings(topology, category);
    if (findings.empty()) { return; }
    if (category == DiagnosticCategory::winding) {
        ImGui::TextWrapped("%zu affected triangle instances across %zu conflict edges. Both incident faces are reported; no unique erroneous face or repair direction is implied. %zu orientation contradiction witnesses.",
            topology.windingFaces.size(), findings.size(), topology.orientationContradictions);
    }
    const auto* comparison = findComparison(state, id);
    const size_t selected = comparison->diagnosticFocus ? comparison->diagnosticFocus->index : 0;
    const size_t page = selected / diagnosticPageSize;
    ImGui::Text("Edges %zu-%zu of %zu", page*diagnosticPageSize+1, std::min(findings.size(), (page+1)*diagnosticPageSize), findings.size());
    ImGui::BeginDisabled(page == 0);
    if (ImGui::SmallButton("Previous page##topology")) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, (page-1)*diagnosticPageSize, id); }
    setLastItemTooltip("Show the previous page and select its first edge.");
    ImGui::EndDisabled(); ImGui::SameLine();
    ImGui::BeginDisabled((page+1)*diagnosticPageSize >= findings.size());
    if (ImGui::SmallButton("Next page##topology")) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, (page+1)*diagnosticPageSize, id); }
    setLastItemTooltip("Show the next page and select its first edge.");
    ImGui::EndDisabled();
    if (ImGui::BeginTable("topology_findings", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Source"); ImGui::TableSetupColumn("Edge"); ImGui::TableSetupColumn("Incident faces");
        ImGui::TableHeadersRow();
        for (size_t i = page*diagnosticPageSize; i < std::min(findings.size(), (page+1)*diagnosticPageSize); ++i) {
            const auto& finding = findings[i];
            const auto& source = topology.sources[finding.source];
            const auto& edge = source.edges[finding.edge];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Text("%zu", i+1); ImGui::TableNextColumn();
            if (ImGui::Selectable(source.source.c_str(), comparison->diagnosticFocus && selected == i, ImGuiSelectableFlags_SpanAllColumns)) {
                selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, i, id);
            }
            ImGui::TableNextColumn(); ImGui::Text("%zu", finding.edge+1);
            ImGui::TableNextColumn(); ImGui::Text("%zu", edge.incidentFaces.size());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!comparison->diagnosticFocus) { return; }
    const auto& finding = findings.at(comparison->diagnosticFocus->index);
    const auto& source = topology.sources[finding.source];
    const auto& edge = source.edges[finding.edge];
    ImGui::TextWrapped("%s: %s (%s)", source.source.c_str(), topologyModeName(source.mode), sourceProvenanceName(source.provenance));
    if (edge.orientationContradiction) { ImGui::TextWrapped("Orientation contradiction: manifold flip constraints cannot all be satisfied in this region."); }
    if (ImGui::BeginChild("incident_faces", {0, ImGui::GetTextLineHeightWithSpacing()*5}, ImGuiChildFlags_Borders)) {
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min(edge.incidentFaces.size(), static_cast<size_t>(std::numeric_limits<int>::max()))));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& use = edge.incidentFaces[static_cast<size_t>(i)];
                const auto& ref = source.faces[use.face].reference;
                ImGui::Text("Triangle %zu / part %llu / %s", ref.triangleId+1, static_cast<unsigned long long>(ref.partId), use.forward ? "forward" : "reverse");
            }
        }
    }
    ImGui::EndChild();
}

void drawDiagnosticNavigation(UiState& state, const ComparisonRuntime& runtime, bool current,
    bool hasA, bool hasB, SceneObjectId id)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Diagnostics");
    auto settings = comparisonSettings(state, id);
    ImGui::SameLine();
    drawInformationIcon("diagnostics_info", "Surface diagnostics",
        "Topology is inspected separately within each source file. Original indices preserve source connectivity; exact positions join equal coordinates. "
        "Open boundaries may be intentional. Run Self-intersections to check crossings and coplanar overlap.\n\n"
        "Use Previous finding and Next finding to inspect edges or duplicate groups across the sources. "
        "Navigation wraps; Next starts at the first finding and Previous at the last.\n\n"
        "Duplicate points use exactly equal imported coordinates within each source file. "
        "Duplicate triangles use the same three source point IDs regardless of winding. "
        "Counts are extra records; navigation visits groups. No tolerance is applied.\n\n"
        "Whole-file inspection includes unused points; selected parts include referenced points. "
        "OBJ IDs precede UV/normal splitting. Triangle IDs identify generated triangles, not original polygons. "
        "Plugin IDs describe the importer vertex table. "
        "Partial results show a known count followed by '+ Partial'; hover the count for details.");
    int topologyMode = static_cast<int>(settings.topologyMode);
    if (ImGui::Combo("Topology", &topologyMode, "Original indices\0Exact positions\0")) {
        settings.topologyMode = static_cast<TopologyMode>(topologyMode);
        setComparisonSettings(state, settings, id);
        current = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Files stay separate. Original indices preserve source connectivity.\nExact positions join exactly equal world coordinates within a file, with no epsilon.");
    }
    validateComparisonDiagnosticFocus(state, runtime.result, current ? runtime.resultSignature : 0, id, inspectorQueries(runtime, state, id).signature);
    constexpr struct {
        const char* name;
        DiagnosticCategory category;
    } rows[] = {
        {"Boundary edges", DiagnosticCategory::boundary},
        {"Non-manifold vertices", DiagnosticCategory::nonManifoldVertices},
        {"Holes", DiagnosticCategory::holes},
        {"Fin candidates", DiagnosticCategory::fins},
        {"Non-manifold edges", DiagnosticCategory::nonManifold},
        {"Inconsistent triangles", DiagnosticCategory::winding},
        {"Duplicate points", DiagnosticCategory::duplicatePoints},
        {"Duplicate triangles", DiagnosticCategory::duplicateTriangles},
        {"Degenerate triangles", DiagnosticCategory::degenerateTriangles},
        {"Self-intersections", DiagnosticCategory::selfIntersections},
    };
    // Fixed icon/count columns and a wrapping name use the available pane width.
    // Only the outer inspector scrolls, so row controls are always reachable.
    if (ImGui::BeginTable("Analysis diagnostics", 5,
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
        ImGui::TableSetupColumn("Finding", ImGuiTableColumnFlags_WidthStretch);
        const float countWidth = ImGui::GetFontSize() * 3.5f;
        ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, countWidth);
        ImGui::TableSetupColumn("##visibility", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
        ImGui::TableSetupColumn("##settings", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
        const float buttonSize = renderModeButtonSize();
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers, buttonSize + ImGui::GetStyle().CellPadding.y * 2);
        for (int column = 0; column < 3; ++column) {
            ImGui::TableSetColumnIndex(column);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (buttonSize - ImGui::GetTextLineHeight()) * 0.5f));
            ImGui::TableHeader(ImGui::TableGetColumnName(column));
        }
        ImGui::TableSetColumnIndex(3);
        const auto visibleCount = countVisibleComparisonDiagnostics(comparisonSettings(state, id));
        bool allVisible = visibleCount == diagnosticCategoryCount;
        if (drawVisibilityIconField("all diagnostics", allVisible, visibleCount > 0 && !allVisible)) {
            setComparisonDiagnosticsVisible(state, allVisible, id);
        }
        for (const auto& row : rows) {
            diagnosticRow(state, runtime, row.name, row.category, hasA, hasB, id);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Findings");
    const auto selected = comparisonSettings(state, id);
    for (const auto& row : rows) {
        if (row.category == selected.diagnosticCategory) { ImGui::TextUnformatted(row.name); break; }
    }
    const auto& findings = comparisonDiagnosticEdges(runtime.result, selected.diagnosticSide, selected.diagnosticCategory);
    const bool ready = inspectorDetectorReady(runtime, state, id, selected.diagnosticCategory);
    const auto selectedPhase = comparisonDetectorStatus(runtime.result, selected.diagnosticCategory).phase;
    const auto selectedSummary = diagnosticSummary(selected.diagnosticSide == ComparisonSide::a ? runtime.result.original : runtime.result.repaired,
        selected.diagnosticCategory, selectedPhase, ready);
    ImGui::TextWrapped("%s",
        selectedSummary.state == AnalysisResultState::ready && !selectedSummary.count ? "No findings" : analysisResultStateLabel(selectedSummary.state));
    if (selectedSummary.state == AnalysisResultState::partial) { ImGui::TextWrapped("Known findings only. Some sources or candidate pairs were not checked."); }
    ImGui::BeginDisabled(!ready || findings.empty());
    int step = 0;
    if (ImGui::Button("Previous finding")) { step = -1; }
    ImGui::SameLine();
    if (ImGui::Button("Next finding")) { step = 1; }
    ImGui::EndDisabled();
    if (step) { navigateComparisonDiagnostic(state, runtime.result, runtime.resultSignature, step, id); }
    ImGui::BeginDisabled(!inspectorStagesReady(runtime, state, id, comparisonSource));
    if (ImGui::Button("Full result")) { frameComparison(state, id); }
    setLastItemTooltip("Clear finding focus and frame the full analysis result.");
    ImGui::EndDisabled();
    current = inspectorStagesReady(runtime, state, id, comparisonSource);
    validateComparisonDiagnosticFocus(state, runtime.result, current ? runtime.resultSignature : 0, id, inspectorQueries(runtime, state, id).signature);
    const auto* comparison = findComparison(state, id);
    if (!current) { ImGui::TextWrapped("Diagnostics unavailable until results are ready"); }
    else if (comparison->diagnosticFocus) {
        const auto& focus = *comparison->diagnosticFocus;
        const char* name = focus.category == DiagnosticCategory::boundary ? "Boundary"
            : focus.category == DiagnosticCategory::nonManifoldVertices ? "Non-manifold vertex"
            : focus.category == DiagnosticCategory::fins ? "Fin candidate patch"
            : focus.category == DiagnosticCategory::holes ? "Hole loop"
            : focus.category == DiagnosticCategory::nonManifold ? "Non-manifold edges"
            : focus.category == DiagnosticCategory::duplicatePoints ? "Duplicate point group"
            : focus.category == DiagnosticCategory::selfIntersections ? "Intersecting face pair"
            : focus.category == DiagnosticCategory::degenerateTriangles ? "Degenerate triangle"
            : focus.category == DiagnosticCategory::duplicateTriangles ? "Duplicate triangle group" : "Winding edges";
        const auto count = comparisonDiagnosticEdges(runtime.result, focus.side, focus.category).size();
        ImGui::TextColored(ImVec4(1, 1, .1f, 1), "%s: %zu of %zu", name, focus.index + 1, count);
        ImGui::TextWrapped("Focused finding is highlighted in yellow");
    }
    drawDuplicateFindings(state, runtime, current, id);
    drawDegenerateFindings(state, runtime, current, id);
    drawIntersectionFindings(state, runtime, current, id);
    drawTopologyFindings(state, runtime, current, id);
    drawTopologyInspectionFindings(state, runtime, current, id);

}

void submitEdges(woby::graphics::ViewId view, woby::graphics::VertexBufferHandle vertices, woby::graphics::ProgramHandle program,
                 woby::graphics::UniformHandle uniform, const std::array<float, 4> &color, const float* transform)
{
    if (!woby::graphics::isValid(vertices))
    {
        return;
    }
    woby::graphics::setTransform(transform);
    woby::graphics::setVertexBuffer(0, vertices);
    woby::graphics::setUniform(uniform, color.data());
    woby::graphics::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_LINES | WOBY_GPU_STATE_DEPTH_TEST_ALWAYS |
                   WOBY_GPU_STATE_MSAA);
    woby::graphics::submit(view, program);
}
void submitWire(woby::graphics::ViewId view, const ComparisonGpuSurface &gpu, woby::graphics::ProgramHandle program,
                woby::graphics::UniformHandle uniform, const std::array<float, 4> &color, bool xray, const float* transform)
{
    woby::graphics::setTransform(transform);
    woby::graphics::setVertexBuffer(0, gpu.vertices);
    woby::graphics::setIndexBuffer(gpu.lines);
    woby::graphics::setUniform(uniform, color.data());
    woby::graphics::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_LINES | WOBY_GPU_STATE_MSAA |
                   (xray ? WOBY_GPU_STATE_DEPTH_TEST_ALWAYS : WOBY_GPU_STATE_DEPTH_TEST_LEQUAL));
    woby::graphics::submit(view, program);
}
} // namespace

namespace {
uint64_t nextResultsRevision()
{
    // Called only by the main-thread runtime adapter, unique even after undo/recreation.
    static uint64_t revision = 0;
    return ++revision;
}
}

bool comparisonStagesReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    uint32_t stages, bool requireGpu)
{
    return comparisonStagesReady(runtime, comparisonSettings(state, id), comparisonGeometrySignature(state, id), stages, requireGpu);
}

bool comparisonStagesReady(const ComparisonRuntime& runtime, const ComparisonSettings& settings,
    uint64_t signature, uint32_t stages, bool requireGpu)
{
    if (!signature || runtime.resultSignature != signature || runtime.cache.signature != signature
        || (runtime.cache.completed & stages) != stages
        || (requireGpu && (runtime.uploadedStages & stages) != stages)) { return false; }
    if ((stages & (comparisonTopology | comparisonIntersections)) && runtime.cache.topologyMode != settings.topologyMode) { return false; }
    if ((stages & comparisonDegenerates) && !sameDegenerateThresholds(runtime.cache.degenerates, settings.degenerates)) { return false; }
    if ((stages & comparisonTopology) && (runtime.result.original.topology.inspection.holeSizeRatioTolerance != settings.topologyInspection.holeSizeRatioTolerance
        || runtime.result.repaired.topology.inspection.holeSizeRatioTolerance != settings.topologyInspection.holeSizeRatioTolerance
        || runtime.result.original.topology.inspection.finMaxAreaRatio != settings.topologyInspection.finMaxAreaRatio
        || runtime.result.repaired.topology.inspection.finMaxAreaRatio != settings.topologyInspection.finMaxAreaRatio)) { return false; }
    if ((stages & comparisonIntersections) && (runtime.intersection.limits != settings.intersections.limits || runtime.result.original.intersections.phase != IntersectionPhase::complete)) { return false; }
    return true;
}

bool comparisonDetectorReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    DiagnosticCategory category, bool requireGpu)
{
    return comparisonDetectorStatus(runtime.result, category).phase == IntersectionPhase::complete
        && comparisonStagesReady(runtime, state, id, comparisonDiagnosticStage(category), requireGpu);
}

bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id, bool fullResults)
{
    const bool both = enabledComparisonPartCount(state, ComparisonSide::a, id) != 0
        && enabledComparisonPartCount(state, ComparisonSide::b, id) != 0;
    return resultsReady(runtime, state, id, comparisonGeometrySignature(state, id), both, fullResults);
}

namespace {
bool resultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    uint64_t signature, bool both, bool fullResults)
{
    const auto settings = comparisonSettings(state, id);
    const auto required = requestedComparisonStages(settings, both, fullResults) & ~(comparisonDetectors | comparisonIntersections);
    if (!comparisonStagesReady(runtime, settings, signature, required) || (!fullResults && !runtime.ready)) { return false; }
    const auto* comparison = findComparison(state, id);
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        const auto category = static_cast<DiagnosticCategory>(i);
        const auto phase = runtime.result.detectors[i].phase;
        if (comparison && comparison->detectorRequests[i].revision != runtime.consumedDetectorRequests[i]) { return false; }
        if (phase == IntersectionPhase::queued || phase == IntersectionPhase::running) { return false; }
        if (diagnosticAutoUpdate(settings, category) && phase != IntersectionPhase::canceled
            && phase != IntersectionPhase::failed && !(phase == IntersectionPhase::complete
                && comparisonStagesReady(runtime, settings, signature, comparisonDiagnosticStage(category)))) { return false; }
    }
    return true;
}

} // namespace

ComparisonSettings readyComparisonSettings(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id)
{
    auto settings = effectiveComparisonSettings(state, id);
    const auto ready = [&](uint32_t stage) { return comparisonStagesReady(runtime, state, id, stage, true); };
    if ((settings.mode == ComparisonMode::distance && !ready(comparisonDistance))
        || (settings.mode == ComparisonMode::surfaceQuality && (!ready(comparisonQuality) || runtime.uploadedQualityMetric != settings.quality.metric))) {
        settings.mode = originalActive(settings) ? ComparisonMode::original : ComparisonMode::repaired;
    }
    const auto detectorReady = [&](DiagnosticCategory category) { return comparisonDetectorReady(runtime, state, id, category, true); };
    settings.showBoundaries &= detectorReady(DiagnosticCategory::boundary);
    settings.showNonManifold &= detectorReady(DiagnosticCategory::nonManifold);
    settings.showWinding &= detectorReady(DiagnosticCategory::winding);
    settings.topologyInspection.nonManifoldVertices = detectorReady(DiagnosticCategory::nonManifoldVertices);
    settings.topologyInspection.holes = detectorReady(DiagnosticCategory::holes);
    settings.topologyInspection.fins = detectorReady(DiagnosticCategory::fins);
    settings.topologyInspection.showFins &= settings.topologyInspection.fins;
    settings.duplicates.points = detectorReady(DiagnosticCategory::duplicatePoints);
    settings.duplicates.triangles = detectorReady(DiagnosticCategory::duplicateTriangles);
    settings.degenerates.enabled = detectorReady(DiagnosticCategory::degenerateTriangles);
    settings.topologyInspection.showHoles &= settings.topologyInspection.holes;
    settings.topologyInspection.showNonManifoldVertices &= settings.topologyInspection.nonManifoldVertices;
    settings.duplicates.showPoints &= settings.duplicates.points;
    settings.duplicates.showTriangles &= settings.duplicates.triangles;
    settings.degenerates.show &= settings.degenerates.enabled;
    settings.intersections.show &= ready(comparisonIntersections);
    return settings;
}

static void destroyStage(ComparisonGpuSurface& gpu, uint32_t stages)
{
    const auto destroy = [](auto& handle) { if (woby::graphics::isValid(handle)) { woby::graphics::destroy(handle); } handle = WOBY_GPU_INVALID_HANDLE; };
    if (stages & comparisonSource) { destroy(gpu.vertices); destroy(gpu.triangles); destroy(gpu.lines); destroy(gpu.quality); }
    if (stages & comparisonTopology) { for (auto* h : {&gpu.boundaries,&gpu.nonManifold,&gpu.winding,&gpu.nonManifoldVertices,&gpu.holes,&gpu.finEdges,&gpu.finFill}) { destroy(*h); } }
    if (stages & comparisonDuplicatePoints) { destroy(gpu.duplicatePoints); }
    if (stages & comparisonDuplicateTriangles) { destroy(gpu.duplicateTriangleEdges); destroy(gpu.duplicateTriangleFill); }
    if (stages & comparisonDegenerates) { destroy(gpu.degenerateEdges); destroy(gpu.degenerateFill); }
    if (stages & comparisonIntersections) { destroy(gpu.intersectionEdges); destroy(gpu.intersectionFill); }
    if (stages & comparisonDistance) { destroy(gpu.samples); }
    if (stages & comparisonQuality) { destroy(gpu.quality); }
}

static void updateComparisonRuntime(ComparisonRuntime& runtime, UiState& state, SceneObjectId id, bool allowStart)
{
    const auto settings = comparisonSettings(state, id);
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    const uint64_t wanted = comparisonGeometrySignature(state, id);
    auto& job = runtime.intersection;
    const auto phase = [&](IntersectionPhase value, const std::string& error = std::string{}) {
        for (auto* surface : {&runtime.result.original, &runtime.result.repaired}) {
            surface->intersections.phase = value; surface->intersections.error = error;
        }
    };
    const auto invalidateGpu = [&](uint32_t stages) {
        destroyStage(runtime.originalGpu, stages); destroyStage(runtime.repairedGpu, stages);
        runtime.uploadedStages &= ~stages;
    };
    const auto invalidateIntersection = [&] {
        job.stop.request_stop(); ++job.revision; job.requested = false; job.canceled = false;
        runtime.cache.completed &= ~comparisonIntersections;
        invalidateGpu(comparisonIntersections);
        for (auto* surface : {&runtime.result.original, &runtime.result.repaired}) {
            surface->intersections.phase = surface->intersections.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
            surface->intersections.error.clear();
            surface->intersectionBounds.clear(); surface->intersectionEdges.clear();
        }
    };
    if (job.limits != settings.intersections.limits) {
        job.limits = settings.intersections.limits;
        invalidateIntersection();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (resetComparisonCache(runtime.cache, wanted)) {
        runtime.preparationStop.request_stop();
        runtime.prepared.reset();
        runtime.stop.request_stop(); invalidateIntersection();
        invalidateComparisonDetectors(runtime.result, comparisonDetectors);
        auto detectors = std::move(runtime.result.detectors);
        auto a = std::move(runtime.result.original.intersections), b = std::move(runtime.result.repaired.intersections);
        destroySurface(runtime.originalGpu); destroySurface(runtime.repairedGpu);
        runtime.result = {}; runtime.result.original.intersections = std::move(a); runtime.result.repaired.intersections = std::move(b);
        runtime.result.detectors = std::move(detectors);
        runtime.inputs.reset(); runtime.uploadedStages = runtime.failedStages = 0;
        runtime.retryDetectorsSeparately = false;
        runtime.resultSignature = runtime.attemptedSignature = 0; runtime.error.clear();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (runtime.preparationWorker.valid() && runtime.preparationWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto prepared = runtime.preparationWorker.get();
            if (!runtime.preparationStop.stop_requested() && runtime.preparationSignature == wanted) {
                runtime.prepared = std::move(prepared);
                runtime.inputs = runtime.prepared->meshes;
            }
        } catch (const std::exception& error) {
            if (!runtime.preparationStop.stop_requested() && runtime.preparationSignature == wanted) {
                runtime.error = error.what();
                runtime.attemptedSignature = wanted;
                runtime.failedStages |= comparisonSource;
            }
        }
    }
    if (resetComparisonTopologyCache(runtime.cache, settings.topologyMode)) {
        if (runtime.workerStages & comparisonTopology) { runtime.stop.request_stop(); }
        invalidateIntersection(); invalidateGpu(comparisonTopology);
        invalidateComparisonDetectors(runtime.result, comparisonTopology);
        runtime.failedStages &= ~comparisonTopology; runtime.attemptedSignature = 0; runtime.error.clear();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (resetComparisonDegenerateCache(runtime.cache, settings.degenerates)) {
        if (runtime.workerStages & comparisonDegenerates) { runtime.stop.request_stop(); }
        invalidateGpu(comparisonDegenerates); runtime.failedStages &= ~comparisonDegenerates;
        invalidateComparisonDetectors(runtime.result, comparisonDegenerates);
        runtime.attemptedSignature = 0; runtime.error.clear(); resetComparisonDiagnosticFocus(state, id);
    }
    if (runtime.holeSizeRatioTolerance != settings.topologyInspection.holeSizeRatioTolerance) {
        runtime.holeSizeRatioTolerance = settings.topologyInspection.holeSizeRatioTolerance;
        auto& status = runtime.result.detectors[static_cast<size_t>(DiagnosticCategory::holes)];
        status.phase = status.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
        status.error.clear();
    }
    if (runtime.finMaxAreaRatio != settings.topologyInspection.finMaxAreaRatio) {
        runtime.finMaxAreaRatio = settings.topologyInspection.finMaxAreaRatio;
        auto& status = runtime.result.detectors[static_cast<size_t>(DiagnosticCategory::fins)];
        status.phase = status.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
        status.error.clear();
    }
    uint32_t queuedStages = 0;
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        auto& status = runtime.result.detectors[i];
        const auto& request = comparison->detectorRequests[i];
        if (request.revision != runtime.consumedDetectorRequests[i]) {
            runtime.consumedDetectorRequests[i] = request.revision;
            if (request.revision != 0) {
                status.phase = request.cancel ? IntersectionPhase::canceled : wanted ? IntersectionPhase::queued : IntersectionPhase::notChecked;
                status.error.clear();
            }
        }
        if (wanted && (settings.enabled || runtime.fullResultsRequested)
            && diagnosticAutoUpdate(settings, static_cast<DiagnosticCategory>(i))
            && (status.phase == IntersectionPhase::notChecked || status.phase == IntersectionPhase::outdated)) {
            status.phase = IntersectionPhase::queued;
        }
        if (status.phase == IntersectionPhase::queued) { queuedStages |= comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)); }
    }
    const auto workerParticipates = [&](size_t i) {
        return (runtime.workerDetectors & (1u << i)) && runtime.result.detectors[i].phase == IntersectionPhase::running
            && runtime.workerDetectorRequests[i] == runtime.consumedDetectorRequests[i];
    };
    if (runtime.worker.valid() && runtime.workerDetectors) {
        bool any = false;
        for (size_t i = 0; i < backgroundDetectorCount; ++i) { any |= workerParticipates(i); }
        if (!any) { runtime.stop.request_stop(); }
    }
    const auto requeueCanceledWorker = [&] {
        // A threshold/mode edit can stop a batch containing other detectors.
        // Retry its remaining participants; explicit cancellations/new requests
        // have already changed phase/revision and must not be overwritten.
        for (size_t i = 0; i < backgroundDetectorCount; ++i) {
            if (workerParticipates(i)) {
                runtime.result.detectors[i].phase = IntersectionPhase::queued;
                queuedStages |= comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i));
            }
        }
    };
    if (comparison->intersectionRequestRevision != job.consumedRequest) {
        job.consumedRequest = comparison->intersectionRequestRevision;
        if (job.consumedRequest != 0) {
            job.stop.request_stop(); ++job.revision;
            job.requested = !comparison->cancelIntersections && wanted != 0;
            job.canceled = comparison->cancelIntersections;
            runtime.cache.completed &= ~comparisonIntersections; runtime.failedStages &= ~comparisonIntersections;
            invalidateGpu(comparisonIntersections);
            phase(job.canceled ? IntersectionPhase::canceled : job.requested ? IntersectionPhase::queued : IntersectionPhase::notChecked);
        }
    }
    if (settings.intersections.autoUpdate != job.autoUpdate) {
        job.autoUpdate = settings.intersections.autoUpdate;
    }
    if (runtime.worker.valid() && runtime.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto update = runtime.worker.get();
            if (!runtime.stop.stop_requested()) {
                auto previous = runtime.result.detectors;
                uint32_t participants = 0;
                for (size_t i = 0; i < backgroundDetectorCount; ++i) { if (workerParticipates(i)) { participants |= 1u << i; } }
                if (applyComparisonStages(runtime.result, runtime.cache, std::move(update), runtime.workerSignature, runtime.workerStages)) {
                    runtime.resultsRevision = nextResultsRevision();
                    invalidateGpu(runtime.workerStages);
                    runtime.resultSignature = wanted; runtime.failedStages &= ~runtime.workerStages;
                    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                        if (!(participants & (1u << i))) { runtime.result.detectors[i] = std::move(previous[i]); }
                    }
                    if (!runtime.failedStages) { runtime.error.clear(); }
                }
            } else { requeueCanceledWorker(); }
        } catch (const std::exception& error) {
            if (!runtime.stop.stop_requested() && wanted == runtime.workerSignature) {
                const auto detectors = runtime.workerStages & comparisonDetectors;
                if (detectors && (detectors & (detectors - 1))) {
                    // Identify the failing stage without discarding independent
                    // results or labeling every detector in the batch failed.
                    runtime.retryDetectorsSeparately = true;
                    requeueCanceledWorker();
                } else {
                    runtime.error = error.what(); runtime.failedStages |= runtime.workerStages;
                    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                        if (workerParticipates(i)) { runtime.result.detectors[i].phase = IntersectionPhase::failed; runtime.result.detectors[i].error = error.what(); }
                    }
                }
            } else { runtime.attemptedSignature = 0; requeueCanceledWorker(); }
        }
    }
    if (job.worker.valid() && job.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto update = job.worker.get();
            if (!job.stop.stop_requested() && job.workerRevision == job.revision
                && applyComparisonStages(runtime.result, runtime.cache, std::move(update), job.workerSignature, comparisonIntersections)) {
                runtime.resultSignature = wanted; runtime.resultsRevision = nextResultsRevision();
            }
        } catch (const std::exception& error) {
            if (!job.stop.stop_requested() && job.workerRevision == job.revision && job.workerSignature == wanted) { phase(IntersectionPhase::failed, error.what()); }
        }
    }
    if (wanted && settings.intersections.autoUpdate && !job.canceled && !job.worker.valid()
        && !(runtime.cache.completed & comparisonIntersections) && runtime.result.original.intersections.phase != IntersectionPhase::failed) {
        job.requested = true;
    }
    if (job.requested) { phase(IntersectionPhase::queued); }
    auto resultSettings = settings;
    resultSettings.duplicates.points = resultSettings.duplicates.triangles = true;
    resultSettings.degenerates.enabled = true;
    resultSettings.topologyInspection.nonManifoldVertices = resultSettings.topologyInspection.holes = resultSettings.topologyInspection.fins = true;
    setComparisonDuplicateEnabled(runtime.result, resultSettings.duplicates);
    setComparisonDegenerateSettings(runtime.result, resultSettings.degenerates);
    setComparisonIntersectionSettings(runtime.result, settings.intersections);
    if (setComparisonTopologyInspectionSettings(runtime.result, resultSettings.topologyInspection)) { runtime.resultsRevision = nextResultsRevision(); invalidateGpu(comparisonTopology); }
    // Filtering may change the count from the worker's default hole threshold.
    auto& holes = runtime.result.detectors[static_cast<size_t>(DiagnosticCategory::holes)];
    if (holes.phase == IntersectionPhase::complete) {
        holes.knownCounts = {runtime.result.original.topology.holes.size(), runtime.result.repaired.topology.holes.size()};
    }
    auto& fins = runtime.result.detectors[static_cast<size_t>(DiagnosticCategory::fins)];
    if (fins.phase == IntersectionPhase::queued && (runtime.cache.completed & comparisonTopology)
        && !(runtime.failedStages & comparisonTopology)) {
        fins.phase = IntersectionPhase::complete; fins.hasResult = true;
        queuedStages = 0;
        for (size_t i = 0; i < backgroundDetectorCount; ++i) {
            if (runtime.result.detectors[i].phase == IntersectionPhase::queued) {
                queuedStages |= comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i));
            }
        }
    }
    if (fins.phase == IntersectionPhase::complete) {
        fins.knownCounts = {runtime.result.original.topology.fins.size(), runtime.result.repaired.topology.fins.size()};
    }
    const bool active = settings.enabled || runtime.fullResultsRequested || job.requested || queuedStages;
    // Begin CPU work before staging the previous result on the GPU.
    // Source upload and detector computation use independent snapshots.
    const auto schedule = [&] {
        const bool both = enabledComparisonPartCount(state, ComparisonSide::a, id) && enabledComparisonPartCount(state, ComparisonSide::b, id);
        const auto requested = requestedComparisonStages(settings, both, runtime.fullResultsRequested) & ~(comparisonDetectors | comparisonIntersections);
        const auto missing = (requested & ~runtime.cache.completed & ~runtime.failedStages) | queuedStages;
        if (!wanted || !allowStart || !active || (!missing && !job.requested)) { return; }
        try {
            if (!runtime.inputs) {
                if (isUvAnalysis(settings.type)) {
                    if (!runtime.preparationWorker.valid()) {
                        auto snapshot = snapshotComparisonInputs(state, id);
                        runtime.preparationStop = std::stop_source{};
                        runtime.preparationSignature = runtime.attemptedSignature = wanted;
                        runtime.preparationWorker = std::async(std::launch::async,
                            [snapshot = std::move(snapshot), stop = runtime.preparationStop.get_token()]() -> std::shared_ptr<const PreparedComparisonInputs> {
                                return std::make_shared<PreparedComparisonInputs>(prepareUvComparisonInputs(snapshot, stop));
                            });
                    }
                    return;
                }
                auto inputs = std::make_shared<std::array<Mesh, 2>>();
                if (enabledComparisonPartCount(state, ComparisonSide::a, id)) { (*inputs)[0] = comparisonWorldMesh(state, ComparisonSide::a, id); }
                if (enabledComparisonPartCount(state, ComparisonSide::b, id)) { (*inputs)[1] = comparisonWorldMesh(state, ComparisonSide::b, id); }
                runtime.inputs = std::move(inputs);
            }
            if (missing && !runtime.worker.valid()) {
                runtime.attemptedSignature = runtime.workerSignature = wanted;
                auto stages = nextComparisonStage(missing);
                if (runtime.retryDetectorsSeparately && (stages & comparisonDetectors)) {
                    stages &= (~stages + 1); // Retry only the first requested stage.
                }
                runtime.attemptedStages = runtime.workerStages = stages;
                runtime.workerDetectors = 0;
                for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                    auto& status = runtime.result.detectors[i];
                    if (status.phase == IntersectionPhase::queued && (comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)) & runtime.workerStages)) {
                        status.phase = IntersectionPhase::running;
                        runtime.workerDetectors |= 1u << i;
                        runtime.workerDetectorRequests[i] = runtime.consumedDetectorRequests[i];
                    }
                }
                runtime.stop = std::stop_source{};
                runtime.worker = std::async(std::launch::async, [inputs = runtime.inputs, stage = runtime.workerStages,
                    degenerates = settings.degenerates, mode = settings.topologyMode, stop = runtime.stop.get_token()] {
                    return computeComparisonStages((*inputs)[0], (*inputs)[1], stage, stop, degenerates, mode);
                });
            } else if (job.requested && !runtime.worker.valid() && !job.worker.valid()) {
                job.requested = false; job.workerSignature = wanted; job.workerRevision = job.revision;
                job.stop = std::stop_source{}; job.started = std::chrono::steady_clock::now(); phase(IntersectionPhase::running);
                job.worker = std::async(std::launch::async, [inputs = runtime.inputs, mode = settings.topologyMode, limits = settings.intersections.limits, stop = job.stop.get_token()] {
                    return computeComparisonStages((*inputs)[0], (*inputs)[1], comparisonIntersections, stop, {}, mode, limits);
                });
            }
        } catch (const std::exception& error) {
            if (job.requested) { job.requested = false; phase(IntersectionPhase::failed, error.what()); }
            else {
                runtime.error = error.what(); runtime.failedStages |= missing;
                for (auto& status : runtime.result.detectors) {
                    if (status.phase == IntersectionPhase::queued || status.phase == IntersectionPhase::running) {
                        status.phase = IntersectionPhase::failed; status.error = error.what();
                    }
                }
            }
        }
    };
    schedule();
    if (wanted && active) {
        const auto pending = runtime.cache.completed & ~runtime.uploadedStages & ~runtime.failedStages;
        for (const auto stage : {comparisonSource, comparisonTopology, comparisonDuplicatePoints, comparisonDuplicateTriangles,
            comparisonDegenerates, comparisonQuality, comparisonDistance, comparisonIntersections}) {
            if (!(pending & stage)) { continue; }
            try {
                uploadSurface(runtime.originalGpu, runtime.result.original, stage, runtime.prepared ? &runtime.prepared->buffers[0] : nullptr);
                uploadSurface(runtime.repairedGpu, runtime.result.repaired, stage, runtime.prepared ? &runtime.prepared->buffers[1] : nullptr);
                runtime.uploadedStages |= stage;
                if (stage & comparisonSource) { runtime.prepared.reset(); }
            } catch (const std::exception& error) {
                invalidateGpu(stage); runtime.failedStages |= stage; runtime.attemptedSignature = wanted;
                if (stage == comparisonIntersections) { phase(IntersectionPhase::failed, error.what()); }
                else {
                    runtime.error = error.what();
                    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                        if ((comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)) & stage)
                            && runtime.result.detectors[i].phase == IntersectionPhase::complete) {
                            runtime.result.detectors[i].phase = IntersectionPhase::failed;
                            runtime.result.detectors[i].error = error.what();
                        }
                    }
                }
            }
        }
        if ((runtime.cache.completed & comparisonQuality) && settings.mode == ComparisonMode::surfaceQuality
            && !(runtime.failedStages & comparisonQuality) && (runtime.uploadedQualityMetric != settings.quality.metric
                || (!runtime.result.original.source.indices.empty() && !woby::graphics::isValid(runtime.originalGpu.quality))
                || (!runtime.result.repaired.source.indices.empty() && !woby::graphics::isValid(runtime.repairedGpu.quality)))) {
            try {
                const auto& distribution = runtime.result.qualityDistributions.at(static_cast<size_t>(settings.quality.metric));
                uploadQuality(runtime.originalGpu, runtime.result.original, settings.quality.metric, distribution);
                uploadQuality(runtime.repairedGpu, runtime.result.repaired, settings.quality.metric, distribution);
                runtime.uploadedQualityMetric = settings.quality.metric;
            } catch (const std::exception& error) { runtime.failedStages |= comparisonQuality; runtime.error = error.what(); }
        }
    }
    runtime.ready = wanted && settings.enabled && (runtime.cache.completed & comparisonSource) && (runtime.uploadedStages & comparisonSource);

}

static void destroyComparisonRuntime(ComparisonRuntime &runtime)
{
    runtime.preparationStop.request_stop();
    if (runtime.preparationWorker.valid()) { runtime.preparationWorker.wait(); }
    runtime.intersection.stop.request_stop();
    if (runtime.intersection.worker.valid()) { runtime.intersection.worker.wait(); }
    if (runtime.worker.valid())
    {
        runtime.stop.request_stop();
        runtime.worker.wait();
    }
    destroySurface(runtime.originalGpu);
    destroySurface(runtime.repairedGpu);
}

void updateComparisonRuntimes(ComparisonRuntimes& runtimes, UiState& state)
{
    for (auto it = runtimes.objects.begin(); it != runtimes.objects.end();) {
        if (!findComparison(state, it->first)) {
            destroyComparisonRuntime(it->second);
            it = runtimes.objects.erase(it);
        } else { ++it; }
    }
    for (const auto& comparison : state.comparisons) {
        auto& runtime = runtimes.objects[comparison.objectId];
        size_t active = 0;
        for (const auto& item : runtimes.objects) {
            active += item.second.preparationWorker.valid();
            active += item.second.worker.valid(); active += item.second.intersection.worker.valid();
        }
        updateComparisonInspectorCache(runtime.inspector, state, comparison.objectId);
        updateComparisonRuntime(runtime, state, comparison.objectId, active < 2);
        for (size_t index = 0; index < diagnosticCategoryCount; ++index) {
            const auto category = static_cast<DiagnosticCategory>(index);
            const auto phase = comparisonDetectorStatus(runtime.result, category).phase;
            const auto stage = comparisonDiagnosticStage(category);
            const bool current = runtime.resultSignature && runtime.resultSignature == runtime.cache.signature
                && (runtime.cache.completed & stage) == stage;
            runtime.diagnosticSummaries[index] = {
                diagnosticSummary(runtime.result.original, category, phase, current),
                diagnosticSummary(runtime.result.repaired, category, phase, current)};
        }
        validateComparisonDiagnosticFocus(state, runtime.result,
            runtime.ready ? runtime.resultSignature : 0, comparison.objectId);
        validateUvFindingFocus(state, runtime.result.original.source,
            runtime.ready ? runtime.resultSignature : 0, comparison.objectId);
    }
}

void destroyComparisonRuntimes(ComparisonRuntimes& runtimes)
{
    for (auto& [id, runtime] : runtimes.objects) { (void)id; runtime.preparationStop.request_stop(); runtime.stop.request_stop(); }
    for (auto& [id, runtime] : runtimes.objects) { (void)id; destroyComparisonRuntime(runtime); }
    runtimes.objects.clear();
    if (woby::graphics::isValid(runtimes.program)) { woby::graphics::destroy(runtimes.program); runtimes.program = WOBY_GPU_INVALID_HANDLE; }
    if (woby::graphics::isValid(runtimes.parameters)) { woby::graphics::destroy(runtimes.parameters); runtimes.parameters = WOBY_GPU_INVALID_HANDLE; }
}

bool comparisonsReadyForScreenshot(const UiState& state, const ComparisonRuntimes& runtimes)
{
    bool ready = true;
    for (const auto& comparison : state.comparisons) {
        if (!comparison.settings.enabled) { continue; }
        if (!canInspectComparison(state, comparison.objectId)) {
            std::string issues;
            for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
                const auto input = comparisonInputSummary(state, side, comparison.objectId);
                if (!input.issue.empty()) { issues += " " + input.issue; }
            }
            throw std::runtime_error(comparison.name + ":" + issues);
        }
        const auto signature = comparisonGeometrySignature(state, comparison.objectId);
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it == runtimes.objects.end()) { ready = false; continue; }
        const auto& runtime = it->second;
        if (!runtime.error.empty() && runtime.attemptedSignature == signature
            && !comparisonResultsReady(runtime, state, comparison.objectId)) {
            throw std::runtime_error(comparison.name + ": " + runtime.error);
        }
        ready = ready && comparisonResultsReady(runtime, state, comparison.objectId);
        const auto phase = runtime.result.original.intersections.phase;
        if (comparison.settings.intersections.show && (runtime.intersection.requested
            || phase == IntersectionPhase::queued || phase == IntersectionPhase::running
            || findComparison(state, comparison.objectId)->intersectionRequestRevision != runtime.intersection.consumedRequest)) { ready = false; }
        if (comparison.settings.mode == ComparisonMode::surfaceQuality) {
            ready = ready && runtime.uploadedQualityMetric == comparison.settings.quality.metric &&
                (runtime.result.original.source.indices.empty() || woby::graphics::isValid(runtime.originalGpu.quality)) &&
                (runtime.result.repaired.source.indices.empty() || woby::graphics::isValid(runtime.repairedGpu.quality));
        }
    }
    return ready;
}

namespace {
void drawSurfaceQualitySizeLimits(UiState& state, SceneObjectId id, const MeshComparison* result)
{
    ImGui::Separator();
    ImGui::TextUnformatted("Size limits (longest edge)");
    ImGui::SameLine();
    drawInformationIcon("size_limits_info", "Size limits",
        "Limits use each triangle's longest edge, with inclusive endpoints. "
        "Counts and percentages exclude degenerate faces. Limits do not change heatmap colors.");
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    ImGui::Checkbox("Minimum##quality", &settings.quality.minimumEnabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(!settings.quality.minimumEnabled);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputFloat("##quality_minimum", &settings.quality.minimumSize, 0, 0, "%.5g");
    ImGui::EndDisabled();
    ImGui::Checkbox("Maximum##quality", &settings.quality.maximumEnabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(!settings.quality.maximumEnabled);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputFloat("##quality_maximum", &settings.quality.maximumSize, 0, 0, "%.5g");
    ImGui::EndDisabled();
    if (settings != initial) { setComparisonSettings(state, settings, id); }
    const auto& quality = comparisonSettings(state, id).quality;
    if (!quality.minimumEnabled && !quality.maximumEnabled) {
        ImGui::TextDisabled("Enable a limit to see outside-limit statistics.");
    } else if (result && ImGui::BeginTable("quality_size_limits", 2,
                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Statistic", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
        const auto limits = surfaceQualitySizeLimits(result->original.quality, quality);
        const auto row = [&](const char* label, const auto& value) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value(limits).c_str());
        };
        if (quality.minimumEnabled) { row("Below minimum", [](const auto& l) { return std::to_string(l.below); }); }
        if (quality.maximumEnabled) { row("Above maximum", [](const auto& l) { return std::to_string(l.above); }); }
        row("Outside: faces", [](const auto& l) { return l.validTriangles ? measurementNumber(l.trianglePercent) + "%" : "N/A"; });
        row("Outside: area", [](const auto& l) { return l.validTriangles ? measurementNumber(l.areaPercent) + "%" : "N/A"; });
        ImGui::EndTable();
    }
    ImGui::Separator();
}

void drawSurfaceQualityStatistics(const MeshComparison& result, const ComparisonSettings& settings)
{
    const auto metric = settings.quality.metric;
    const auto index = static_cast<size_t>(metric);
    const auto& distribution = result.qualityDistributions[index];
    ImGui::Separator();
    ImGui::TextUnformatted("Surface mesh quality");
    ImGui::SameLine();
    drawInformationIcon("quality_info", "Surface mesh quality",
        "Longest edge: largest triangle span. Equivalent size: edge of an equilateral triangle with the same area.\n\n"
        "Shape: 4*sqrt(3)*area / sum(squared edges); 1 = equilateral, 0 = collapsed.\n\n"
        "Local size jump: largest equivalent-size ratio across valid manifold neighbors. Boundary and non-manifold "
        "edges are excluded; no neighbor = unavailable. Coincident positions are matched.\n\n"
        "Statistics count valid source triangles equally; percentiles use linear interpolation. "
        "Degenerate faces are excluded and reported separately. Size colors describe size, not FEM accuracy.");
    ImGui::TextUnformatted(surfaceQualityMetricName(metric));
    const float legend = drawSurfaceQualityLegend(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(),
        ImGui::GetContentRegionAvail().x, ImGui::GetFontSize(), metric, distribution);
    ImGui::Dummy({0, legend});
    ImGui::TextWrapped("Magenta: degenerate. Gray: unavailable.");
    if (ImGui::BeginTable("quality_statistics", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Statistic", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
        const auto row = [&](const char* label, const auto& value) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value(result.original.quality).c_str());
        };
        row("Triangles", [](const auto& q) { return std::to_string(q.triangles.size()); });
        row("Degenerate", [](const auto& q) { return std::to_string(q.degenerateTriangles); });
        row("Measured faces", [&](const auto& q) { return std::to_string(q.statistics[index].count); });
        const auto statisticRow = [&](const char* name, double QualityStatistics::*field) {
            row(name, [&](const auto& q) { return q.statistics[index].count ? measurementNumber(q.statistics[index].*field) : "N/A"; });
        };
        statisticRow("Minimum", &QualityStatistics::minimum);
        statisticRow("P5", &QualityStatistics::percentile5);
        statisticRow("Median", &QualityStatistics::median);
        statisticRow("P95", &QualityStatistics::percentile95);
        statisticRow("Maximum", &QualityStatistics::maximum);
        row("Worst shape", [](const auto& q) { return q.statistics[2].count ? measurementNumber(q.statistics[2].minimum) : "N/A"; });
        row("Max size jump", [](const auto& q) { return q.statistics[3].count ? measurementNumber(q.statistics[3].maximum) : "N/A"; });
        ImGui::EndTable();
    }
    ImGui::TextUnformatted("Distribution (% of measured faces)");
    const auto position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x), height = ImGui::GetFontSize() * 5;
    auto& draw = *ImGui::GetWindowDrawList();
    const auto percent = [&](size_t bin) {
        return distribution.counts[0] ? 100.0 * static_cast<double>(distribution.bins[0][bin]) /
            static_cast<double>(distribution.counts[0]) : 0;
    };
    double peak = 0;
    for (size_t bin = 0; bin < surfaceQualityBinCount; ++bin) { peak = std::max(peak, percent(bin)); }
    const float binWidth = width / static_cast<float>(surfaceQualityBinCount);
    draw.AddRectFilled(position, {position.x + width, position.y + height}, IM_COL32(30, 34, 40, 255));
    for (size_t bin = 0; bin < surfaceQualityBinCount; ++bin) {
        const float barHeight = peak > 0 ? height * static_cast<float>(percent(bin) / peak) : 0;
        const float x = position.x + static_cast<float>(bin) * binWidth;
        draw.AddRectFilled({x, position.y + height - barHeight}, {x + binWidth * .9f, position.y + height},
            IM_COL32(77, 166, 255, 255));
    }
    ImGui::InvisibleButton("quality_histogram", {width, height});
    if (ImGui::IsItemHovered()) {
        const auto bin = std::min(static_cast<size_t>(std::max(0.0f, ImGui::GetIO().MousePos.x - position.x) / binWidth),
            surfaceQualityBinCount - 1);
        const double step = (distribution.maximum - distribution.minimum) / surfaceQualityBinCount;
        ImGui::BeginTooltip();
        ImGui::Text("[%.5g, %.5g%s", distribution.minimum + static_cast<double>(bin) * step,
            distribution.minimum + static_cast<double>(bin + 1) * step, bin + 1 == surfaceQualityBinCount ? "]" : ")");
        ImGui::Text("%zu faces (%.2f%%)", distribution.bins[0][bin], percent(bin));
        ImGui::EndTooltip();
    }
    ImGui::TextWrapped("%.5g to %.5g; vertical maximum %.3g%%", distribution.minimum, distribution.maximum, peak);
}

void drawUvFindings(UiState& state, const Mesh& display, uint64_t signature, SceneObjectId id)
{
    const auto& quality = *display.uvQuality;
    ImGui::SeparatorText("Findings");
    ImGui::TextWrapped("%zu triangles with findings", quality.findings.size());
    if (quality.findings.empty()) { ImGui::TextDisabled("No findings in the checked UV categories."); return; }
    bool scrollToFocus = false;
    if (ImGui::Button("Previous finding")) { navigateUvFinding(state, display, signature, -1, id); scrollToFocus = true; }
    ImGui::SameLine();
    if (ImGui::Button("Next finding")) { navigateUvFinding(state, display, signature, 1, id); scrollToFocus = true; }
    const auto* focus = focusedUvFinding(state, signature, id);
    if (focus) {
        const auto& triangle = quality.triangles[quality.findings[focus->index]];
        const auto source = findSceneObject(state, triangle.partId);
        ImGui::TextWrapped("%zu / %zu | %s / triangle %zu", focus->index + 1, quality.findings.size(),
            source ? source->name.c_str() : "Missing patch", triangle.triangle);
        if (triangle.missing) { ImGui::TextUnformatted("Missing UVs"); }
        if (triangle.collapsed) { ImGui::TextUnformatted("Collapsed UV triangle"); }
        if (triangle.degenerateSurface) { ImGui::TextUnformatted("Degenerate surface triangle"); }
        if (triangle.mixedOrientation) { ImGui::TextUnformatted("Mixed orientation within this patch"); }
        if (!triangle.missing && !triangle.collapsed && !triangle.degenerateSurface) {
            ImGui::TextWrapped("Angle: %.4g deg | log2 area ratio: %.4g", triangle.angleDegrees, triangle.areaLog2);
        }
    }
    ImGui::TextWrapped("Selecting a finding frames and highlights its triangle. Missing or collapsed UVs open in 3D.");
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::BeginChild("UV finding list", {0, rowHeight * 7}, ImGuiChildFlags_Borders)) {
        if (scrollToFocus && focus) { ImGui::SetScrollY(static_cast<float>(focus->index) * rowHeight); }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(std::min(quality.findings.size(), static_cast<size_t>(INT_MAX))), rowHeight);
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const auto index = static_cast<size_t>(row);
                const auto& triangle = quality.triangles[quality.findings[index]];
                const auto label = std::to_string(index + 1) + " | " + uvFindingLabel(triangle, quality.settings);
                // Selection can replace the focus, so never retain its pointer across a click.
                const auto* selected = focusedUvFinding(state, signature, id);
                if (ImGui::Selectable(label.c_str(), selected && selected->index == index)) {
                    selectUvFinding(state, display, signature, index, id);
                }
                if (ImGui::IsItemHovered()) {
                    const auto source = findSceneObject(state, triangle.partId);
                    ImGui::SetTooltip("%s / triangle %zu", source ? source->name.c_str() : "Missing patch", triangle.triangle);
                }
            }
        }
    }
    ImGui::EndChild();
}

void drawUvInspector(UiState& state, ComparisonRuntime& runtime, SceneObjectId id)
{
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    ImGui::SeparatorText("Tool");
    if (ImGui::RadioButton("Layout", settings.type == AnalysisType::uv)) { settings.type = AnalysisType::uv; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Distortion", settings.type == AnalysisType::uvQuality)) { settings.type = AnalysisType::uvQuality; }
    ImGui::TextUnformatted("View");
    int viewMode = static_cast<int>(settings.uvView);
    const char* views[] = {"2D UV layout", "3D surface"};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##uv_view", &viewMode, views, 2)) { settings.uvView = static_cast<UvView>(viewMode); }
    if (settings.type == AnalysisType::uvQuality) {
        const auto* quality = inspectorStagesReady(runtime, state, id, comparisonSource)
            ? runtime.result.original.source.uvQuality.get() : nullptr;
        drawUvQualityControls(state, settings, quality, id);
    } else {
        drawVisibilityField("UV coloring", settings.uvGrid.enabled);
        int mode = static_cast<int>(settings.uvGrid.mode);
        const char* modes[] = {"Grid", "U gradient", "V gradient"};
        ImGui::TextUnformatted("Color"); ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##uv_color", &mode, modes, 3)) { settings.uvGrid.mode = static_cast<UvColorMode>(mode); }
        if (mode == 0) {
            ImGui::InputFloat("U cells / UV unit", &settings.uvGrid.densityU, 0, 0, "%.5g");
            ImGui::InputFloat("V cells / UV unit", &settings.uvGrid.densityV, 0, 0, "%.5g");
        } else {
            ImGui::InputFloat("Range minimum", &settings.uvGrid.minimum, 0, 0, "%.5g");
            ImGui::InputFloat("Range maximum", &settings.uvGrid.maximum, 0, 0, "%.5g");
        }
    }
    if (ImGui::CollapsingHeader("Display options")) {
        ImGui::BeginDisabled(settings.uvView != UvView::layout);
        ImGui::Checkbox("Separate patches", &settings.uvSeparated);
        ImGui::EndDisabled();
        ImGui::Checkbox("Link patch selection to source", &settings.uvLinkedSelection);
        drawVisibilityField("Triangle edges", settings.showEdges);
        if (ImGui::Button("Show all patches")) { isolateUvObjects(state, {}, id); }
        ImGui::TextWrapped("Right-click an input to isolate it. Layout separation preserves a common scale and the original UVs.");
    }
    if (settings != initial) { setComparisonSettings(state, settings, id); }
    ImGui::SeparatorText("Results");
    ImGui::BeginDisabled(inspectorQueries(runtime, state, id).signature == 0);
    if (ImGui::Button("Full result")) { frameComparison(state, id); }
    ImGui::SameLine();
    if (ImGui::Button("Fit scene")) { frameCameraToScene(state); }
    ImGui::EndDisabled();
    if (settings.type == AnalysisType::uvQuality) {
        ImGui::TextUnformatted("Legend"); ImGui::SameLine();
        drawInformationIcon("uv_legend_info", "UV colors", uvQualityLegend(settings.uvMetric));
        const float height = drawUvQualityLegend(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(),
            ImGui::GetContentRegionAvail().x, ImGui::GetFontSize(), settings.uvMetric);
        ImGui::Dummy({0, height});
        ImGui::PushTextWrapPos();
        ImGui::TextColored({1, 0, 1, 1}, "Collapsed UV");
        ImGui::TextColored({.56f, .61f, .67f, 1}, "Missing UV / degenerate surface");
        ImGui::PopTextWrapPos();
    } else if (settings.uvGrid.mode == UvColorMode::grid) {
        ImGui::TextColored({.12f, .78f, .92f, 1}, "Cyan: constant U");
        ImGui::TextColored({1, .55f, .16f, 1}, "Orange: constant V");
    } else {
        ImGui::TextWrapped("Blue: %.5g | Yellow: %.5g", settings.uvGrid.minimum, settings.uvGrid.maximum);
    }
    if (!inspectorStagesReady(runtime, state, id, comparisonSource)) {
        ImGui::TextDisabled("Results are not current. See activity above."); return;
    }
    const auto& display = runtime.result.original.source;
    if (settings.type == AnalysisType::uvQuality && display.uvQuality) {
        const auto& quality = *display.uvQuality;
        if (ImGui::BeginTable("UV result summary", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 4);
            const auto row = [](const char* label, size_t count) {
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", label);
                ImGui::TableNextColumn(); ImGui::Text("%zu", count);
            };
            row("Triangles checked", quality.triangles.size());
            row("Collapsed UV triangles", quality.collapsed);
            row("Patches with mixed orientation", quality.mixedOrientationPatches);
            row("Missing UV triangles", quality.missing);
            row("Degenerate surface triangles", quality.degenerateSurface);
            ImGui::EndTable();
        }
        drawUvFindings(state, display, runtime.resultSignature, id);
    } else if (display.nodes.empty()) {
        ImGui::TextWrapped("No UV coordinates to display. Add a source with UVs or choose the 3D surface view.");
    }
}

void drawComparisonContents(UiState &state, ComparisonRuntime &runtime, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    auto task = comparisonTask(state, id);
    std::array<char, 512> name{};
    std::copy_n(comparison->name.data(), std::min(comparison->name.size(), name.size() - 1), name.data());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##comparison_name", name.data(), name.size())) { renameComparison(state, id, name.data()); }
    if (isSingleSourceMeshTask(task)) {
        const AnalysisTask tasks[] = {AnalysisTask::meshChecks, AnalysisTask::meshQuality};
        const char* labels[] = {"Mesh checks", "Mesh quality"};
        int selectedTask = task == AnalysisTask::meshQuality ? 1 : 0;
        ImGui::TextUnformatted("Analysis task");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##analysis_task", &selectedTask, labels, 2)) {
            setAnalysisTask(state, id, tasks[selectedTask]);
            task = tasks[selectedTask];
        }
    } else { ImGui::TextUnformatted(analysisTaskLabel(task)); }
    ImGui::SeparatorText(isSingleSourceMeshTask(task) ? "Sources" : "Inputs");
    ImGui::SameLine();
    drawInformationIcon("comparison_info", "Analysis inputs",
        isSingleSourceMeshTask(task)
            ? "Drag models onto Sources, or use Analysis membership in the model context menu. Hidden models are included. Expand Sources to enable, isolate, or remove parts."
            : "Drag models onto an input, or use Analysis membership in the model context menu. Hidden source models are included. Expand an input to enable, isolate, or remove its parts.");
    membershipTree(state, runtime, isSingleSourceMeshTask(task) && comparison->a.empty() && !comparison->b.empty() ? ComparisonSide::b : ComparisonSide::a, id);
    if (isUvAnalysis(comparison->settings.type)) {
        drawUvInspector(state, runtime, id);
        return;
    }
    if (task == AnalysisTask::surfaceComparison) { membershipTree(state, runtime, ComparisonSide::b, id); }
    const bool hasA = inspectorEnabledPartCount(runtime, state, ComparisonSide::a, id) != 0;
    const bool hasB = inspectorEnabledPartCount(runtime, state, ComparisonSide::b, id) != 0;
    const bool both = hasA && hasB;
    const bool resultReady = inspectorStagesReady(runtime, state, id, comparisonDistance);
    ImGui::Separator();
    if (task == AnalysisTask::surfaceComparison && both) {
        if (ImGui::Button("Swap inputs A / B")) { swapComparisonGroups(state, id); }
        setLastItemTooltip("Exchange inputs A and B while keeping the measurement direction.");
    }
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    const bool valid = (inspectorQueries(runtime, state, id).signature != 0);
    {
        if (task == AnalysisTask::surfaceComparison) {
            ImGui::SeparatorText("Display");
            const char* modes[] = {"Distance heatmap", "Input A", "Input B", "Overlay"};
            int mode = settings.mode == ComparisonMode::surfaceQuality ? 0 : static_cast<int>(settings.mode);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##comparison_mode", &mode, modes, 4)) {
                settings.task = AnalysisTask::surfaceComparison;
                settings.mode = static_cast<ComparisonMode>(mode);
                resetComparisonDiagnosticFocus(state, id);
            }
            if (!both) { ImGui::TextWrapped("Add a second input for surface comparison. Available input geometry remains visible."); }
        }
        if (both && settings.mode == ComparisonMode::distance)
        {
            ImGui::TextUnformatted("Measurement direction");
            ImGui::SameLine();
            const auto a = comparisonInputSummary(state, ComparisonSide::a, id);
            const auto b = comparisonInputSummary(state, ComparisonSide::b, id);
            const std::string directionHint = std::string("Measured surface (heatmap): ")
                + (settings.distanceOnOriginal ? "A - " : "B - ")
                + (settings.distanceOnOriginal ? a.sourceNames : b.sourceNames)
                + "\n\nReference surface (nearest distance): "
                + (settings.distanceOnOriginal ? "B - " : "A - ")
                + (settings.distanceOnOriginal ? b.sourceNames : a.sourceNames);
            drawInformationIcon("direction_info", "Measurement direction", directionHint.c_str());
            if (ImGui::RadioButton("A -> B", settings.distanceOnOriginal)) { settings.distanceOnOriginal = true; }
            setLastItemTooltip("Measure distances from vertices on A to the nearest surface on B.");
            ImGui::SameLine();
            if (ImGui::RadioButton("B -> A", !settings.distanceOnOriginal)) { settings.distanceOnOriginal = false; }
            setLastItemTooltip("Measure distances from vertices on B to the nearest surface on A.");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            ImGui::InputFloat("Tolerance", &settings.tolerance, 0, 0, "%.5g");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            ImGui::InputFloat("Color maximum", &settings.colorRange, 0, 0, "%.5g");
            ImGui::BeginDisabled(!resultReady || !(runtime.cache.completed & comparisonDistance));
            if (ImGui::Button("Fit color range")) {
                const auto& surface = settings.distanceOnOriginal ? runtime.result.original : runtime.result.repaired;
                settings.colorRange = std::max(static_cast<float>(surface.maximum), settings.tolerance);
            }
            setLastItemTooltip("Fit the color maximum to the largest measured distance, with tolerance as the minimum.");
            ImGui::EndDisabled();

        }
        if (task == AnalysisTask::meshQuality) {
            ImGui::SeparatorText("Metric");
            int metric = static_cast<int>(settings.quality.metric);
            const char* metrics[] = {"Longest edge", "Equivalent size", "Shape quality", "Local size jump"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##quality_metric", &metric, metrics, 4)) {
                settings.quality.metric = static_cast<SurfaceQualityMetric>(metric);
            }
        }
        if (both && settings.mode == ComparisonMode::overlay)
        {
            drawInformationIcon("overlay_info", "Overlay colors",
                "Blue: group A wireframe (X-ray).\n\nSolid gray: group B surface.");
        }
        drawVisibilityField("Triangle edges", settings.showEdges);
    }
    // Merely opening the panel must not change scene settings.
    if (settings != initial)
    {
        setComparisonSettings(state, settings, id);
    }
    if (task != AnalysisTask::meshChecks) {
        ImGui::SeparatorText("Results");
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Full result")) { frameComparison(state, id); }
        ImGui::EndDisabled();
    }
    if (both && comparisonSettings(state, id).mode == ComparisonMode::distance) {
        const auto currentSettings = comparisonSettings(state, id);
        ImGui::TextUnformatted("Distance");
        ImGui::SameLine();
        drawInformationIcon("distance_info", "Distance colors and statistics",
            currentSettings.colorRange == currentSettings.tolerance ?
            "Gray: at or below tolerance. Above tolerance saturates orange-red.\n\n"
            "Approximate unsigned distance, four samples per triangle; not an exact maximum. Surface shading affects brightness." :
            "Gray: at or below tolerance. >= color maximum saturates orange-red.\n\n"
            "Approximate unsigned distance, four samples per triangle; not an exact maximum. Surface shading affects brightness.");
        const float legendHeight = drawComparisonLegend(*ImGui::GetWindowDrawList(), ImGui::GetCursorScreenPos(),
            ImGui::GetContentRegionAvail().x, ImGui::GetFontSize(), currentSettings);
        ImGui::Dummy({0, legendHeight});
    }
    const bool validVisible = valid && comparisonSettings(state, id).enabled;
    if (task == AnalysisTask::meshChecks) { drawDiagnosticNavigation(state, runtime, validVisible, hasA, hasB, id); }
    if (!validVisible) { return; }
    if (task == AnalysisTask::meshQuality) {
        const bool qualityReady = inspectorStagesReady(runtime, state, id, comparisonQuality);
        if (qualityReady) { drawSurfaceQualityStatistics(runtime.result, comparisonSettings(state, id)); }
        drawSurfaceQualitySizeLimits(state, id, qualityReady ? &runtime.result : nullptr);
    }
    // Diagnostic focus temporarily changes the drawn surface, not the measured direction.
    const bool useOriginal = !hasB || (hasA && originalActive(comparisonSettings(state, id)));
    const auto &surface = useOriginal ? runtime.result.original : runtime.result.repaired;
    if (both && comparisonSettings(state, id).mode == ComparisonMode::distance && inspectorStagesReady(runtime, state, id, comparisonDistance))
    {
        if (surface.maximum > comparisonSettings(state, id).colorRange) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1, .65f, .25f, 1), "SATURATED: sample max exceeds color maximum");
            ImGui::PopTextWrapPos();
        }
        ImGui::TextWrapped("Sample max: %.5g", surface.maximum);
        ImGui::TextWrapped("Area-weighted mean: %.5g", surface.mean);
        ImGui::TextWrapped("Area-weighted P95: %.5g", surface.percentile95);
        ImGui::TextWrapped("Area above tolerance: %.2f%%", surfacePercentAboveTolerance(surface, comparisonSettings(state, id).tolerance));
    }
    if (task != AnalysisTask::meshChecks) { return; }
    const auto &a = runtime.result.original.diagnostics;
    const auto &b = runtime.result.repaired.diagnostics;
    if (!inspectorStagesReady(runtime, state, id, comparisonTopology)) { return; }
    if (both) {
        ImGui::TextWrapped("Numerically collapsed triangles: %zu -> %zu", a.degenerateTriangles, b.degenerateTriangles);
        ImGui::TextWrapped("Geometric duplicate triangles: %zu -> %zu", a.duplicateTriangles, b.duplicateTriangles);
    } else {
        ImGui::TextWrapped("Numerically collapsed triangles: %zu", surface.diagnostics.degenerateTriangles);
        ImGui::TextWrapped("Geometric duplicate triangles: %zu", surface.diagnostics.duplicateTriangles);
    }
}

} // namespace

void drawComparisonPanelContents(UiState& state, ComparisonRuntimes& runtimes)
{
    const auto* comparison = selectedComparison(state);
    if (!comparison) { return; }
    const auto id = comparison->objectId;
    ImGui::PushID(std::to_string(id).c_str());
    drawComparisonActivity(state, runtimes.objects[id], id);
    if (ImGui::BeginChild("comparison_properties")) {
        drawComparisonContents(state, runtimes.objects[id], id);
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Placement")) {
            auto translation = findComparison(state, id)->translation;
            ImGui::TextWrapped("Display offset only; source measurements stay unchanged.");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat3("Result position", translation.data(), .1f)) {
                setComparisonTranslation(state, id, translation);
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

static void submitComparisonScene(woby::graphics::ViewId view, const UiComparison& comparison, const ComparisonRuntime& runtime,
                           const ComparisonRuntimes& runtimes,
                           woby::graphics::ProgramHandle colorProgram, woby::graphics::UniformHandle colorUniform, const ComparisonSettings& settings, woby::graphics::ProgramHandle markerProgram)
{
    if (!comparison.settings.enabled || !runtime.ready) { return; }
    const bool useOriginal = originalActive(settings);
    const auto &gpu = useOriginal ? runtime.originalGpu : runtime.repairedGpu;
    const bool quality = settings.mode == ComparisonMode::surfaceQuality;
    const bool heatmap = settings.mode == ComparisonMode::distance || quality;
    if (quality && (!woby::graphics::isValid(gpu.quality) || runtime.uploadedQualityMetric != settings.quality.metric)) { return; }
    float identity[16];
    bx::mtxTranslate(identity, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
    if (isUvAnalysis(settings.type)) {
        if (!woby::graphics::isValid(gpu.vertices) || !woby::graphics::isValid(gpu.triangles)) { return; }
        const std::array<float, 4> gray = {.58f, .63f, .69f, 1};
        for (const auto& node : runtime.result.original.source.nodes) {
            const bool uvQuality = settings.type == AnalysisType::uvQuality;
            const auto uv = uvQuality ? std::array<float,4>{0,0,6,settings.uvMetric == UvQualityMetric::area ? 1.0f : 0.0f} : uvColorParameters(settings.uvGrid,node.hasTexcoords,true);
            woby::graphics::setTransform(identity);
            woby::graphics::setUniform(runtimes.parameters, uv.data());
            woby::graphics::setUniform(colorUniform, gray.data());
            if (uvQuality) { woby::graphics::setVertexBuffer(0, gpu.quality, node.indexOffset, node.indexCount); }
            else {
                woby::graphics::setVertexBuffer(0, gpu.vertices);
                woby::graphics::setIndexBuffer(gpu.triangles, node.indexOffset, node.indexCount);
            }
            setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_WRITE_Z |
                WOBY_GPU_STATE_DEPTH_TEST_LEQUAL | WOBY_GPU_STATE_MSAA, woby::graphics::isValid(markerProgram));
            woby::graphics::submit(view, woby::graphics::isValid(markerProgram) ? markerProgram : runtimes.program);
        }
        if (settings.showEdges) { submitWire(view, gpu, colorProgram, colorUniform, {.22f, .25f, .30f, 1}, false, identity); }
        return;
    }
    const std::array<float, 4> parameters = {settings.tolerance, settings.colorRange, quality ? 2.0f : heatmap ? 1.0f : 0.0f,
        quality && settings.quality.metric == SurfaceQualityMetric::shape ? 1.0f : 0.0f};
    const std::array<float, 4> gray = {.58f, .63f, .69f, 1};
    woby::graphics::setTransform(identity);
    woby::graphics::setUniform(runtimes.parameters, parameters.data());
    woby::graphics::setUniform(colorUniform, gray.data());
    woby::graphics::setVertexBuffer(0, quality ? gpu.quality : heatmap ? gpu.samples : gpu.vertices);
    if (!heatmap)
    {
        woby::graphics::setIndexBuffer(gpu.triangles);
    }
    setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_WRITE_Z | WOBY_GPU_STATE_DEPTH_TEST_LEQUAL |
                   WOBY_GPU_STATE_MSAA, woby::graphics::isValid(markerProgram));
    woby::graphics::submit(view, woby::graphics::isValid(markerProgram) ? markerProgram : runtimes.program);
    if (settings.showEdges)
    {
        submitWire(view, gpu, colorProgram, colorUniform, {.22f, .25f, .30f, 1}, false, identity);
    }
    if (settings.mode == ComparisonMode::overlay)
    {
        submitWire(view, runtime.originalGpu, colorProgram, colorUniform, {.3f, .75f, 1, 1}, true, identity);
    }
    if (settings.intersections.show) {
        if (woby::graphics::isValid(gpu.intersectionFill)) {
            const std::array<float, 4> red = {1, .2f, .15f, .45f};
            woby::graphics::setTransform(identity); woby::graphics::setVertexBuffer(0, gpu.intersectionFill);
            woby::graphics::setUniform(colorUniform, red.data());
            setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_DEPTH_TEST_ALWAYS | WOBY_GPU_STATE_BLEND_ALPHA | WOBY_GPU_STATE_MSAA, woby::graphics::isValid(markerProgram));
            woby::graphics::submit(view, colorProgram);
        }
        submitEdges(view, gpu.intersectionEdges, colorProgram, colorUniform, {1, .2f, .15f, 1}, identity);
    }
    if (settings.degenerates.enabled && settings.degenerates.show) {
        if (woby::graphics::isValid(gpu.degenerateFill)) {
            const std::array<float, 4> purple = {.8f, .25f, 1, .45f};
            woby::graphics::setTransform(identity); woby::graphics::setVertexBuffer(0, gpu.degenerateFill);
            woby::graphics::setUniform(colorUniform, purple.data());
            setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_DEPTH_TEST_ALWAYS | WOBY_GPU_STATE_BLEND_ALPHA | WOBY_GPU_STATE_MSAA, woby::graphics::isValid(markerProgram));
            woby::graphics::submit(view, colorProgram);
        }
        submitEdges(view, gpu.degenerateEdges, colorProgram, colorUniform, {.8f, .25f, 1, 1}, identity);
    }
    if (settings.duplicates.triangles && settings.duplicates.showTriangles) {
        if (woby::graphics::isValid(gpu.duplicateTriangleFill)) {
            const std::array<float, 4> orange = {1, .45f, .08f, .4f};
            woby::graphics::setTransform(identity); woby::graphics::setVertexBuffer(0, gpu.duplicateTriangleFill);
            woby::graphics::setUniform(colorUniform, orange.data());
            setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_DEPTH_TEST_ALWAYS | WOBY_GPU_STATE_BLEND_ALPHA | WOBY_GPU_STATE_MSAA, woby::graphics::isValid(markerProgram));
            woby::graphics::submit(view, colorProgram);
        }
        submitEdges(view, gpu.duplicateTriangleEdges, colorProgram, colorUniform, {1, .45f, .08f, 1}, identity);
    }
    if (settings.duplicates.points && settings.duplicates.showPoints) {
        submitEdges(view, gpu.duplicatePoints, colorProgram, colorUniform, {.1f, .85f, 1, 1}, identity);
    }
    if (settings.showBoundaries)
    {
        submitEdges(view, gpu.boundaries, colorProgram, colorUniform, {.2f, 1, .6f, 1}, identity);
    }
    if (settings.showNonManifold)
    {
        submitEdges(view, gpu.nonManifold, colorProgram, colorUniform, {1, .15f, .55f, 1}, identity);
    }
    if (settings.topologyInspection.nonManifoldVertices && settings.topologyInspection.showNonManifoldVertices) {
        submitEdges(view, gpu.nonManifoldVertices, colorProgram, colorUniform, {1, .65f, .05f, 1}, identity);
    }
    if (settings.topologyInspection.fins && settings.topologyInspection.showFins) {
        if (woby::graphics::isValid(gpu.finFill)) {
            const float color[] = {.2f, .9f, .65f, .35f};
            woby::graphics::setTransform(identity); woby::graphics::setUniform(colorUniform, color);
            woby::graphics::setVertexBuffer(0, gpu.finFill);
            setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_DEPTH_TEST_LEQUAL | WOBY_GPU_STATE_BLEND_ALPHA, woby::graphics::isValid(markerProgram));
            woby::graphics::submit(view, colorProgram);
        }
        submitEdges(view, gpu.finEdges, colorProgram, colorUniform, {.2f, .9f, .65f, 1}, identity);
    }
    if (settings.topologyInspection.holes && settings.topologyInspection.showHoles) {
        submitEdges(view, gpu.holes, colorProgram, colorUniform, {.1f, .65f, 1, 1}, identity);
    }
    if (settings.showWinding) {
        submitEdges(view, gpu.winding, colorProgram, colorUniform, {1, .2f, .2f, 1}, identity);
    }
}

void appendVisibleComparisonPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    const ComparisonRuntimes& runtimes)
{
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it != runtimes.objects.end() && comparisonStagesReady(it->second, state, comparison.objectId, comparisonSource, true)) {
            const auto first = parts.size();
            appendComparisonPickParts(parts, comparison, readyComparisonSettings(it->second, state, comparison.objectId),
                it->second.result, sceneObjectSelected(state, comparison.objectId));
            for (size_t i = first; i < parts.size(); ++i) {
                if (parts[i].objectId != comparison.objectId) { parts[i].selected = sceneObjectSelected(state,parts[i].objectId); }
            }
        }
    }
}

void submitComparisonScenes(woby::graphics::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    woby::graphics::ProgramHandle colorProgram, woby::graphics::UniformHandle colorUniform, SceneRenderScratch& scratch, woby::graphics::ProgramHandle markerProgram)
{
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it != runtimes.objects.end() && comparisonStagesReady(it->second, state, comparison.objectId, comparisonSource, true)) {
            auto settings = readyComparisonSettings(it->second, state, comparison.objectId);
            if (it->second.resultSignature != comparisonGeometrySignature(state, comparison.objectId)) {
                settings.duplicates.showPoints = false; settings.duplicates.showTriangles = false;
            }
            submitComparisonScene(view, comparison, it->second, runtimes, colorProgram, colorUniform, settings, markerProgram);
        }
    }
    // Submit focus last so surfaces and other diagnostic edges cannot obscure it.
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it == runtimes.objects.end() || !it->second.ready) { continue; }
        const auto* edge = focusedComparisonDiagnostic(state, it->second.result,
            it->second.resultSignature, comparison.objectId);
        const auto* uv = focusedUvFinding(state, it->second.resultSignature, comparison.objectId);
        if (!edge && !uv) { continue; }
        auto& points = scratch.focusPoints;
        auto& faceFill = scratch.positions;
        points.clear();
        faceFill.clear();
        const auto focus = comparison.diagnosticFocus.value_or(DiagnosticFocus{});
        const bool duplicatePoints = focus.category == DiagnosticCategory::duplicatePoints;
        const bool duplicateTriangles = focus.category == DiagnosticCategory::duplicateTriangles;
        const float radius = state.camera.distance * .015f;
        if (uv) {
            faceFill.assign(uv->geometry->begin(), uv->geometry->end());
            for (size_t k = 0; k < 3; ++k) {
                points.push_back((*uv->geometry)[k]); points.push_back((*uv->geometry)[(k + 1) % 3]);
            }
            appendCross(points, (*uv->geometry)[0], radius);
        } else if (duplicatePoints || duplicateTriangles) {
            const auto& finding = comparisonDuplicates(it->second.result, focus.side, focus.category).findings.at(focus.index);
            if (duplicatePoints) {
                for (const auto& point : finding.geometry) { appendCross(points, point, radius); }
            } else {
                faceFill = finding.geometry;
                for (size_t i = 0; i < finding.geometry.size(); i += 3) {
                    for (size_t k = 0; k < 3; ++k) { points.push_back(finding.geometry[i+k]); points.push_back(finding.geometry[i+(k+1)%3]); }
                }
            }
        } else if (focus.category == DiagnosticCategory::selfIntersections) {
            const auto& surface = focus.side == ComparisonSide::a ? it->second.result.original : it->second.result.repaired;
            const auto& finding = surface.intersections.findings.at(focus.index);
            faceFill.insert(faceFill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t i = 0; i < 6; i += 3) {
                for (size_t k = 0; k < 3; ++k) { points.push_back(finding.geometry[i+k]); points.push_back(finding.geometry[i+(k+1)%3]); }
            }
        } else if (focus.category == DiagnosticCategory::degenerateTriangles) {
            const auto& surface = focus.side == ComparisonSide::a ? it->second.result.original : it->second.result.repaired;
            const auto& finding = surface.degenerates.findings.at(focus.index);
            faceFill.assign(finding.geometry.begin(), finding.geometry.end());
            for (size_t k = 0; k < 3; ++k) { points.push_back(finding.geometry[k]); points.push_back(finding.geometry[(k+1)%3]); }
            if (finding.reasons.collapsed) { appendCross(points, finding.geometry[0], radius); }
        } else if (focus.category == DiagnosticCategory::fins) {
            const auto& topology = (focus.side == ComparisonSide::a ? it->second.result.original : it->second.result.repaired).topology;
            const auto& patch = topology.finPatches[topology.fins.at(focus.index)];
            const auto& source = topology.sources[patch.source];
            for (const auto f : patch.faces) {
                for (const auto v : source.faces[f].vertices) {
                    const auto& p = source.vertices[v].position;
                    faceFill.push_back({static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])});
                }
            }
        } else if (focus.category == DiagnosticCategory::nonManifoldVertices || focus.category == DiagnosticCategory::holes) {
            const auto& topology = (focus.side == ComparisonSide::a ? it->second.result.original : it->second.result.repaired).topology;
            const auto point = [](const auto& p) { return std::array<float, 3>{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])}; };
            if (focus.category == DiagnosticCategory::nonManifoldVertices) {
                const auto& finding = topology.nonManifoldVertices.at(focus.index);
                const auto& source = topology.sources[finding.source];
                const auto& vertex = source.vertices[finding.vertex];
                appendCross(points, point(vertex.position), radius);
                for (const auto f : vertex.faces) {
                    for (const auto v : source.faces[f].vertices) { faceFill.push_back(point(source.vertices[v].position)); }
                }
            } else {
                const auto& boundary = topology.boundaryRegions[topology.holes.at(focus.index)];
                const auto& source = topology.sources[boundary.source];
                for (const auto e : boundary.edges) {
                    for (const auto v : source.edges[e].vertices) { points.push_back(point(source.vertices[v].position)); }
                }
            }
        } else {
            points.push_back(edge->a); points.push_back(edge->b);
            for (const auto& endpoint : {edge->a, edge->b}) { appendCross(points, endpoint, radius); }
            const auto& surface = focus.side == ComparisonSide::a ? it->second.result.original : it->second.result.repaired;
            const auto& findings = topologyFindings(surface.topology, focus.category);
            if (focus.index < findings.size()) {
                const auto& finding = findings[focus.index];
                const auto& source = surface.topology.sources[finding.source];
                for (const auto& use : source.edges[finding.edge].incidentFaces) {
                    for (const auto vertex : source.faces[use.face].vertices) {
                        const auto& p = source.vertices[vertex].position;
                        faceFill.push_back({static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])});
                    }
                }
            }
        }
        // Portable thick lines: graphics line primitives are only one pixel wide on
        // some backends and would merge into the yellow object-selection outline.
        auto& triangles = faceFill;
        triangles.reserve(triangles.size() + points.size()*3);
        const auto direction = bx::sub(cameraEye(state.camera, state.upAxis), cameraLookAt(state.camera));
        const float halfWidth = state.camera.distance * .0025f;
        for (size_t i = 0; i < points.size(); i += 2) {
            const auto& a = points[i];
            const auto& b = points[i + 1];
            const auto normal = bx::cross(bx::Vec3{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, direction);
            const float length = bx::length(normal);
            if (length <= 0.0f) { continue; }
            const auto offset = bx::mul(normal, halfWidth / length);
            const std::array<float, 3> a0 = {a[0] - offset.x, a[1] - offset.y, a[2] - offset.z};
            const std::array<float, 3> a1 = {a[0] + offset.x, a[1] + offset.y, a[2] + offset.z};
            const std::array<float, 3> b0 = {b[0] - offset.x, b[1] - offset.y, b[2] - offset.z};
            const std::array<float, 3> b1 = {b[0] + offset.x, b[1] + offset.y, b[2] + offset.z};
            triangles.insert(triangles.end(), {a0, a1, b0, a1, b1, b0});
        }
        const auto layout = helperLineVertexLayout();
        const auto vertexCount = static_cast<uint32_t>(triangles.size());
        if (vertexCount == 0) { continue; }
        if (woby::graphics::getAvailTransientVertexBuffer(vertexCount, layout) < vertexCount) { continue; }
        woby::graphics::TransientVertexBuffer buffer;
        woby::graphics::allocTransientVertexBuffer(&buffer, vertexCount, layout);
        std::memcpy(buffer.data, triangles.data(), triangles.size() * sizeof(triangles[0]));
        float transform[16];
        bx::mtxTranslate(transform, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
        const std::array<float, 4> yellow = {1, 1, .1f, 1};
        woby::graphics::setTransform(transform);
        woby::graphics::setUniform(colorUniform, yellow.data());
        woby::graphics::setVertexBuffer(0, &buffer);
        woby::graphics::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A |
            WOBY_GPU_STATE_DEPTH_TEST_ALWAYS | WOBY_GPU_STATE_MSAA);
        woby::graphics::submit(view, colorProgram);
    }
}

void drawAnalysisCreationMenu(UiState& state)
{
    drawAnalysisCreationMenu(state, state.selectedSceneObjects);
}

void drawAnalysisCreationMenu(UiState& state, const std::vector<SceneObjectId>& sources)
{
    for (const auto task : {AnalysisTask::meshChecks, AnalysisTask::meshQuality,
            AnalysisTask::surfaceComparison, AnalysisTask::uvInspection}) {
        const bool allowed = task != AnalysisTask::surfaceComparison || sources.size() <= 2;
        const char* label = task == AnalysisTask::surfaceComparison ? "Surface comparison" : analysisTaskLabel(task);
        if (ImGui::MenuItem(label, nullptr, false, allowed)) { createAnalysisFromObjects(state, task, sources); }
        if (!allowed) { setLastItemTooltip("Select up to two sources, in A then B order."); }
    }
}

static void drawComparisonObjectHint(const UiState& state, const UiComparison& comparison, const ComparisonRuntime* runtime)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) { return; }
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
    ImGui::TextUnformatted(comparison.name.c_str());
    const auto id = comparison.objectId;
    const auto& settings = comparison.settings;
    const bool currentInputs = runtime && comparisonInspectorCacheCurrent(runtime->inspector, state, id);
    const bool hasA = currentInputs && runtime->inspector.inputs[0].summary.enabledPartCount != 0;
    const bool hasB = currentInputs && runtime->inspector.inputs[1].summary.enabledPartCount != 0;
    const auto task = comparisonTask(state, id);
    ImGui::TextDisabled("%s", analysisTaskLabel(task));
    ImGui::SameLine();
    const char* status = "Updating...";
    if (currentInputs) {
        if (!hasA && !hasB) { status = "Needs inputs"; }
        else if (task == AnalysisTask::surfaceComparison && !(hasA && hasB)) { status = "Needs second input"; }
        else if (!canInspectComparison(state, id)) { status = "Missing source"; }
        else if (!runtime->error.empty()) { status = "Failed"; }
        else if (runtime->preparationWorker.valid() || runtime->worker.valid() || runtime->intersection.worker.valid()) { status = "Running"; }
        else if (!runtime->ready) { status = settings.enabled ? "Queued" : "Not run"; }
        else { status = "Ready"; }
        if (task == AnalysisTask::meshChecks && runtime->ready && canInspectComparison(state, id)) {
            for (const auto phase : {AnalysisResultState::notRun, AnalysisResultState::unavailable,
                    AnalysisResultState::partial, AnalysisResultState::canceled, AnalysisResultState::outdated,
                    AnalysisResultState::queued, AnalysisResultState::running, AnalysisResultState::failed}) {
                for (const auto& check : runtime->diagnosticSummaries) {
                    if ((hasA && check[0].state == phase) || (hasB && check[1].state == phase)) {
                        status = phase == AnalysisResultState::notRun ? "Checks pending" : analysisResultStateLabel(phase);
                        break;
                    }
                }
            }
        }
    }
    ImGui::TextDisabled("| %s", status);
    if (currentInputs) {
        const auto& a = runtime->inspector.inputs[0].summary;
        const auto& b = runtime->inspector.inputs[1].summary;
        if (task == AnalysisTask::meshChecks && (hasA || hasB)) {
            size_t withFindings = 0, notRun = 0, incomplete = 0;
            for (const auto& check : runtime->diagnosticSummaries) {
                const bool finding = (hasA && check[0].count != 0) || (hasB && check[1].count != 0);
                withFindings += finding;
                notRun += (hasA && check[0].state == AnalysisResultState::notRun)
                    || (hasB && check[1].state == AnalysisResultState::notRun);
                const auto unfinished = [](AnalysisResultState phase) {
                    return phase != AnalysisResultState::ready && phase != AnalysisResultState::notRun;
                };
                incomplete += (hasA && unfinished(check[0].state)) || (hasB && unfinished(check[1].state));
            }
            ImGui::TextWrapped("%zu checks with findings | %zu not run%s", withFindings, notRun,
                incomplete ? " | incomplete results" : "");
        }
        ImGui::TextWrapped("Sources: %s", runtime->inspector.sources.c_str());
        if (!canInspectComparison(state, id)) {
            if (!a.issue.empty()) { ImGui::TextWrapped("%s", a.issue.c_str()); }
            if (!b.issue.empty()) { ImGui::TextWrapped("%s", b.issue.c_str()); }
        }
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Select this analysis. Double-click to rename.");
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void drawComparisonObjects(UiState& state, ComparisonNameEdit& edit, const ComparisonRuntimes& runtimes)
{
    if (edit.lastFrame != ImGui::GetFrameCount() - 1
        || (edit.objectId != invalidSceneObjectId && !findComparison(state, edit.objectId))) {
        edit = {};
    }
    edit.lastFrame = ImGui::GetFrameCount();
    const auto beginRename = [&](SceneObjectId id) {
        if (const auto* comparison = findComparison(state, id)) {
            edit.objectId = id;
            edit.text = comparison->name;
            edit.focus = true;
            selectSceneObject(state, id);
        }
    };
    const bool sceneFocused = ImGui::IsWindowFocused();
    const bool canStartRename = sceneFocused && !ImGui::GetIO().WantTextInput
        && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (canStartRename && edit.objectId == invalidSceneObjectId && state.selectedSceneObjects.size() == 1
        && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        beginRename(state.selectedSceneObjects.front());
    }
    bool open = false;
    if (ImGui::BeginTable("analysis_header", 3, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("visibility", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("create", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        const auto visible = static_cast<size_t>(std::count_if(state.comparisons.begin(), state.comparisons.end(),
            [](const auto& item) { return item.settings.enabled; }));
        if (drawTriStateVisibilityButton("analyses_visible", "Analyses", visible, state.comparisons.size(), "analyses")) {
            setAllComparisonsVisible(state, visible != state.comparisons.size());
        }
        ImGui::TableNextColumn();
        open = ImGui::CollapsingHeader("Analyses", ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::TableNextColumn();
        const auto buttonPosition = ImGui::GetCursorScreenPos();
        if (drawRenderModeIconButton("create_analysis", "+", "Create an analysis", RenderModeState::off, false)) {
            ImGui::OpenPopup("create_analysis");
        }
        ImGui::SetNextWindowPos({buttonPosition.x + renderModeButtonSize(),
            buttonPosition.y + renderModeButtonSize() + ImGui::GetStyle().ItemSpacing.y}, ImGuiCond_Appearing, {1, 0});
        if (ImGui::BeginPopup("create_analysis")) {
            drawAnalysisCreationMenu(state);
            ImGui::EndPopup();
        }
        ImGui::EndTable();
    }
    if (!open) { return; }
    if (state.comparisons.empty()) { ImGui::TextDisabled("Select models, then use + to create an analysis."); }
    for (const auto& comparison : state.comparisons) {
        const auto id = comparison.objectId;
        const auto foundRuntime = runtimes.objects.find(id);
        const auto label = std::to_string(id);
        ImGui::PushID(label.c_str());
        auto settings = comparison.settings;
        if (drawVisibilityButton("visible", settings.enabled, "analysis")) {
            settings.enabled = !settings.enabled;
            setComparisonSettings(state, settings, id);
        }
        ImGui::SameLine();
        const float nameWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        bool changed = false;
        if (edit.objectId == id) {
            const bool focusing = edit.focus;
            if (focusing) { ImGui::SetKeyboardFocusHere(); edit.focus = false; }
            ImGui::SetNextItemWidth(nameWidth);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x,
                std::max(0.0f, (renderModeButtonSize() - ImGui::GetTextLineHeight()) * 0.5f)));
            const bool entered = ImGui::InputText("##comparison_name_edit", &edit.text,
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopStyleVar();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                edit.objectId = invalidSceneObjectId;
            } else if (entered || (!focusing && ImGui::IsItemDeactivated())) {
                renameComparison(state, id, edit.text);
                edit.objectId = invalidSceneObjectId;
            }
        } else {
            const bool selected = sceneObjectSelected(state, id);
            if (drawSceneItemButton((comparison.name + "###comparison_row").c_str(), nameWidth, selected)) {
                selectSceneObject(state, id, ImGui::GetIO().KeyCtrl);
            }
            if (selected) { drawSceneItemOutline(); }
            drawComparisonObjectHint(state, comparison, foundRuntime == runtimes.objects.end() ? nullptr : &foundRuntime->second);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                beginRename(id);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectSceneObject(state, id, false, true); }
            if (ImGui::BeginPopupContextItem("comparison_object")) {
                if (ImGui::MenuItem("Properties")) { selectSceneObject(state, id); }
                if (ImGui::MenuItem("Frame result", nullptr, false, canInspectComparison(state, id))) { frameComparison(state, id); }
                ImGui::Separator();
                if (ImGui::MenuItem("Rename", "F2")) { beginRename(id); }
                if (ImGui::MenuItem("Duplicate")) { duplicateComparison(state, id); changed = true; }
                if (ImGui::MenuItem("Delete analysis")) { removeComparison(state, id); changed = true; }
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
        if (changed) { break; }
    }
}
} // namespace woby
