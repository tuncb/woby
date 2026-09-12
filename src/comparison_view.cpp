#include "comparison_view.h"
#include "comparison_scene.h"
#include "comparison_legend.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "utf8_path.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <climits>
#include <stdexcept>

namespace woby
{
namespace
{
void destroySurface(ComparisonGpuSurface &gpu)
{
    for (const auto handle : {gpu.vertices, gpu.samples, gpu.quality, gpu.boundaries, gpu.nonManifold, gpu.winding,
        gpu.duplicatePoints, gpu.duplicateTriangleEdges, gpu.duplicateTriangleFill})
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    for (const auto handle : {gpu.triangles, gpu.lines})
    {
        if (bgfx::isValid(handle))
        {
            bgfx::destroy(handle);
        }
    }
    gpu = {};
}
bgfx::VertexBufferHandle uploadEdges(const std::vector<DiagnosticEdge> &edges)
{
    if (edges.empty())
    {
        return BGFX_INVALID_HANDLE;
    }
    const auto bytes = comparisonBufferBytes(edges.size(), 2 * sizeof(std::array<float, 3>));
    std::vector<std::array<float, 3>> points;
    points.reserve(edges.size() * 2);
    for (const auto &edge : edges)
    {
        points.push_back(edge.a);
        points.push_back(edge.b);
    }
    const auto handle = bgfx::createVertexBuffer(
        bgfx::copy(points.data(), bytes), helperLineVertexLayout());
    if (!bgfx::isValid(handle))
    {
        throw std::runtime_error("Cannot allocate analysis edge buffer.");
    }
    return handle;
}
bgfx::VertexBufferHandle uploadPositions(const std::vector<std::array<float, 3>>& positions)
{
    if (positions.empty()) { return BGFX_INVALID_HANDLE; }
    const auto handle = bgfx::createVertexBuffer(bgfx::copy(positions.data(), comparisonBufferBytes(positions.size(), sizeof(positions[0]))), helperLineVertexLayout());
    if (!bgfx::isValid(handle)) { throw std::runtime_error("Cannot allocate duplicate overlay buffer."); }
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

void uploadSurface(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages)
{
    if (surface.source.indices.empty()) { return; }
    if (stages & comparisonSource) {
        const auto vertexBytes = comparisonBufferBytes(surface.source.vertices.size(), sizeof(Vertex));
        const auto indexBytes = comparisonBufferBytes(surface.source.indices.size(), sizeof(uint32_t));
        const auto lineBytes = comparisonBufferBytes(surface.source.indices.size(), 2 * sizeof(uint32_t));
        auto vertices = surface.source.vertices;
        generateSmoothNormals(vertices, surface.source.indices);
        gpu.vertices = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), vertexBytes), meshVertexLayout());
        const auto& indices = surface.source.indices;
        gpu.triangles = bgfx::createIndexBuffer(bgfx::copy(indices.data(), indexBytes), BGFX_BUFFER_INDEX32);
        std::vector<uint32_t> lines;
        lines.reserve(indices.size() * 2);
        for (size_t i = 0; i < indices.size(); i += 3) {
            for (size_t k = 0; k < 3; ++k) {
                lines.push_back(indices[i + k]);
                lines.push_back(indices[i + (k + 1) % 3]);
            }
        }
        gpu.lines = bgfx::createIndexBuffer(bgfx::copy(lines.data(), lineBytes), BGFX_BUFFER_INDEX32);
        if (!bgfx::isValid(gpu.vertices) || !bgfx::isValid(gpu.triangles) || !bgfx::isValid(gpu.lines)) {
            throw std::runtime_error("Cannot allocate analysis surface buffers.");
        }
    }
    if ((stages & comparisonDistance) && !surface.sampled.vertices.empty()) {
        const auto& samples = surface.sampled.vertices;
        gpu.samples = bgfx::createVertexBuffer(bgfx::copy(samples.data(), comparisonBufferBytes(samples.size(), sizeof(Vertex))), meshVertexLayout());
        if (!bgfx::isValid(gpu.samples)) { throw std::runtime_error("Cannot allocate analysis distance buffer."); }
    }
    if (stages & comparisonTopology) {
        gpu.boundaries = uploadEdges(surface.diagnostics.boundaryEdges);
        gpu.nonManifold = uploadEdges(surface.diagnostics.nonManifoldEdges);
        gpu.winding = uploadEdges(surface.diagnostics.inconsistentWindingEdges);
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
    const auto handle = bgfx::createVertexBuffer(bgfx::copy(vertices.data(), bytes), meshVertexLayout());
    if (!bgfx::isValid(handle)) { throw std::runtime_error("Cannot allocate surface mesh quality buffer."); }
    if (bgfx::isValid(gpu.quality)) { bgfx::destroy(gpu.quality); }
    gpu.quality = handle;
}
void comparisonEnabledCheckbox(UiState& state, ComparisonSide side, SceneObjectId id,
    const std::vector<SceneObjectId>& objects)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    const auto parts = comparisonObjectParts(state, objects);
    size_t total = 0, checked = 0;
    for (const auto& member : members) {
        if (!objects.empty() && !std::binary_search(parts.begin(), parts.end(), member.objectId)) { continue; }
        ++total;
        if (member.enabled) { ++checked; }
    }
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
    const auto nodeId = std::to_string(node.objectId);
    ImGui::PushID(nodeId.c_str());
    comparisonEnabledCheckbox(state, side, id, {node.objectId});
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
        const char* label = side == ComparisonSide::a ? "Remove from group A" : "Remove from group B";
        if (ImGui::MenuItem(label)) { setComparisonObjects(state, {node.objectId}, side, false, id); }
        ImGui::EndPopup();
    }
    if (open && !leaf) {
        for (const auto& child : node.children) { drawComparisonTreeNode(state, side, child, id); }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void membershipTree(UiState& state, ComparisonSide side, SceneObjectId id)
{
    const char* label = side == ComparisonSide::a ? "Group A" : "Group B";
    ImGui::PushID(label);
    comparisonEnabledCheckbox(state, side, id, {});
    const auto summary = comparisonInputSummary(state, side, id);
    const auto roots = comparisonTree(state, side, id);
    size_t triangles = 0;
    for (const auto& root : roots) { triangles += root.triangleCount; }
    const bool open = ImGui::TreeNodeEx("root", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_FramePadding,
        "%s (%zu/%zu %s on) %zu triangles", label, summary.enabledPartCount, summary.partCount,
        summary.partCount == 1 ? "part" : "parts", triangles);
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
        if (ImGui::MenuItem(side == ComparisonSide::a ? "Clear group A" : "Clear group B",
                nullptr, false, !members.empty())) {
            clearComparisonGroup(state, side, id);
        }
        ImGui::EndPopup();
    }
    if (!open) {
        ImGui::PopID();
        return;
    }
    const auto current = comparisonInputSummary(state, side, id);
    if (members.empty()) {
        ImGui::TextDisabled("No input");
    } else if (!current.issue.empty()) { ImGui::TextWrapped("%s", current.issue.c_str()); }
    if (members.size() > current.partCount && ImGui::SmallButton("Remove missing references")) {
        removeMissingComparisonParts(state, side, id);
    }
    for (const auto& root : roots) {
        drawComparisonTreeNode(state, side, root, id);
    }
    ImGui::TreePop();
    ImGui::PopID();
}
void diagnosticRow(UiState& state, const ComparisonRuntime& runtime, bool current,
    const char* name, DiagnosticCategory category, bool hasA, bool hasB, SceneObjectId id)
{
    ImGui::PushID(name);
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    const bool duplicate = category == DiagnosticCategory::duplicatePoints || category == DiagnosticCategory::duplicateTriangles;
    bool enabled = !duplicate || (category == DiagnosticCategory::duplicatePoints ? settings.duplicates.points : settings.duplicates.triangles);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!duplicate);
    ImGui::Checkbox("##run", &enabled);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", duplicate ? "Enable this detector" : "Always included in surface analysis");
    }
    if (category == DiagnosticCategory::duplicatePoints) { settings.duplicates.points = enabled; }
    if (category == DiagnosticCategory::duplicateTriangles) { settings.duplicates.triangles = enabled; }
    ImGui::TableNextColumn();
    // Wrap long edge labels in narrower panels while retaining a selectable row.
    const auto position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x);
    const float height = std::max(ImGui::GetFrameHeight(), ImGui::CalcTextSize(name, nullptr, false, width).y);
    if (ImGui::Selectable("##category", settings.diagnosticCategory == category, 0, {0, height})) {
        settings.diagnosticCategory = category;
    }
    ImGui::GetWindowDrawList()->AddText(nullptr, 0, position, ImGui::GetColorU32(ImGuiCol_Text), name, nullptr, width);
    const bool settingsCurrent = settings.duplicates.points == initial.duplicates.points && settings.duplicates.triangles == initial.duplicates.triangles;
    current = current && settingsCurrent && runtime.resultSignature == comparisonGeometrySignature(state, id);
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        if ((side == ComparisonSide::a && !hasA) || (side == ComparisonSide::b && !hasB)) { continue; }
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        if (!enabled) { ImGui::TextDisabled("Off"); }
        else if (!current) { ImGui::TextDisabled("%s", runtime.error.empty() ? "..." : "Failed"); }
        else if (!duplicate) { ImGui::Text("%zu", comparisonDiagnosticEdges(runtime.result, side, category).size()); }
        else {
            const auto& result = comparisonDuplicates(runtime.result, side, category);
            if (result.unavailableSources && !result.availableSources) { ImGui::TextDisabled("N/A"); }
            else { ImGui::Text("%zu%s", result.duplicateCount, result.unavailableSources || result.informationalCount ? "*" : ""); }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%zu extra records in %zu groups\n%zu informational STL corners\n%zu unavailable sources\nStatus: %s",
                    result.duplicateCount, result.findings.size(), result.informationalCount, result.unavailableSources, duplicateStatus(result));
            }
        }
    }
    ImGui::TableNextColumn();
    bool visible = category == DiagnosticCategory::boundary ? settings.showBoundaries
        : category == DiagnosticCategory::duplicatePoints ? settings.duplicates.showPoints
        : category == DiagnosticCategory::duplicateTriangles ? settings.duplicates.showTriangles : settings.showNonManifold;
    if (ImGui::Checkbox("##show", &visible)) {
        if (category == DiagnosticCategory::boundary) { settings.showBoundaries = visible; }
        else if (category == DiagnosticCategory::duplicatePoints) { settings.duplicates.showPoints = visible; }
        else if (category == DiagnosticCategory::duplicateTriangles) { settings.duplicates.showTriangles = visible; }
        else { settings.showNonManifold = visible; }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", category == DiagnosticCategory::nonManifold || category == DiagnosticCategory::winding
            ? "Show non-manifold and winding edges (shared visibility)" : "Show this diagnostic overlay");
    }
    if (settings != initial) { setComparisonSettings(state, settings, id); }
    ImGui::TableNextColumn();
    const auto& findings = comparisonDiagnosticEdges(runtime.result, settings.diagnosticSide, category);
    ImGui::BeginDisabled(!current || !enabled || findings.empty());
    int step = 0;
    if (ImGui::ArrowButton("previous", ImGuiDir_Left)) { step = -1; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Previous %s on %s", duplicate ? "duplicate group" : "edge", settings.diagnosticSide == ComparisonSide::a ? "A" : "B");
    }
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
    if (ImGui::ArrowButton("next", ImGuiDir_Right)) { step = 1; }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Next %s on %s", duplicate ? "duplicate group" : "edge", settings.diagnosticSide == ComparisonSide::a ? "A" : "B");
    }
    ImGui::EndDisabled();
    if (step != 0) {
        if (settings.diagnosticCategory != category) {
            settings.diagnosticCategory = category;
            setComparisonSettings(state, settings, id);
        }
        navigateComparisonDiagnostic(state, runtime.result, runtime.resultSignature, step, id);
    }
    ImGui::PopID();
}

void drawDuplicateFindings(UiState& state, const ComparisonRuntime& runtime, bool current, SceneObjectId id)
{
    const auto settings = comparisonSettings(state, id);
    const bool hasTarget = enabledComparisonPartCount(state, settings.diagnosticSide, id) != 0;
    const bool points = settings.diagnosticCategory == DiagnosticCategory::duplicatePoints;
    if (!points && settings.diagnosticCategory != DiagnosticCategory::duplicateTriangles) { return; }
    const auto& result = comparisonDuplicates(runtime.result, settings.diagnosticSide, settings.diagnosticCategory);
    if (!current || !hasTarget || !(points ? settings.duplicates.points : settings.duplicates.triangles)) { return; }
    if (result.unavailableSources) {
        ImGui::TextWrapped("%zu source(s) unavailable: %s", result.unavailableSources,
            points ? "original source records were not retained." : "STL has no shared point IDs, or source records were not retained.");
    }
    ImGui::TextWrapped("%zu extra %s in %zu group%s%s", result.duplicateCount, points ? "points" : "triangles",
        result.findings.size(), result.findings.size() == 1 ? "" : "s", result.informationalCount ? " (includes informational STL groups)" : "");
    if (result.findings.empty()) { return; }
    const auto* comparison = findComparison(state, id);
    const auto selected = comparison->diagnosticFocus ? comparison->diagnosticFocus->index : size_t{0};
    constexpr size_t pageSize = 10;
    const size_t page = selected / pageSize, pages = (result.findings.size() + pageSize - 1) / pageSize;
    const auto choose = [&](size_t index) { selectComparisonDiagnostic(state, runtime.result, runtime.resultSignature, index, id); };
    if (ImGui::BeginTable("duplicate_findings", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Source"); ImGui::TableSetupColumn("Representative"); ImGui::TableSetupColumn("Extra records");
        ImGui::TableHeadersRow();
        for (size_t i = page*pageSize; i < std::min((page+1)*pageSize, result.findings.size()); ++i) {
            const auto& finding = result.findings[i];
            ImGui::PushID(static_cast<int>(i)); ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable(finding.source.c_str(), comparison->diagnosticFocus && selected == i, ImGuiSelectableFlags_SpanAllColumns)) { choose(i); }
            ImGui::TableNextColumn(); ImGui::Text("%zu", finding.members.front().id+1);
            ImGui::TableNextColumn(); ImGui::Text("%zu", finding.members.size()-1); ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(page == 0);
    if (ImGui::Button("Previous page")) { choose((page-1)*pageSize); }
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::Text("%zu / %zu", page+1, pages); ImGui::SameLine();
    ImGui::BeginDisabled(page+1 == pages);
    if (ImGui::Button("Next page")) { choose((page+1)*pageSize); }
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

void drawDiagnosticNavigation(UiState& state, const ComparisonRuntime& runtime, bool current,
    bool hasA, bool hasB, SceneObjectId id)
{
    ImGui::TextUnformatted("Diagnostics");
    ImGui::SameLine();
    drawInformationIcon("diagnostics_info", "Surface diagnostics",
        "Diagnostics describe combined surfaces; coincident edges across parts are matched. "
        "Open boundaries may be intentional. Self-intersections are not checked.\n\n"
        "Use each row's arrows to inspect edges or duplicate groups on the chosen target. "
        "Navigation wraps; right starts at the first finding and left at the last.\n\n"
        "Duplicate points use exactly equal imported coordinates within each source file. "
        "Duplicate triangles use the same three source point IDs regardless of winding. "
        "Counts are extra records; navigation visits groups. No tolerance is applied.\n\n"
        "Whole-file inspection includes unused points; selected parts include referenced points. "
        "OBJ IDs precede UV/normal splitting. Triangle IDs identify generated triangles, not original polygons. "
        "Plugin IDs describe the importer vertex table. STL corners are informational; its source-ID triangle check is unavailable. "
        "An asterisk indicates informational or partial results; hover the count for details.");
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    ImGui::TextUnformatted("Target");
    ImGui::SameLine();
    if (ImGui::RadioButton("A##diagnostics", settings.diagnosticSide == ComparisonSide::a)) {
        settings.diagnosticSide = ComparisonSide::a;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("B##diagnostics", settings.diagnosticSide == ComparisonSide::b)) {
        settings.diagnosticSide = ComparisonSide::b;
    }
    if (settings != initial) { setComparisonSettings(state, settings, id); }
    validateComparisonDiagnosticFocus(state, runtime.result, current ? runtime.resultSignature : 0, id);
    if (ImGui::BeginTable("Analysis diagnostics", 4 + static_cast<int>(hasA) + static_cast<int>(hasB),
            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Run", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Finding", ImGuiTableColumnFlags_WidthStretch);
        if (hasA) { ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3); }
        if (hasB) { ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 3); }
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##navigation", ImGuiTableColumnFlags_WidthFixed,
            2 * ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::TableHeadersRow();
        diagnosticRow(state, runtime, current, "Boundary edges", DiagnosticCategory::boundary, hasA, hasB, id);
        diagnosticRow(state, runtime, current, "Non-manifold edges", DiagnosticCategory::nonManifold, hasA, hasB, id);
        diagnosticRow(state, runtime, current, "Winding edges", DiagnosticCategory::winding, hasA, hasB, id);
        diagnosticRow(state, runtime, current, "Duplicate points", DiagnosticCategory::duplicatePoints, hasA, hasB, id);
        diagnosticRow(state, runtime, current, "Duplicate triangles", DiagnosticCategory::duplicateTriangles, hasA, hasB, id);
        ImGui::EndTable();
    }
    current = current && runtime.resultSignature == comparisonGeometrySignature(state, id);
    validateComparisonDiagnosticFocus(state, runtime.result, current ? runtime.resultSignature : 0, id);
    const auto* comparison = findComparison(state, id);
    if (!current) { ImGui::TextWrapped("Diagnostics unavailable until results are ready"); }
    else if (comparison->diagnosticFocus) {
        const auto& focus = *comparison->diagnosticFocus;
        const char* name = focus.category == DiagnosticCategory::boundary ? "Boundary"
            : focus.category == DiagnosticCategory::nonManifold ? "Non-manifold edges"
            : focus.category == DiagnosticCategory::duplicatePoints ? "Duplicate point group"
            : focus.category == DiagnosticCategory::duplicateTriangles ? "Duplicate triangle group" : "Winding edges";
        const auto count = comparisonDiagnosticEdges(runtime.result, focus.side, focus.category).size();
        ImGui::TextColored(ImVec4(1, 1, .1f, 1), "%s: %zu of %zu", name, focus.index + 1, count);
        ImGui::TextWrapped("Focused finding is highlighted in yellow");
    }
    drawDuplicateFindings(state, runtime, current, id);
    ImGui::BeginDisabled(!current);
    if (ImGui::Button("Full result")) { frameComparison(state, id); }
    ImGui::EndDisabled();
}

void submitEdges(bgfx::ViewId view, bgfx::VertexBufferHandle vertices, bgfx::ProgramHandle program,
                 bgfx::UniformHandle uniform, const std::array<float, 4> &color, const float* transform)
{
    if (!bgfx::isValid(vertices))
    {
        return;
    }
    bgfx::setTransform(transform);
    bgfx::setVertexBuffer(0, vertices);
    bgfx::setUniform(uniform, color.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_LINES | BGFX_STATE_DEPTH_TEST_ALWAYS |
                   BGFX_STATE_MSAA);
    bgfx::submit(view, program);
}
void submitWire(bgfx::ViewId view, const ComparisonGpuSurface &gpu, bgfx::ProgramHandle program,
                bgfx::UniformHandle uniform, const std::array<float, 4> &color, bool xray, const float* transform)
{
    bgfx::setTransform(transform);
    bgfx::setVertexBuffer(0, gpu.vertices);
    bgfx::setIndexBuffer(gpu.lines);
    bgfx::setUniform(uniform, color.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_LINES | BGFX_STATE_MSAA |
                   (xray ? BGFX_STATE_DEPTH_TEST_ALWAYS : BGFX_STATE_DEPTH_TEST_LEQUAL));
    bgfx::submit(view, program);
}
} // namespace

bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id, bool fullResults)
{
    const auto signature = comparisonGeometrySignature(state, id);
    const bool both = enabledComparisonPartCount(state, ComparisonSide::a, id) != 0
        && enabledComparisonPartCount(state, ComparisonSide::b, id) != 0;
    const auto required = requestedComparisonStages(comparisonSettings(state, id), both, fullResults);
    return signature != 0 && runtime.resultSignature == signature && runtime.cache.signature == signature
        && (runtime.cache.completed & required) == required && (fullResults || runtime.ready);
}

static void updateComparisonRuntime(ComparisonRuntime& runtime, UiState& state, SceneObjectId id, bool allowStart)
{
    const auto settings = comparisonSettings(state, id);
    const uint64_t wanted = comparisonGeometrySignature(state, id);
    const bool active = settings.enabled || runtime.fullResultsRequested;
    const bool both = enabledComparisonPartCount(state, ComparisonSide::a, id) != 0
        && enabledComparisonPartCount(state, ComparisonSide::b, id) != 0;
    const auto required = requestedComparisonStages(settings, both);
    const auto requested = requestedComparisonStages(settings, both, runtime.fullResultsRequested);
    if (resetComparisonCache(runtime.cache, wanted)) {
        runtime.stop.request_stop();
        destroySurface(runtime.originalGpu);
        destroySurface(runtime.repairedGpu);
        runtime.result = {};
        runtime.inputs.reset();
        runtime.uploadedStages = 0;
        runtime.resultSignature = runtime.attemptedSignature = 0;
        runtime.error.clear();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (runtime.worker.valid() && runtime.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto update = runtime.worker.get();
            if (applyComparisonStages(runtime.result, runtime.cache, std::move(update), runtime.workerSignature, runtime.workerStages)) {
                runtime.resultSignature = wanted;
                runtime.error.clear();
            }
        } catch (const std::exception& error) {
            if (!runtime.stop.stop_requested() && wanted == runtime.workerSignature) { runtime.error = error.what(); }
            else { runtime.attemptedSignature = 0; }
        }
    }
    setComparisonDuplicateEnabled(runtime.result, settings.duplicates);
    // CPU stages and their GPU uploads are retained independently. Retry an upload
    // without rerunning successful detectors, including after partial GPU failure.
    if (wanted && active && (runtime.error.empty() || runtime.attemptedSignature == 0)) {
        try {
            const auto pending = runtime.cache.completed & ~runtime.uploadedStages;
            if (pending) {
                uploadSurface(runtime.originalGpu, runtime.result.original, pending);
                uploadSurface(runtime.repairedGpu, runtime.result.repaired, pending);
                runtime.uploadedStages |= pending;
            }
            if ((runtime.cache.completed & comparisonQuality) && settings.mode == ComparisonMode::surfaceQuality &&
                (runtime.uploadedQualityMetric != settings.quality.metric ||
                 (!runtime.result.original.source.indices.empty() && !bgfx::isValid(runtime.originalGpu.quality)) ||
                 (!runtime.result.repaired.source.indices.empty() && !bgfx::isValid(runtime.repairedGpu.quality)))) {
                const auto& distribution = runtime.result.qualityDistributions.at(static_cast<size_t>(settings.quality.metric));
                uploadQuality(runtime.originalGpu, runtime.result.original, settings.quality.metric, distribution);
                uploadQuality(runtime.repairedGpu, runtime.result.repaired, settings.quality.metric, distribution);
                runtime.uploadedQualityMetric = settings.quality.metric;
            }
            runtime.error.clear();
        } catch (const std::exception& error) {
            destroySurface(runtime.originalGpu);
            destroySurface(runtime.repairedGpu);
            runtime.uploadedStages = 0;
            runtime.attemptedSignature = wanted;
            runtime.error = error.what();
        }
    }
    runtime.ready = wanted && settings.enabled && (runtime.cache.completed & required) == required
        && (runtime.uploadedStages & required) == required;
    const auto missing = requested & ~runtime.cache.completed;
    if (!wanted || !active || !missing || runtime.worker.valid() || !allowStart ||
        (runtime.attemptedSignature == wanted && runtime.attemptedStages == missing && !runtime.error.empty())) { return; }
    runtime.attemptedSignature = runtime.workerSignature = wanted;
    runtime.attemptedStages = runtime.workerStages = missing;
    runtime.error.clear();
    try {
        if (!runtime.inputs) {
            auto inputs = std::make_shared<std::array<Mesh, 2>>();
            if (enabledComparisonPartCount(state, ComparisonSide::a, id)) { (*inputs)[0] = comparisonWorldMesh(state, ComparisonSide::a, id); }
            if (enabledComparisonPartCount(state, ComparisonSide::b, id)) { (*inputs)[1] = comparisonWorldMesh(state, ComparisonSide::b, id); }
            runtime.inputs = std::move(inputs);
        }
        runtime.stop = std::stop_source{};
        runtime.worker = std::async(std::launch::async, [inputs = runtime.inputs, missing, stop = runtime.stop.get_token()] {
            return computeComparisonStages((*inputs)[0], (*inputs)[1], missing, stop);
        });
    } catch (const std::exception& error) { runtime.error = error.what(); }
}

static void destroyComparisonRuntime(ComparisonRuntime &runtime)
{
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
        const auto active = std::count_if(runtimes.objects.begin(), runtimes.objects.end(), [](const auto& item) {
            return item.second.worker.valid();
        });
        updateComparisonRuntime(runtime, state, comparison.objectId, active < 2);
        validateComparisonDiagnosticFocus(state, runtime.result,
            runtime.ready ? runtime.resultSignature : 0, comparison.objectId);
    }
}

void destroyComparisonRuntimes(ComparisonRuntimes& runtimes)
{
    for (auto& [id, runtime] : runtimes.objects) { (void)id; runtime.stop.request_stop(); }
    for (auto& [id, runtime] : runtimes.objects) { (void)id; destroyComparisonRuntime(runtime); }
    runtimes.objects.clear();
    if (bgfx::isValid(runtimes.program)) { bgfx::destroy(runtimes.program); runtimes.program = BGFX_INVALID_HANDLE; }
    if (bgfx::isValid(runtimes.parameters)) { bgfx::destroy(runtimes.parameters); runtimes.parameters = BGFX_INVALID_HANDLE; }
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
        if (comparison.settings.mode == ComparisonMode::surfaceQuality) {
            ready = ready && runtime.uploadedQualityMetric == comparison.settings.quality.metric &&
                (runtime.result.original.source.indices.empty() || bgfx::isValid(runtime.originalGpu.quality)) &&
                (runtime.result.repaired.source.indices.empty() || bgfx::isValid(runtime.repairedGpu.quality));
        }
    }
    return ready;
}

namespace {
void drawSurfaceQualitySizeLimits(UiState& state, SceneObjectId id, const MeshComparison* result, bool hasA, bool hasB)
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
    } else if (result && ImGui::BeginTable("quality_size_limits", 3,
                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Statistic", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("A"); ImGui::TableSetupColumn("B"); ImGui::TableHeadersRow();
        const std::array<QualitySizeLimits, 2> limits = {
            surfaceQualitySizeLimits(result->original.quality, quality),
            surfaceQualitySizeLimits(result->repaired.quality, quality)};
        const std::array<bool, 2> present = {hasA, hasB};
        const auto row = [&](const char* label, const auto& value) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            for (size_t side = 0; side < limits.size(); ++side) {
                ImGui::TableNextColumn();
                const auto text = present[side] ? value(limits[side]) : "-";
                ImGui::TextUnformatted(text.c_str());
            }
        };
        if (quality.minimumEnabled) { row("Below minimum", [](const auto& l) { return std::to_string(l.below); }); }
        if (quality.maximumEnabled) { row("Above maximum", [](const auto& l) { return std::to_string(l.above); }); }
        row("Outside: faces", [](const auto& l) { return l.validTriangles ? measurementNumber(l.trianglePercent) + "%" : "N/A"; });
        row("Outside: area", [](const auto& l) { return l.validTriangles ? measurementNumber(l.areaPercent) + "%" : "N/A"; });
        ImGui::EndTable();
    }
    ImGui::Separator();
}

void drawSurfaceQualityStatistics(const MeshComparison& result, const ComparisonSettings& settings, bool hasA, bool hasB)
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
    ImGui::TextWrapped("Shared A/B range. Magenta: degenerate. Gray: unavailable.");
    const std::array<const SurfaceMeshQuality*, 2> sides = {&result.original.quality, &result.repaired.quality};
    const std::array<bool, 2> present = {hasA, hasB};
    if (ImGui::BeginTable("quality_statistics", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Statistic", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("A"); ImGui::TableSetupColumn("B"); ImGui::TableHeadersRow();
        const auto row = [&](const char* label, const auto& value) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            for (size_t side = 0; side < sides.size(); ++side) {
                ImGui::TableNextColumn();
                const auto text = present[side] ? value(*sides[side]) : "-";
                ImGui::TextUnformatted(text.c_str());
            }
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
    ImGui::TextColored(ImVec4(.3f, .65f, 1, 1), "A"); ImGui::SameLine();
    ImGui::TextColored(ImVec4(1, .65f, .25f, 1), "B"); ImGui::SameLine();
    ImGui::TextUnformatted("Shared bins and vertical scale");
    const auto position = ImGui::GetCursorScreenPos();
    const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x), height = ImGui::GetFontSize() * 5;
    auto& draw = *ImGui::GetWindowDrawList();
    const auto percent = [&](size_t side, size_t bin) {
        return distribution.counts[side] ? 100.0 * static_cast<double>(distribution.bins[side][bin]) /
            static_cast<double>(distribution.counts[side]) : 0;
    };
    double peak = 0;
    for (size_t side = 0; side < 2; ++side) {
        for (size_t bin = 0; bin < surfaceQualityBinCount; ++bin) { peak = std::max(peak, percent(side, bin)); }
    }
    const float binWidth = width / static_cast<float>(surfaceQualityBinCount);
    draw.AddRectFilled(position, {position.x + width, position.y + height}, IM_COL32(30, 34, 40, 255));
    for (size_t bin = 0; bin < surfaceQualityBinCount; ++bin) {
        for (size_t side = 0; side < 2; ++side) {
            const float barHeight = peak > 0 ? height * static_cast<float>(percent(side, bin) / peak) : 0;
            const float x = position.x + static_cast<float>(bin) * binWidth + static_cast<float>(side) * binWidth * .5f;
            draw.AddRectFilled({x, position.y + height - barHeight}, {x + binWidth * .45f, position.y + height},
                side == 0 ? IM_COL32(77, 166, 255, 255) : IM_COL32(255, 166, 64, 255));
        }
    }
    ImGui::InvisibleButton("quality_histogram", {width, height});
    if (ImGui::IsItemHovered()) {
        const auto bin = std::min(static_cast<size_t>(std::max(0.0f, ImGui::GetIO().MousePos.x - position.x) / binWidth),
            surfaceQualityBinCount - 1);
        const double step = (distribution.maximum - distribution.minimum) / surfaceQualityBinCount;
        ImGui::BeginTooltip();
        ImGui::Text("[%.5g, %.5g%s", distribution.minimum + static_cast<double>(bin) * step,
            distribution.minimum + static_cast<double>(bin + 1) * step, bin + 1 == surfaceQualityBinCount ? "]" : ")");
        for (size_t side = 0; side < 2; ++side) {
            ImGui::Text("%s: %zu faces (%.2f%%)", side == 0 ? "A" : "B", distribution.bins[side][bin], percent(side, bin));
        }
        ImGui::EndTooltip();
    }
    ImGui::TextWrapped("%.5g to %.5g; vertical maximum %.3g%%", distribution.minimum, distribution.maximum, peak);
}

void drawComparisonContents(UiState &state, ComparisonRuntime &runtime, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Analysis");
    ImGui::SameLine();
    std::array<char, 512> name{};
    std::copy_n(comparison->name.data(), std::min(comparison->name.size(), name.size() - 1), name.data());
    ImGui::SetNextItemWidth(-informationIconSize() - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##comparison_name", name.data(), name.size())) { renameComparison(state, id, name.data()); }
    ImGui::SameLine();
    drawInformationIcon("comparison_info", "Analysis inputs",
        "Combined surfaces at scene positions. Hidden members are included. "
        "Other scene objects retain their own appearance.\n\n"
        "Use Analysis membership in the scene tree context menu, or drag sources onto group A or B. "
        "Right-click a group to clear it, or a source below to remove it.\n\n"
        "One input enables surface inspection. Add a second input for surface distance and overlay.");
    const bool resultReady = comparisonResultsReady(runtime, state, id);
    auto translation = comparison->translation;
    ImGui::TextUnformatted("Result position");
    ImGui::SameLine();
    drawInformationIcon("position_info", "Result position", "Display offset only.");
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::DragFloat3("##result_position", translation.data(), .1f)) { setComparisonTranslation(state, id, translation); }
    ImGui::Separator();
    membershipTree(state, ComparisonSide::a, id);
    membershipTree(state, ComparisonSide::b, id);
    ImGui::Separator();
    if (ImGui::Button("Swap inputs A / B")) { swapComparisonGroups(state, id); }
    ImGui::SameLine();
    drawInformationIcon("swap_info", "Swap inputs", "Swap exchanges assignments; measurement direction stays the same.");
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    const bool valid = canInspectComparison(state, id);
    const bool hasA = enabledComparisonPartCount(state, ComparisonSide::a, id) != 0;
    const bool hasB = enabledComparisonPartCount(state, ComparisonSide::b, id) != 0;
    const bool both = hasA && hasB;
    {
        const char *modes[] = {"Surface distance", "Group A", "Group B", "Overlay", "Surface mesh quality"};
        if (both) {
            int mode = static_cast<int>(settings.mode);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##comparison_mode", &mode, modes, 5)) {
                settings.mode = static_cast<ComparisonMode>(mode);
            }
        } else if (valid) {
            int mode = settings.mode == ComparisonMode::surfaceQuality ? 1 : 0;
            const char* singleModes[] = {hasA ? "Group A surface" : "Group B surface", "Surface mesh quality"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##comparison_mode", &mode, singleModes, 2)) {
                settings.mode = mode == 1 ? ComparisonMode::surfaceQuality : hasA ? ComparisonMode::original : ComparisonMode::repaired;
            }
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
            ImGui::SameLine();
            if (ImGui::RadioButton("B -> A", !settings.distanceOnOriginal)) { settings.distanceOnOriginal = false; }
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            ImGui::InputFloat("Tolerance", &settings.tolerance, 0, 0, "%.5g");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            ImGui::InputFloat("Color maximum", &settings.colorRange, 0, 0, "%.5g");
            ImGui::BeginDisabled(!resultReady || !(runtime.cache.completed & comparisonDistance));
            if (ImGui::Button("Fit color range")) {
                const auto& surface = settings.distanceOnOriginal ? runtime.result.original : runtime.result.repaired;
                settings.colorRange = std::max(static_cast<float>(surface.maximum), settings.tolerance);
            }
            ImGui::EndDisabled();

        }
        if (settings.mode == ComparisonMode::surfaceQuality) {
            int metric = static_cast<int>(settings.quality.metric);
            const char* metrics[] = {"Longest edge", "Equivalent size", "Shape quality", "Local size jump"};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##quality_metric", &metric, metrics, 4)) {
                settings.quality.metric = static_cast<SurfaceQualityMetric>(metric);
            }
            if (both) {
                ImGui::TextUnformatted("Heatmap surface");
                if (ImGui::RadioButton("A##quality", settings.quality.onOriginal)) { settings.quality.onOriginal = true; }
                ImGui::SameLine();
                if (ImGui::RadioButton("B##quality", !settings.quality.onOriginal)) { settings.quality.onOriginal = false; }
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
    if (settings.mode == ComparisonMode::surfaceQuality && (!comparisonResultsReady(runtime, state, id) || !valid || !settings.enabled)) {
        drawSurfaceQualitySizeLimits(state, id, nullptr, hasA, hasB);
    }
    const bool diagnosticsCurrent = valid && comparisonSettings(state, id).enabled && comparisonResultsReady(runtime, state, id);
    if (!diagnosticsCurrent) { drawDiagnosticNavigation(state, runtime, false, hasA, hasB, id); }
    if (!valid || !comparisonSettings(state, id).enabled)
    {
        return;
    }
    const bool current = comparisonResultsReady(runtime, state, id);
    if (!current)
    {
        if (!runtime.error.empty() && runtime.attemptedSignature == comparisonGeometrySignature(state, id))
        {
            ImGui::TextWrapped("%s", runtime.error.c_str());
            if (ImGui::Button("Retry analysis"))
            {
                runtime.attemptedSignature = 0;
            }
        }
        else
        {
            ImGui::TextUnformatted("Computing analysis...");
        }
        return;
    }
    if (settings.mode == ComparisonMode::surfaceQuality) {
        drawSurfaceQualityStatistics(runtime.result, comparisonSettings(state, id), hasA, hasB);
        drawSurfaceQualitySizeLimits(state, id, &runtime.result, hasA, hasB);
    }
    // Diagnostic focus temporarily changes the drawn surface, not the measured direction.
    const bool useOriginal = !hasB || (hasA && originalActive(comparisonSettings(state, id)));
    const auto &surface = useOriginal ? runtime.result.original : runtime.result.repaired;
    if (both && comparisonSettings(state, id).mode == ComparisonMode::distance)
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
    const auto &a = runtime.result.original.diagnostics;
    const auto &b = runtime.result.repaired.diagnostics;
    drawDiagnosticNavigation(state, runtime, true, hasA, hasB, id);
    if (both) {
        ImGui::TextWrapped("Degenerate triangles: %zu -> %zu", a.degenerateTriangles, b.degenerateTriangles);
        ImGui::TextWrapped("Geometric duplicate triangles: %zu -> %zu", a.duplicateTriangles, b.duplicateTriangles);
    } else {
        ImGui::TextWrapped("Degenerate triangles: %zu", surface.diagnostics.degenerateTriangles);
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
    if (ImGui::BeginChild("comparison_properties")) {
        drawComparisonContents(state, runtimes.objects[id], id);
    }
    ImGui::EndChild();
    ImGui::PopID();
}

static void submitComparisonScene(bgfx::ViewId view, const UiComparison& comparison, const ComparisonRuntime& runtime,
                           const ComparisonRuntimes& runtimes,
                           bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform, const ComparisonSettings& settings)
{
    if (!comparison.settings.enabled || !runtime.ready) { return; }
    const bool useOriginal = originalActive(settings);
    const auto &gpu = useOriginal ? runtime.originalGpu : runtime.repairedGpu;
    const bool quality = settings.mode == ComparisonMode::surfaceQuality;
    const bool heatmap = settings.mode == ComparisonMode::distance || quality;
    if (quality && (!bgfx::isValid(gpu.quality) || runtime.uploadedQualityMetric != settings.quality.metric)) { return; }
    float identity[16];
    bx::mtxTranslate(identity, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
    const std::array<float, 4> parameters = {settings.tolerance, settings.colorRange, quality ? 2.0f : heatmap ? 1.0f : 0.0f,
        quality && settings.quality.metric == SurfaceQualityMetric::shape ? 1.0f : 0.0f};
    const std::array<float, 4> gray = {.58f, .63f, .69f, 1};
    bgfx::setTransform(identity);
    bgfx::setUniform(runtimes.parameters, parameters.data());
    bgfx::setUniform(colorUniform, gray.data());
    bgfx::setVertexBuffer(0, quality ? gpu.quality : heatmap ? gpu.samples : gpu.vertices);
    if (!heatmap)
    {
        bgfx::setIndexBuffer(gpu.triangles);
    }
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LEQUAL |
                   BGFX_STATE_MSAA);
    bgfx::submit(view, runtimes.program);
    if (settings.showEdges)
    {
        submitWire(view, gpu, colorProgram, colorUniform, {.22f, .25f, .30f, 1}, false, identity);
    }
    if (settings.mode == ComparisonMode::overlay)
    {
        submitWire(view, runtime.originalGpu, colorProgram, colorUniform, {.3f, .75f, 1, 1}, true, identity);
    }
    if (settings.duplicates.triangles && settings.duplicates.showTriangles) {
        if (bgfx::isValid(gpu.duplicateTriangleFill)) {
            const std::array<float, 4> orange = {1, .45f, .08f, .4f};
            bgfx::setTransform(identity); bgfx::setVertexBuffer(0, gpu.duplicateTriangleFill);
            bgfx::setUniform(colorUniform, orange.data());
            bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
            bgfx::submit(view, colorProgram);
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
        submitEdges(view, gpu.winding, colorProgram, colorUniform, {1, .2f, .2f, 1}, identity);
    }
}

void appendVisibleComparisonPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    const ComparisonRuntimes& runtimes)
{
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it != runtimes.objects.end() && comparisonResultsReady(it->second, state, comparison.objectId)) {
            appendComparisonPickParts(parts, comparison, effectiveComparisonSettings(state, comparison.objectId),
                it->second.result, sceneObjectSelected(state, comparison.objectId));
        }
    }
}

void submitComparisonScenes(bgfx::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform)
{
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it != runtimes.objects.end() && comparisonResultsReady(it->second, state, comparison.objectId)) {
            auto settings = effectiveComparisonSettings(state, comparison.objectId);
            if (it->second.resultSignature != comparisonGeometrySignature(state, comparison.objectId)) {
                settings.duplicates.showPoints = false; settings.duplicates.showTriangles = false;
            }
            submitComparisonScene(view, comparison, it->second, runtimes, colorProgram, colorUniform, settings);
        }
    }
    // Submit focus last so surfaces and other diagnostic edges cannot obscure it.
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it == runtimes.objects.end() || !it->second.ready) { continue; }
        const auto* edge = focusedComparisonDiagnostic(state, it->second.result,
            it->second.resultSignature, comparison.objectId);
        if (!edge) { continue; }
        std::vector<std::array<float, 3>> points, faceFill;
        const auto& focus = *comparison.diagnosticFocus;
        const bool duplicatePoints = focus.category == DiagnosticCategory::duplicatePoints;
        const bool duplicateTriangles = focus.category == DiagnosticCategory::duplicateTriangles;
        const float radius = state.camera.distance * .015f;
        if (duplicatePoints || duplicateTriangles) {
            const auto& finding = comparisonDuplicates(it->second.result, focus.side, focus.category).findings.at(focus.index);
            if (duplicatePoints) {
                for (const auto& point : finding.geometry) { appendCross(points, point, radius); }
            } else {
                faceFill = finding.geometry;
                for (size_t i = 0; i < finding.geometry.size(); i += 3) {
                    for (size_t k = 0; k < 3; ++k) { points.push_back(finding.geometry[i+k]); points.push_back(finding.geometry[i+(k+1)%3]); }
                }
            }
        } else {
            points.push_back(edge->a); points.push_back(edge->b);
            for (const auto& endpoint : {edge->a, edge->b}) { appendCross(points, endpoint, radius); }
        }
        // Portable thick lines: bgfx line primitives are only one pixel wide on
        // some backends and would merge into the yellow object-selection outline.
        std::vector<std::array<float, 3>> triangles;
        triangles = std::move(faceFill);
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
        if (bgfx::getAvailTransientVertexBuffer(vertexCount, layout) < vertexCount) { continue; }
        bgfx::TransientVertexBuffer buffer;
        bgfx::allocTransientVertexBuffer(&buffer, vertexCount, layout);
        std::memcpy(buffer.data, triangles.data(), triangles.size() * sizeof(triangles[0]));
        float transform[16];
        bx::mtxTranslate(transform, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
        const std::array<float, 4> yellow = {1, 1, .1f, 1};
        bgfx::setTransform(transform);
        bgfx::setUniform(colorUniform, yellow.data());
        bgfx::setVertexBuffer(0, &buffer);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
            BGFX_STATE_DEPTH_TEST_ALWAYS | BGFX_STATE_MSAA);
        bgfx::submit(view, colorProgram);
    }
}

void drawComparisonObjects(UiState& state, ComparisonNameEdit& edit)
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
    const bool sceneFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    const bool canStartRename = sceneFocused && !ImGui::GetIO().WantTextInput
        && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (canStartRename && edit.objectId == invalidSceneObjectId && state.selectedSceneObjects.size() == 1
        && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        beginRename(state.selectedSceneObjects.front());
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Analyses");
    if (state.comparisons.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Right-click an object and choose Create analysis to add an analysis.");
        ImGui::PopStyleColor();
    }
    for (const auto& comparison : state.comparisons) {
        const auto id = comparison.objectId;
        const auto label = std::to_string(id);
        ImGui::PushID(label.c_str());
        const float removeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - renderModeButtonSize();
        auto settings = comparison.settings;
        if (drawVisibilityButton("visible", settings.enabled, "analysis")) {
            settings.enabled = !settings.enabled;
            setComparisonSettings(state, settings, id);
        }
        ImGui::SameLine();
        // Keep long names and their hit targets out of the remove button's column.
        const float nameWidth = std::max(1.0f, removeX - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
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
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) {
                beginRename(id);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectSceneObject(state, id, false, true); }
            if (ImGui::BeginPopupContextItem("comparison_object")) {
                if (ImGui::MenuItem("Properties")) { selectSceneObject(state, id); }
                if (ImGui::MenuItem("Rename", "F2")) { beginRename(id); }
                if (ImGui::MenuItem("Frame result", nullptr, false, canInspectComparison(state, id))) { frameComparison(state, id); }
                if (ImGui::MenuItem("Duplicate")) { duplicateComparison(state, id); changed = true; }
                if (ImGui::MenuItem("Delete analysis")) { removeComparison(state, id); changed = true; }
                ImGui::EndPopup();
            }
        }
        if (!changed) {
            ImGui::SameLine(removeX, 0.0f);
            if (drawRemoveButton("remove", "Remove analysis from scene")) {
                removeComparison(state, id);
                changed = true;
            }
        }
        if (!changed && !canInspectComparison(state, id)) {
            for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
                const auto summary = comparisonInputSummary(state, side, id);
                if (summary.issue.empty()) { continue; }
                const auto& members = side == ComparisonSide::a ? comparison.a : comparison.b;
                ImGui::TextDisabled("    Input %s: %s", side == ComparisonSide::a ? "A" : "B",
                    members.empty() ? "empty" : summary.enabledPartCount == 0 ? "off / unavailable" : "missing / invalid source");
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", summary.issue.c_str());
                }
            }
        }
        ImGui::PopID();
        if (changed) { break; }
    }
}
} // namespace woby
