#include "comparison_view.h"
#include "comparison_scene.h"
#include "comparison_legend.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "utf8_path.h"

#include <imgui.h>
#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace woby
{
namespace
{
void destroySurface(ComparisonGpuSurface &gpu)
{
    for (const auto handle : {gpu.vertices, gpu.samples, gpu.boundaries, gpu.nonManifold, gpu.winding})
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
        throw std::runtime_error("Cannot allocate comparison edge buffer.");
    }
    return handle;
}
void uploadSurface(ComparisonGpuSurface &gpu, const SurfaceComparison &surface)
{
    if (surface.source.indices.empty()) { return; }
    // Validate every upload before allocating CPU staging memory or GPU handles.
    const auto vertexBytes = comparisonBufferBytes(surface.source.vertices.size(), sizeof(Vertex));
    const auto indexBytes = comparisonBufferBytes(surface.source.indices.size(), sizeof(uint32_t));
    const auto lineBytes = comparisonBufferBytes(surface.source.indices.size(), 2 * sizeof(uint32_t));
    const auto sampleBytes = comparisonBufferBytes(surface.sampled.vertices.size(), sizeof(Vertex));
    for (const auto* edges : {&surface.diagnostics.boundaryEdges, &surface.diagnostics.nonManifoldEdges,
                             &surface.diagnostics.inconsistentWindingEdges})
    {
        (void)comparisonBufferBytes(edges->size(), 2 * sizeof(std::array<float, 3>));
    }
    auto vertices = surface.source.vertices;
    generateSmoothNormals(vertices, surface.source.indices);
    gpu.vertices = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), vertexBytes), meshVertexLayout());
    const auto &indices = surface.source.indices;
    gpu.triangles = bgfx::createIndexBuffer(
        bgfx::copy(indices.data(), indexBytes), BGFX_BUFFER_INDEX32);
    std::vector<uint32_t> lines;
    lines.reserve(indices.size() * 2);
    for (size_t i = 0; i < indices.size(); i += 3)
    {
        for (size_t k = 0; k < 3; ++k)
        {
            lines.push_back(indices[i + k]);
            lines.push_back(indices[i + (k + 1) % 3]);
        }
    }
    gpu.lines = bgfx::createIndexBuffer(
        bgfx::copy(lines.data(), lineBytes), BGFX_BUFFER_INDEX32);
    const auto &samples = surface.sampled.vertices;
    if (!samples.empty()) {
        gpu.samples = bgfx::createVertexBuffer(
            bgfx::copy(samples.data(), sampleBytes), meshVertexLayout());
    }
    if (!bgfx::isValid(gpu.vertices) || !bgfx::isValid(gpu.triangles) || !bgfx::isValid(gpu.lines) ||
        (!samples.empty() && !bgfx::isValid(gpu.samples)))
    {
        throw std::runtime_error("Cannot allocate comparison surface buffers.");
    }
    gpu.boundaries = uploadEdges(surface.diagnostics.boundaryEdges);
    gpu.nonManifold = uploadEdges(surface.diagnostics.nonManifoldEdges);
    gpu.winding = uploadEdges(surface.diagnostics.inconsistentWindingEdges);
}
bool originalActive(const ComparisonSettings &settings)
{
    return settings.mode == ComparisonMode::original ||
           (settings.mode == ComparisonMode::distance && settings.distanceOnOriginal);
}
void drawComparisonTreeNode(UiState& state, ComparisonSide side, const ComparisonTreeNode& node, SceneObjectId id)
{
    const auto nodeId = std::to_string(node.objectId);
    ImGui::PushID(nodeId.c_str());
    const bool leaf = node.children.empty();
    const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth
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
    const auto summary = comparisonInputSummary(state, side, id);
    const auto roots = comparisonTree(state, side, id);
    size_t triangles = 0;
    for (const auto& root : roots) { triangles += root.triangleCount; }
    const bool open = ImGui::TreeNodeEx("root", ImGuiTreeNodeFlags_DefaultOpen,
        "%s (%zu %s) %zu triangles", label, summary.partCount,
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
void frameEdges(UiState &state, const std::vector<DiagnosticEdge> &edges, SceneObjectId id)
{
    if (edges.empty())
    {
        return;
    }
    const auto &edge = edges.front();
    std::vector<Vertex> points(2);
    points[0].position = edge.a;
    points[1].position = edge.b;
    auto bounds = calculateBounds(points);
    bounds.radius = std::max(bounds.radius * 3, state.sceneBounds.radius * .06f);
    if (const auto* comparison = findComparison(state, id)) {
        for (size_t k = 0; k < 3; ++k) {
            bounds.min[k] += comparison->translation[k];
            bounds.max[k] += comparison->translation[k];
            bounds.center[k] += comparison->translation[k];
        }
    }
    frameComparisonBounds(state, bounds);
}
void diagnosticRow(UiState &state, const char *name, const std::vector<DiagnosticEdge> &original,
                   const std::vector<DiagnosticEdge> &repaired, bool useOriginal, bool hasA, bool hasB, SceneObjectId id)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    if (hasA) { ImGui::TableNextColumn(); ImGui::Text("%zu", original.size()); }
    if (hasB) { ImGui::TableNextColumn(); ImGui::Text("%zu", repaired.size()); }
    ImGui::TableNextColumn();
    const auto &active = useOriginal ? original : repaired;
    ImGui::PushID(name);
    ImGui::BeginDisabled(active.empty());
    if (ImGui::SmallButton("First"))
    {
        auto settings = comparisonSettings(state, id);
        if (std::string(name) == "Boundary")
        {
            settings.showBoundaries = true;
        }
        else
        {
            settings.showNonManifold = true;
        }
        setComparisonSettings(state, settings, id);
        frameEdges(state, active, id);
    }
    ImGui::EndDisabled();
    ImGui::PopID();
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

static void updateComparisonRuntime(ComparisonRuntime& runtime, const UiState& state, SceneObjectId id, bool allowStart)
{
    const uint64_t wanted = comparisonSettings(state, id).enabled ? comparisonGeometrySignature(state, id) : 0;
    if (runtime.worker.valid() && wanted != runtime.workerSignature)
    {
        runtime.stop.request_stop();
    }
    runtime.ready = wanted != 0 && runtime.resultSignature == wanted;
    if (runtime.worker.valid() && runtime.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            auto result = runtime.worker.get();
            if (wanted != 0 && wanted == runtime.workerSignature)
            {
                destroySurface(runtime.originalGpu);
                destroySurface(runtime.repairedGpu);
                runtime.resultSignature = 0;
                runtime.result = std::move(result);
                uploadSurface(runtime.originalGpu, runtime.result.original);
                uploadSurface(runtime.repairedGpu, runtime.result.repaired);
                runtime.resultSignature = wanted;
                runtime.ready = true;
                runtime.error.clear();
            }
            else
            {
                runtime.attemptedSignature = 0;
            }
        }
        catch (const std::exception &error)
        {
            if (runtime.stop.stop_requested())
            {
                runtime.attemptedSignature = 0;
            }
            else if (wanted == runtime.workerSignature)
            {
                destroySurface(runtime.originalGpu);
                destroySurface(runtime.repairedGpu);
                runtime.resultSignature = 0;
                runtime.ready = false;
                runtime.error = error.what();
            }
            else
            {
                runtime.attemptedSignature = 0;
            }
        }
    }
    if (wanted == 0 || runtime.ready || runtime.worker.valid() || wanted == runtime.attemptedSignature || !allowStart)
    {
        return;
    }
    runtime.attemptedSignature = wanted;
    runtime.workerSignature = wanted;
    runtime.error.clear();
    try
    {
        auto original = comparisonPartCount(state, ComparisonSide::a, id) ? comparisonWorldMesh(state, ComparisonSide::a, id) : Mesh{};
        auto repaired = comparisonPartCount(state, ComparisonSide::b, id) ? comparisonWorldMesh(state, ComparisonSide::b, id) : Mesh{};
        runtime.stop = std::stop_source{};
        const auto stop = runtime.stop.get_token();
        runtime.worker = std::async(std::launch::async, [original = std::move(original), repaired = std::move(repaired),
                                                         stop] { return compareMeshes(original, repaired, stop); });
    }
    catch (const std::exception &error)
    {
        runtime.error = error.what();
    }
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

void updateComparisonRuntimes(ComparisonRuntimes& runtimes, const UiState& state)
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
        if (!runtime.error.empty() && runtime.attemptedSignature == signature) {
            throw std::runtime_error(comparison.name + ": " + runtime.error);
        }
        ready = ready && runtime.ready && runtime.resultSignature == signature;
    }
    return ready;
}

namespace {
void drawComparisonContents(UiState &state, ComparisonRuntime &runtime, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Comparison");
    ImGui::SameLine();
    std::array<char, 512> name{};
    std::copy_n(comparison->name.data(), std::min(comparison->name.size(), name.size() - 1), name.data());
    ImGui::SetNextItemWidth(-informationIconSize() - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::InputText("##comparison_name", name.data(), name.size())) { renameComparison(state, id, name.data()); }
    ImGui::SameLine();
    drawInformationIcon("comparison_info", "Comparison inputs",
        "Combined surfaces at scene positions, in model units. Hidden members are included. "
        "Other scene objects retain their own appearance.\n\n"
        "Use Comparison membership in the scene tree context menu, or drag sources onto group A or B. "
        "Right-click a group to clear it, or a source below to remove it.\n\n"
        "One input enables surface inspection. Add a second input for surface distance and overlay.");
    const bool resultReady = runtime.ready && runtime.resultSignature == comparisonGeometrySignature(state, id);
    if (resultReady) { ImGui::TextUnformatted("Result ready"); }
    ImGui::BeginDisabled(!resultReady);
    if (ImGui::Button("Frame result", ImVec2(-1.0f, 0.0f))) { frameComparison(state, id); }
    ImGui::EndDisabled();
    auto translation = comparison->translation;
    ImGui::TextUnformatted("Result position");
    ImGui::SameLine();
    drawInformationIcon("position_info", "Result position", "Display offset only, in model units.");
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
    const bool hasA = !comparison->a.empty(), hasB = !comparison->b.empty();
    const bool both = hasA && hasB;
    {
        const char *modes[] = {"Surface distance", "Group A", "Group B", "Overlay"};
        if (both) {
            int mode = static_cast<int>(settings.mode);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##comparison_mode", &mode, modes, 4)) {
                settings.mode = static_cast<ComparisonMode>(mode);
            }
        } else if (valid) {
            ImGui::TextUnformatted(hasA ? "Group A surface" : "Group B surface");
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
            ImGui::BeginDisabled(!resultReady);
            if (ImGui::Button("Fit color range")) {
                const auto& surface = settings.distanceOnOriginal ? runtime.result.original : runtime.result.repaired;
                settings.colorRange = std::max(static_cast<float>(surface.maximum), settings.tolerance);
            }
            ImGui::EndDisabled();
            std::array<char, 256> units{};
            std::copy_n(settings.unitLabel.data(), std::min(settings.unitLabel.size(), units.size() - 1), units.data());
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::InputTextWithHint("Unit label", "model units", units.data(), units.size())) {
                settings.unitLabel = units.data();
            }
            ImGui::SameLine();
            drawInformationIcon("units_info", "Unit label", "Label only; does not convert coordinates. Blank = model units.");
        }
        if (both && settings.mode == ComparisonMode::overlay)
        {
            drawInformationIcon("overlay_info", "Overlay colors",
                "Blue: group A wireframe (X-ray).\n\nSolid gray: group B surface.");
        }
        ImGui::Checkbox("Triangle edges", &settings.showEdges);
        ImGui::Checkbox("Boundary edges (yellow)", &settings.showBoundaries);
        ImGui::Checkbox("Non-manifold / winding edges", &settings.showNonManifold);
    }
    // Merely opening the panel must not change scene settings.
    if (settings != initial)
    {
        setComparisonSettings(state, settings, id);
    }
    if (both && comparisonSettings(state, id).mode == ComparisonMode::distance) {
        const auto currentSettings = comparisonSettings(state, id);
        ImGui::Text("Distance (%s)", comparisonUnits(currentSettings).c_str());
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
    if (!valid || !comparisonSettings(state, id).enabled)
    {
        return;
    }
    const bool current = runtime.ready && runtime.resultSignature == comparisonGeometrySignature(state, id);
    if (!current)
    {
        if (!runtime.error.empty() && runtime.attemptedSignature == comparisonGeometrySignature(state, id))
        {
            ImGui::TextWrapped("%s", runtime.error.c_str());
            if (ImGui::Button("Retry comparison"))
            {
                runtime.attemptedSignature = 0;
            }
        }
        else
        {
            ImGui::TextUnformatted("Computing comparison...");
        }
        return;
    }
    const bool useOriginal = originalActive(effectiveComparisonSettings(state, id));
    const auto &surface = useOriginal ? runtime.result.original : runtime.result.repaired;
    if (both && comparisonSettings(state, id).mode == ComparisonMode::distance)
    {
        if (surface.maximum > comparisonSettings(state, id).colorRange) {
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1, .65f, .25f, 1), "SATURATED: sample max exceeds color maximum");
            ImGui::PopTextWrapPos();
        }
        const auto units = comparisonUnits(comparisonSettings(state, id));
        ImGui::TextWrapped("Sample max: %.5g %s", surface.maximum, units.c_str());
        ImGui::TextWrapped("Area-weighted mean: %.5g %s", surface.mean, units.c_str());
        ImGui::TextWrapped("Area-weighted P95: %.5g %s", surface.percentile95, units.c_str());
        ImGui::TextWrapped("Area above tolerance: %.2f%%", surfacePercentAboveTolerance(surface, comparisonSettings(state, id).tolerance));
    }
    const auto &a = runtime.result.original.diagnostics;
    const auto &b = runtime.result.repaired.diagnostics;
    ImGui::TextUnformatted("Diagnostics");
    ImGui::SameLine();
    drawInformationIcon("diagnostics_info", "Surface diagnostics",
        "Diagnostics describe combined surfaces; coincident edges across parts are matched. "
        "Open boundaries may be intentional. Self-intersections are not checked.");
    if (ImGui::BeginTable("Comparison diagnostics", both ? 4 : 3, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Edges");
        if (hasA) { ImGui::TableSetupColumn("A"); }
        if (hasB) { ImGui::TableSetupColumn("B"); }
        ImGui::TableSetupColumn("Focus");
        ImGui::TableHeadersRow();
        diagnosticRow(state, "Boundary", a.boundaryEdges, b.boundaryEdges, useOriginal, hasA, hasB, id);
        diagnosticRow(state, "Non-manifold", a.nonManifoldEdges, b.nonManifoldEdges, useOriginal, hasA, hasB, id);
        diagnosticRow(state, "Winding", a.inconsistentWindingEdges, b.inconsistentWindingEdges, useOriginal, hasA, hasB, id);
        ImGui::EndTable();
    }
    if (both) {
        ImGui::TextWrapped("Degenerate triangles: %zu -> %zu", a.degenerateTriangles, b.degenerateTriangles);
        ImGui::TextWrapped("Duplicate triangles: %zu -> %zu", a.duplicateTriangles, b.duplicateTriangles);
    } else {
        ImGui::TextWrapped("Degenerate triangles: %zu", surface.diagnostics.degenerateTriangles);
        ImGui::TextWrapped("Duplicate triangles: %zu", surface.diagnostics.duplicateTriangles);
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
    const bool heatmap = settings.mode == ComparisonMode::distance;
    float identity[16];
    bx::mtxTranslate(identity, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
    const std::array<float, 4> parameters = {settings.tolerance, settings.colorRange, heatmap ? 1.0f : 0.0f, 0};
    const std::array<float, 4> gray = {.58f, .63f, .69f, 1};
    bgfx::setTransform(identity);
    bgfx::setUniform(runtimes.parameters, parameters.data());
    bgfx::setUniform(colorUniform, gray.data());
    bgfx::setVertexBuffer(0, heatmap ? gpu.samples : gpu.vertices);
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
    if (settings.showBoundaries)
    {
        submitEdges(view, gpu.boundaries, colorProgram, colorUniform, {1, .85f, .15f, 1}, identity);
    }
    if (settings.showNonManifold)
    {
        submitEdges(view, gpu.nonManifold, colorProgram, colorUniform, {1, .15f, .55f, 1}, identity);
        submitEdges(view, gpu.winding, colorProgram, colorUniform, {1, .2f, .2f, 1}, identity);
    }
}

void submitComparisonScenes(bgfx::ViewId view, const UiState& state, const ComparisonRuntimes& runtimes,
    bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform)
{
    for (const auto& comparison : state.comparisons) {
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it != runtimes.objects.end()) {
            submitComparisonScene(view, comparison, it->second, runtimes, colorProgram, colorUniform,
                effectiveComparisonSettings(state, comparison.objectId));
        }
    }
}

void drawComparisonObjects(UiState& state)
{
    ImGui::Separator();
    ImGui::TextUnformatted("Comparisons");
    if (state.comparisons.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("Right-click an object and choose Create comparison to add a comparison.");
        ImGui::PopStyleColor();
    }
    for (const auto& comparison : state.comparisons) {
        const auto id = comparison.objectId;
        const auto label = std::to_string(id);
        ImGui::PushID(label.c_str());
        const float removeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - renderModeButtonSize();
        auto settings = comparison.settings;
        if (drawVisibilityButton("visible", settings.enabled, "comparison")) {
            settings.enabled = !settings.enabled;
            setComparisonSettings(state, settings, id);
        }
        ImGui::SameLine();
        // Keep long names and their hit targets out of the remove button's column.
        const float nameWidth = std::max(1.0f, removeX - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
        if (drawSceneItemButton((comparison.name + "###comparison_row").c_str(), nameWidth,
                sceneObjectSelected(state, id))) {
            selectSceneObject(state, id, ImGui::GetIO().KeyCtrl);
        }
        if (sceneObjectSelected(state, id)) {
            drawSceneItemOutline();
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectSceneObject(state, id, false, true); }
        bool changed = false;
        if (ImGui::BeginPopupContextItem("comparison_object")) {
            if (ImGui::MenuItem("Properties")) { selectSceneObject(state, id); }
            if (ImGui::MenuItem("Frame result", nullptr, false, canInspectComparison(state, id))) { frameComparison(state, id); }
            if (ImGui::MenuItem("Duplicate")) { duplicateComparison(state, id); changed = true; }
            if (ImGui::MenuItem("Delete comparison")) { removeComparison(state, id); changed = true; }
            ImGui::EndPopup();
        }
        if (!changed) {
            ImGui::SameLine(removeX, 0.0f);
            if (drawRemoveButton("remove", "Remove comparison from scene")) {
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
                    members.empty() ? "empty" : "missing / invalid source");
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
