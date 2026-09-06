#include "comparison_view.h"
#include "comparison_scene.h"
#include "ui_operations.h"
#include "utf8_path.h"

#include <imgui.h>
#include <bx/math.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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
    gpu.samples = bgfx::createVertexBuffer(
        bgfx::copy(samples.data(), sampleBytes), meshVertexLayout());
    if (!bgfx::isValid(gpu.vertices) || !bgfx::isValid(gpu.triangles) || !bgfx::isValid(gpu.lines) ||
        !bgfx::isValid(gpu.samples))
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
void drawComparisonTreeNode(UiState& state, ComparisonSide side, const ComparisonTreeNode& node)
{
    const auto id = std::to_string(node.objectId);
    ImGui::PushID(id.c_str());
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
        ImGui::TextUnformatted("Right-click to remove from this comparison group.");
        ImGui::EndTooltip();
    }
    if (ImGui::BeginPopupContextItem("membership")) {
        const char* label = side == ComparisonSide::a ? "Remove from group A" : "Remove from group B";
        if (ImGui::MenuItem(label)) { setComparisonObjects(state, {node.objectId}, side, false); }
        ImGui::EndPopup();
    }
    if (open && !leaf) {
        for (const auto& child : node.children) { drawComparisonTreeNode(state, side, child); }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void membershipTree(UiState& state, ComparisonSide side)
{
    const char* label = side == ComparisonSide::a ? "Group A" : "Group B";
    const auto roots = comparisonTree(state, side);
    size_t count = 0, triangles = 0;
    for (const auto& root : roots) { count += root.partCount; triangles += root.triangleCount; }
    ImGui::PushID(label);
    const bool open = ImGui::TreeNodeEx("root", ImGuiTreeNodeFlags_DefaultOpen,
        "%s (%zu %s)", label, count, count == 1 ? "part" : "parts");
    ImGui::SameLine();
    ImGui::BeginDisabled(count == 0);
    const bool clear = ImGui::SmallButton("Clear");
    if (clear) { clearComparisonGroup(state, side); }
    ImGui::EndDisabled();
    if (open) {
        if (roots.empty() || clear) { ImGui::TextDisabled("No objects added"); }
        else {
            ImGui::TextDisabled("%zu triangles", triangles);
            for (const auto& root : roots) { drawComparisonTreeNode(state, side, root); }
        }
        if (const auto* comparison = findComparison(state)) {
            const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
            bool missing = false;
            for (const auto& part : members) {
                if (comparisonObjectParts(state, {part.objectId}).empty()) {
                    ImGui::TextWrapped("Missing / invalid: %s", part.name.c_str());
                    missing = true;
                }
            }
            if (missing && ImGui::SmallButton("Remove missing references")) { removeMissingComparisonParts(state, side); }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}
void frameEdges(UiState &state, const std::vector<DiagnosticEdge> &edges)
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
    if (const auto* comparison = findComparison(state)) {
        for (size_t k = 0; k < 3; ++k) {
            bounds.min[k] += comparison->translation[k];
            bounds.max[k] += comparison->translation[k];
            bounds.center[k] += comparison->translation[k];
        }
    }
    frameComparisonBounds(state, bounds);
}
void diagnosticRow(UiState &state, const char *name, const std::vector<DiagnosticEdge> &original,
                   const std::vector<DiagnosticEdge> &repaired, bool useOriginal)
{
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    ImGui::TableNextColumn();
    ImGui::Text("%zu", original.size());
    ImGui::TableNextColumn();
    ImGui::Text("%zu", repaired.size());
    ImGui::TableNextColumn();
    const auto &active = useOriginal ? original : repaired;
    ImGui::PushID(name);
    ImGui::BeginDisabled(active.empty());
    if (ImGui::SmallButton("First"))
    {
        auto settings = comparisonSettings(state);
        if (std::string(name) == "Boundary")
        {
            settings.showBoundaries = true;
        }
        else
        {
            settings.showNonManifold = true;
        }
        setComparisonSettings(state, settings);
        frameEdges(state, active);
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
        auto original = comparisonWorldMesh(state, ComparisonSide::a, id);
        auto repaired = comparisonWorldMesh(state, ComparisonSide::b, id);
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
        if (!canCompareGroups(state, comparison.objectId)) {
            throw std::runtime_error(comparison.name + ": comparison inputs are incomplete.");
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
void drawComparisonContents(UiState &state, ComparisonRuntime &runtime)
{
    const auto* comparison = findComparison(state);
    if (!comparison) { return; }
    const auto id = comparison->objectId;
    std::array<char, 512> name{};
    std::copy_n(comparison->name.data(), std::min(comparison->name.size(), name.size() - 1), name.data());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##comparison_name", name.data(), name.size())) { renameComparison(state, id, name.data()); }
    const bool resultReady = runtime.ready && runtime.resultSignature == comparisonGeometrySignature(state);
    if (resultReady) { ImGui::TextUnformatted("Result ready"); }
    ImGui::BeginDisabled(!resultReady);
    if (ImGui::Button("Frame result", ImVec2(-1.0f, 0.0f))) { frameComparison(state, id); }
    ImGui::EndDisabled();
    auto translation = comparison->translation;
    if (ImGui::DragFloat3("Result position", translation.data(), .1f)) { setComparisonTranslation(state, id, translation); }
    ImGui::TextDisabled("Display offset only, in model units.");
    ImGui::Separator();
    ImGui::TextWrapped("Add objects from the scene's context menu. Right-click a branch or part below to remove it from that group.");
    ImGui::Separator();
    membershipTree(state, ComparisonSide::a);
    membershipTree(state, ComparisonSide::b);
    ImGui::Separator();
    if (ImGui::Button("Swap A / B")) { swapComparisonGroups(state); }
    auto settings = comparisonSettings(state);
    const auto initial = settings;
    const bool valid = canCompareGroups(state);
    ImGui::SameLine();

    ImGui::Checkbox("Visible", &settings.enabled);
    if (!valid) { ImGui::TextWrapped("Incomplete: each side needs mesh parts, and all missing or invalid references must be repaired or removed."); }
    {
        ImGui::TextWrapped(
            "Combined surfaces at scene positions, in model units. Hidden members are included. Other scene objects retain their own appearance.");
        const char *modes[] = {"Surface distance", "Group A", "Group B", "Overlay"};
        int mode = static_cast<int>(settings.mode);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##comparison_mode", &mode, modes, 4))
        {
            settings.mode = static_cast<ComparisonMode>(mode);
        }
        if (settings.mode == ComparisonMode::distance)
        {
            ImGui::Checkbox("Measure on A", &settings.distanceOnOriginal);
            ImGui::TextWrapped(settings.distanceOnOriginal ? "A -> B: distance from A to the nearest surface in B."
                                                           : "B -> A: distance from B to the nearest surface in A.");
            ImGui::SetNextItemWidth(110);
            ImGui::InputFloat("Tolerance", &settings.tolerance, 0, 0, "%.5g");
            ImGui::SetNextItemWidth(110);
            ImGui::InputFloat("Color maximum", &settings.colorRange, 0, 0, "%.5g");
            ImGui::TextColored(ImVec4(.65f, .70f, .77f, 1), "Gray: within tolerance");
            ImGui::TextColored(ImVec4(1, .65f, .25f, 1), "Yellow -> orange: increasing distance");
        }
        if (settings.mode == ComparisonMode::overlay)
        {
            ImGui::TextColored(ImVec4(.3f, .75f, 1, 1), "Blue: group A wireframe (X-ray)");
            ImGui::TextWrapped("Solid gray: group B surface");
        }
        ImGui::Checkbox("Triangle edges", &settings.showEdges);
        ImGui::Checkbox("Boundary edges (yellow)", &settings.showBoundaries);
        ImGui::Checkbox("Non-manifold / winding edges", &settings.showNonManifold);
    }
    // Merely opening the panel must not change scene settings.
    if (settings != initial)
    {
        setComparisonSettings(state, settings);
    }
    if (!valid || !comparisonSettings(state).enabled)
    {
        return;
    }
    const bool current = runtime.ready && runtime.resultSignature == comparisonGeometrySignature(state);
    if (!current)
    {
        if (!runtime.error.empty() && runtime.attemptedSignature == comparisonGeometrySignature(state))
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
    const bool useOriginal = originalActive(comparisonSettings(state));
    const auto &surface = useOriginal ? runtime.result.original : runtime.result.repaired;
    if (comparisonSettings(state).mode == ComparisonMode::distance)
    {
        if (ImGui::Button("Fit color range"))
        {
            settings = comparisonSettings(state);
            settings.colorRange = std::max(static_cast<float>(surface.maximum), settings.tolerance);
            setComparisonSettings(state, settings);
        }
        ImGui::Text("Sample max: %.5g", surface.maximum);
        ImGui::Text("Area-weighted mean: %.5g", surface.mean);
        ImGui::Text("Area-weighted P95: %.5g", surface.percentile95);
        ImGui::Text("Area above tolerance: %.2f%%", surfacePercentAboveTolerance(surface, comparisonSettings(state).tolerance));
        ImGui::TextWrapped("Approximate unsigned distance, four samples per triangle. Values are in model units.");
    }
    const auto &a = runtime.result.original.diagnostics;
    const auto &b = runtime.result.repaired.diagnostics;
    if (ImGui::BeginTable("Comparison diagnostics", 4, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Edges");
        ImGui::TableSetupColumn("A");
        ImGui::TableSetupColumn("B");
        ImGui::TableSetupColumn("Focus");
        ImGui::TableHeadersRow();
        diagnosticRow(state, "Boundary", a.boundaryEdges, b.boundaryEdges, useOriginal);
        diagnosticRow(state, "Non-manifold", a.nonManifoldEdges, b.nonManifoldEdges, useOriginal);
        diagnosticRow(state, "Winding", a.inconsistentWindingEdges, b.inconsistentWindingEdges, useOriginal);
        ImGui::EndTable();
    }
    ImGui::Text("Degenerate triangles: %zu -> %zu", a.degenerateTriangles, b.degenerateTriangles);
    ImGui::Text("Duplicate triangles: %zu -> %zu", a.duplicateTriangles, b.duplicateTriangles);
    ImGui::TextWrapped(
        "Diagnostics describe combined surfaces; coincident edges across parts are matched. Open boundaries may be intentional. Self-intersections are not checked.");
}

} // namespace

void drawComparisonPanelContents(UiState& state, ComparisonRuntimes& runtimes)
{
    if (findComparison(state)) {
        drawComparisonContents(state, runtimes.objects[state.activeComparisonId]);
    } else {
        ImGui::TextWrapped("Select two source objects and choose Create comparison, or start an empty comparison.");
        if (ImGui::Button("New comparison")) { createComparison(state); }
    }
}

static void submitComparisonScene(bgfx::ViewId view, const UiComparison& comparison, const ComparisonRuntime& runtime,
                           const ComparisonRuntimes& runtimes,
                           bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform)
{
    if (!comparison.settings.enabled || !runtime.ready) { return; }
    const auto& settings = comparison.settings;
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
            submitComparisonScene(view, comparison, it->second, runtimes, colorProgram, colorUniform);
        }
    }
}

void drawComparisonObjects(UiState& state)
{
    ImGui::Separator();
    ImGui::TextUnformatted("Comparisons");
    ImGui::SameLine();
    if (ImGui::SmallButton("New comparison")) { createComparison(state); }
    for (const auto& comparison : state.comparisons) {
        const auto id = comparison.objectId;
        const auto label = std::to_string(id);
        ImGui::PushID(label.c_str());
        auto settings = comparison.settings;
        if (ImGui::Checkbox("##visible", &settings.enabled)) { setComparisonSettings(state, settings, id); }
        ImGui::SameLine();
        if (ImGui::Selectable(comparison.name.c_str(), sceneObjectSelected(state, id))) { selectSceneObject(state, id); }
        bool changed = false;
        if (ImGui::BeginPopupContextItem("comparison_object")) {
            if (ImGui::MenuItem("Properties")) { selectSceneObject(state, id); }
            if (ImGui::MenuItem("Frame result", nullptr, false, canCompareGroups(state, id))) { frameComparison(state, id); }
            if (ImGui::MenuItem("Duplicate")) { duplicateComparison(state, id); changed = true; }
            if (ImGui::MenuItem("Delete comparison")) { removeComparison(state, id); changed = true; }
            ImGui::EndPopup();
        }
        if (!changed && !canCompareGroups(state, id)) { ImGui::TextDisabled("    Incomplete inputs"); }
        ImGui::PopID();
        if (changed) { break; }
    }
}
} // namespace woby
