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
void filePicker(const UiState &state, const char *label, int &index)
{
    const std::string preview = index >= 0 && static_cast<size_t>(index) < state.files.size()
                                    ? pathToUtf8(state.files[static_cast<size_t>(index)].path.filename())
                                    : "Select file";
    ImGui::SetNextItemWidth(-1);
    ImGui::TextUnformatted(label);
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##file", preview.c_str()))
    {
        for (size_t i = 0; i < state.files.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            const auto name = pathToUtf8(state.files[i].path.filename());
            if (ImGui::Selectable(name.c_str(), index == static_cast<int>(i)))
            {
                index = static_cast<int>(i);
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("%s", pathToUtf8(state.files[i].path).c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
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
        auto original = comparisonWorldMesh(state, static_cast<size_t>(state.comparison.originalFile));
        auto repaired = comparisonWorldMesh(state, static_cast<size_t>(state.comparison.repairedFile));
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
    if (!ImGui::CollapsingHeader("Compare meshes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        return;
    }
    auto settings = state.comparison;
    if (settings.originalFile < 0 && !state.files.empty())
    {
        settings.originalFile = 0;
    }
    if (settings.repairedFile < 0 && state.files.size() > 1)
    {
        settings.repairedFile = 1;
    }
    const auto initial = settings;
    filePicker(state, "Original", settings.originalFile);
    filePicker(state, "Repaired", settings.repairedFile);
    const bool valid =
        settings.originalFile >= 0 && settings.repairedFile >= 0 && settings.originalFile != settings.repairedFile;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(settings.enabled ? "Exit comparison" : "Compare"))
    {
        settings.enabled = !settings.enabled;
    }
    ImGui::EndDisabled();
    if (!valid)
    {
        ImGui::TextWrapped("Load and select two different mesh files.");
    }
    if (settings.enabled)
    {
        ImGui::TextWrapped(
            "Whole files, scene positions, model units. Other scene objects are hidden during comparison.");
        const char *modes[] = {"Surface distance", "Original", "Repaired", "Overlay"};
        int mode = static_cast<int>(settings.mode);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##comparison_mode", &mode, modes, 4))
        {
            settings.mode = static_cast<ComparisonMode>(mode);
        }
        if (settings.mode == ComparisonMode::distance)
        {
            ImGui::Checkbox("Measure on original", &settings.distanceOnOriginal);
            ImGui::TextWrapped(settings.distanceOnOriginal ? "Original -> repaired: inspect removed regions."
                                                           : "Repaired -> original: inspect new surface regions.");
            ImGui::SetNextItemWidth(110);
            ImGui::InputFloat("Tolerance", &settings.tolerance, 0, 0, "%.5g");
            ImGui::SetNextItemWidth(110);
            ImGui::InputFloat("Color maximum", &settings.colorRange, 0, 0, "%.5g");
            ImGui::TextColored(ImVec4(.65f, .70f, .77f, 1), "Gray: within tolerance");
            ImGui::TextColored(ImVec4(1, .65f, .25f, 1), "Yellow -> orange: increasing distance");
        }
        if (settings.mode == ComparisonMode::overlay)
        {
            ImGui::TextColored(ImVec4(.3f, .75f, 1, 1), "Blue: original wireframe (X-ray)");
            ImGui::TextWrapped("Solid gray: repaired surface");
        }
        ImGui::Checkbox("Triangle edges", &settings.showEdges);
        ImGui::Checkbox("Boundary edges (yellow)", &settings.showBoundaries);
        ImGui::Checkbox("Non-manifold / winding edges", &settings.showNonManifold);
    }
    // Merely opening the panel must not create a dirty scene or choose files.
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
    if (ImGui::Button("Frame compared mesh"))
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
        ImGui::TableSetupColumn("Before");
        ImGui::TableSetupColumn("After");
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
        "Focus uses the displayed mesh. Open boundaries may be intentional. Self-intersections are not checked.");
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
