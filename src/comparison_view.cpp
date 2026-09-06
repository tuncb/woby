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
    std::vector<std::array<float, 3>> points;
    points.reserve(edges.size() * 2);
    for (const auto &edge : edges)
    {
        points.push_back(edge.a);
        points.push_back(edge.b);
    }
    const auto handle = bgfx::createVertexBuffer(
        bgfx::copy(points.data(), static_cast<uint32_t>(points.size() * sizeof(points[0]))), helperLineVertexLayout());
    if (!bgfx::isValid(handle))
    {
        throw std::runtime_error("Cannot allocate comparison edge buffer.");
    }
    return handle;
}
void uploadSurface(ComparisonGpuSurface &gpu, const SurfaceComparison &surface)
{
    auto vertices = surface.source.vertices;
    generateSmoothNormals(vertices, surface.source.indices);
    gpu.vertices = bgfx::createVertexBuffer(
        bgfx::copy(vertices.data(), static_cast<uint32_t>(vertices.size() * sizeof(Vertex))), meshVertexLayout());
    const auto &indices = surface.source.indices;
    gpu.triangles = bgfx::createIndexBuffer(
        bgfx::copy(indices.data(), static_cast<uint32_t>(indices.size() * sizeof(uint32_t))), BGFX_BUFFER_INDEX32);
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
        bgfx::copy(lines.data(), static_cast<uint32_t>(lines.size() * sizeof(uint32_t))), BGFX_BUFFER_INDEX32);
    const auto &samples = surface.sampled.vertices;
    gpu.samples = bgfx::createVertexBuffer(
        bgfx::copy(samples.data(), static_cast<uint32_t>(samples.size() * sizeof(Vertex))), meshVertexLayout());
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
void membershipList(UiState& state, ComparisonSide side)
{
    const char* label = side == ComparisonSide::a ? "Group A" : "Group B";
    ImGui::PushID(label);
    const size_t count = comparisonPartCount(state, side);
    size_t triangles = 0;
    for (const auto& file : state.files) {
        for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
            if (comparisonMember(file.groupSettings[i].comparison, side)) {
                triangles += file.mesh.nodes[i].indexCount / 3;
            }
        }
    }
    ImGui::Text("%s: %zu parts, %zu triangles", label, count, triangles);
    ImGui::SameLine();
    ImGui::BeginDisabled(count == 0);
    if (ImGui::SmallButton("Clear")) { clearComparisonGroup(state, side); }
    ImGui::EndDisabled();
    if (count != 0) {
        const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min(count, size_t{5}));
        if (ImGui::BeginChild("members", ImVec2(0, height))) {
            for (const auto& file : state.files) {
                for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
                    const auto& group = file.groupSettings[i];
                    if (!comparisonMember(group.comparison, side)) { continue; }
                    const std::string name = pathToUtf8(file.path.filename()) + " / " + file.mesh.nodes[i].name;
                    const std::string id = std::to_string(group.objectId);
                    ImGui::PushID(id.c_str());
                    ImGui::TextUnformatted(name.c_str());
                    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", pathToUtf8(file.path).c_str()); }
                    if (ImGui::BeginPopupContextItem("member")) {
                        if (ImGui::MenuItem("Remove from group")) {
                            setComparisonObjects(state, {group.objectId}, side, false);
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
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
        auto settings = state.comparison;
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
                 bgfx::UniformHandle uniform, const std::array<float, 4> &color)
{
    if (!bgfx::isValid(vertices))
    {
        return;
    }
    float identity[16];
    bx::mtxIdentity(identity);
    bgfx::setTransform(identity);
    bgfx::setVertexBuffer(0, vertices);
    bgfx::setUniform(uniform, color.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_LINES | BGFX_STATE_DEPTH_TEST_ALWAYS |
                   BGFX_STATE_MSAA);
    bgfx::submit(view, program);
}
void submitWire(bgfx::ViewId view, const ComparisonGpuSurface &gpu, bgfx::ProgramHandle program,
                bgfx::UniformHandle uniform, const std::array<float, 4> &color, bool xray)
{
    float identity[16];
    bx::mtxIdentity(identity);
    bgfx::setTransform(identity);
    bgfx::setVertexBuffer(0, gpu.vertices);
    bgfx::setIndexBuffer(gpu.lines);
    bgfx::setUniform(uniform, color.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_PT_LINES | BGFX_STATE_MSAA |
                   (xray ? BGFX_STATE_DEPTH_TEST_ALWAYS : BGFX_STATE_DEPTH_TEST_LEQUAL));
    bgfx::submit(view, program);
}
} // namespace

void updateComparisonRuntime(ComparisonRuntime &runtime, const UiState &state)
{
    const uint64_t wanted = state.comparison.enabled ? comparisonGeometrySignature(state) : 0;
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
    if (wanted == 0 || runtime.ready || runtime.worker.valid() || wanted == runtime.attemptedSignature)
    {
        return;
    }
    runtime.attemptedSignature = wanted;
    runtime.workerSignature = wanted;
    runtime.error.clear();
    try
    {
        auto original = comparisonWorldMesh(state, ComparisonSide::a);
        auto repaired = comparisonWorldMesh(state, ComparisonSide::b);
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

void destroyComparisonRuntime(ComparisonRuntime &runtime)
{
    if (runtime.worker.valid())
    {
        runtime.stop.request_stop();
        runtime.worker.wait();
    }
    destroySurface(runtime.originalGpu);
    destroySurface(runtime.repairedGpu);
    if (bgfx::isValid(runtime.program))
    {
        bgfx::destroy(runtime.program);
        runtime.program = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(runtime.parameters))
    {
        bgfx::destroy(runtime.parameters);
        runtime.parameters = BGFX_INVALID_HANDLE;
    }
}

void drawComparisonPanel(UiState &state, ComparisonRuntime &runtime)
{
    if (runtime.openPanelRequested) {
        ImGui::SetNextItemOpen(true);
        runtime.openPanelRequested = false;
    }
    if (!ImGui::CollapsingHeader("Compare groups", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    ImGui::TextWrapped("Right-click scene objects to add or remove them from A and B. Files and folders add their current mesh parts.");
    membershipList(state, ComparisonSide::a);
    membershipList(state, ComparisonSide::b);
    if (ImGui::Button("Swap A / B")) { swapComparisonGroups(state); }
    auto settings = state.comparison;
    const auto initial = settings;
    const bool valid = canCompareGroups(state);
    ImGui::SameLine();
    ImGui::BeginDisabled(!valid && !settings.enabled);
    if (ImGui::Button(settings.enabled ? "Exit comparison" : "Compare A and B")) {
        settings.enabled = !settings.enabled;
    }
    ImGui::EndDisabled();
    if (!valid) { ImGui::TextWrapped("Add at least one mesh part with triangles to each group."); }
    if (settings.enabled)
    {
        ImGui::TextWrapped(
            "Combined surfaces at scene positions, in model units. Hidden members are included. Other scene objects are hidden during comparison.");
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
    if (!state.comparison.enabled)
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
    const bool useOriginal = originalActive(state.comparison);
    const auto &surface = useOriginal ? runtime.result.original : runtime.result.repaired;
    if (ImGui::Button("Frame compared group"))
    {
        frameComparisonBounds(state, surface.source.bounds);
    }
    if (state.comparison.mode == ComparisonMode::distance)
    {
        if (ImGui::Button("Fit color range"))
        {
            settings = state.comparison;
            settings.colorRange = std::max(static_cast<float>(surface.maximum), settings.tolerance);
            setComparisonSettings(state, settings);
        }
        ImGui::Text("Sample max: %.5g", surface.maximum);
        ImGui::Text("Area-weighted mean: %.5g", surface.mean);
        ImGui::Text("Area-weighted P95: %.5g", surface.percentile95);
        ImGui::Text("Area above tolerance: %.2f%%", surfacePercentAboveTolerance(surface, state.comparison.tolerance));
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

bool submitComparisonScene(bgfx::ViewId view, const UiState &state, const ComparisonRuntime &runtime,
                           bgfx::ProgramHandle colorProgram, bgfx::UniformHandle colorUniform)
{
    bgfx::setViewMode(view, state.comparison.enabled ? bgfx::ViewMode::Sequential : bgfx::ViewMode::Default);
    if (!state.comparison.enabled)
    {
        return false;
    }
    // Never present a stale result while recomputing after geometry changes.
    if (!runtime.ready)
    {
        return true;
    }
    const auto &settings = state.comparison;
    const bool useOriginal = originalActive(settings);
    const auto &gpu = useOriginal ? runtime.originalGpu : runtime.repairedGpu;
    const bool heatmap = settings.mode == ComparisonMode::distance;
    float identity[16];
    bx::mtxIdentity(identity);
    const std::array<float, 4> parameters = {settings.tolerance, settings.colorRange, heatmap ? 1.0f : 0.0f, 0};
    const std::array<float, 4> gray = {.58f, .63f, .69f, 1};
    bgfx::setTransform(identity);
    bgfx::setUniform(runtime.parameters, parameters.data());
    bgfx::setUniform(colorUniform, gray.data());
    bgfx::setVertexBuffer(0, heatmap ? gpu.samples : gpu.vertices);
    if (!heatmap)
    {
        bgfx::setIndexBuffer(gpu.triangles);
    }
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS |
                   BGFX_STATE_MSAA);
    bgfx::submit(view, runtime.program);
    if (settings.showEdges)
    {
        submitWire(view, gpu, colorProgram, colorUniform, {.22f, .25f, .30f, 1}, false);
    }
    if (settings.mode == ComparisonMode::overlay)
    {
        submitWire(view, runtime.originalGpu, colorProgram, colorUniform, {.3f, .75f, 1, 1}, true);
    }
    if (settings.showBoundaries)
    {
        submitEdges(view, gpu.boundaries, colorProgram, colorUniform, {1, .85f, .15f, 1});
    }
    if (settings.showNonManifold)
    {
        submitEdges(view, gpu.nonManifold, colorProgram, colorUniform, {1, .15f, .55f, 1});
        submitEdges(view, gpu.winding, colorProgram, colorUniform, {1, .2f, .2f, 1});
    }
    return true;
}
} // namespace woby
