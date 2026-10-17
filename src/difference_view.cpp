#include "difference_runtime.h"
#include "count_format.h"
#include "math_utils.h"
#include "graphics_helpers.h"
#include "marker_pick.h"
#include "ui_icon_controls.h"
#include "ui_operations.h"
#include <imgui.h>

namespace woby
{
void drawDifferenceInspector(UiState &state, ComparisonRuntime &runtime, SceneObjectId id)
{
    auto settings = comparisonSettings(state, id);
    auto initial = settings;
    auto &d = settings.difference;
    auto &r = runtime.difference;
    ImGui::SeparatorText("Difference mesh");
    const bool checking = r.resolution.phase == "queued" || r.resolution.phase == "running";
    const bool busy = r.phase == "queued" || r.phase == "running" || checking;
    ImGui::BeginDisabled(busy);
    int method = static_cast<int>(d.method);
    const char *methods[] = {"Direct mesh", "Approximate volume"};
    if (ImGui::Combo("Method", &method, methods, 2)) { d.method = static_cast<DifferenceMethod>(method); }
    if (d.method == DifferenceMethod::approximate) {
        ImGui::InputDouble("Cell size", &d.cellSize, 0, 0, "%.6g");
        ImGui::TextWrapped("Model units. Smaller cells retain more detail and use more memory.");
        if (ImGui::TreeNode("Input assumptions")) {
            ImGui::TextWrapped("Both inputs must represent solids with reasonably consistent triangle orientation. Missing surfaces are inferred. Open sheets need a separate closure rule.");
            ImGui::TextWrapped("Parametric surfaces use their current tessellation. Smaller cells do not add surface detail.");
            ImGui::TreePop();
        }
    }
    if (settings != initial) {
        setComparisonSettings(state, settings, id);
        settings = comparisonSettings(state, id);
        initial = settings;
    }
    ImGui::BeginDisabled(!enabledComparisonPartCount(state, ComparisonSide::a, id) ||
                         !enabledComparisonPartCount(state, ComparisonSide::b, id));
    const bool rebuild = ImGui::Button(d.created ? "Rebuild difference mesh" : "Create difference mesh");
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (rebuild) {
        requestDifferenceMesh(state, id);
        return;
    }
    if (d.created && !busy) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            requestDifferenceMesh(state, id, true);
            settings.difference = {};
            setComparisonSettings(state, settings, id);
            return;
        }
    }
    if (busy) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            if (checking) { requestDifferenceResolutionCheck(state, id, true); }
            else { requestDifferenceMesh(state, id, true); }
        }
        ImGui::TextWrapped("%s", checking ? "Checking volumes at half the cell size..." : "Building the difference mesh...");
    }
    const bool current = differenceResultCurrent(r, state, id);
    if (r.phase == "outdated" || (r.phase == "ready" && !current)) {
        ImGui::TextWrapped("Out of date. Inputs or method settings changed. Rebuild to see the current difference.");
    }
    if (r.phase == "canceled") {
        ImGui::TextUnformatted("Difference creation canceled.");
    }
    if (!r.error.empty()) {
        ImGui::TextWrapped("Cannot create difference: %s", r.error.c_str());
        if (d.method == DifferenceMethod::direct && ImGui::Button("Use approximate volume")) {
            d.method = DifferenceMethod::approximate;
            setComparisonSettings(state, settings, id);
            return;
        }
    }
    if (current) {
        const auto &v = *r.result;
        const bool approximate = v.construction.method == DifferenceMethod::approximate;
        if (approximate) {
            ImGui::SeparatorText("Approximate result");
            ImGui::Text("Computed with cell size %.6g", v.construction.cellSize);
        }
        ImGui::TextWrapped("%s", v.message.c_str());
        const auto visibility = [&](const char *label, bool &value) {
            if (drawVisibilityButton(label, value, label)) {
                value = !value;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        };
        visibility(v.constructed ? "Gap surfaces" : "A only (removed from A)", d.showA);
        if (!v.constructed) {
            visibility("B only (added in B)", d.showB);
        } else {
            visibility("Generated closure walls", d.showWalls);
        }
        ImGui::SliderFloat("Opacity", &d.opacity, .05f, 1, "%.2f");
        ImGui::SeparatorText(approximate ? "Approximate volume (model units cubed)" : "Volume (model units cubed)");
        if (!v.constructed) {
            ImGui::Text("A only: %.9g", v.volumes[0]);
            ImGui::Text("B only: %.9g", v.volumes[1]);
            ImGui::Text("Net change: %.9g", v.volumes[1] - v.volumes[0]);
        }
        ImGui::Text("%s: %.9g", v.constructed ? "Constructed gap" : "Total changed", v.volumes[0] + v.volumes[1]);
        if (v.constructed) {
            ImGui::Text("Closure triangles: %s", formatCount(v.closureTriangles).c_str());
        }
        if (approximate) {
            ImGui::SeparatorText("Resolution check");
            const auto &resolution = r.resolution;
            if (resolution.base == r.result && resolution.phase == "ready") {
                const auto total = v.volumes[0] + v.volumes[1];
                const auto refined = resolution.volumes[0] + resolution.volumes[1];
                ImGui::Text("Cell size %.6g: total %.9g", resolution.cellSize, refined);
                ImGui::Text("A only: %.9g; B only: %.9g", resolution.volumes[0], resolution.volumes[1]);
                ImGui::Text("Total change: %+.9g", refined - total);
                if (total > 0) { ImGui::Text("Relative change: %+.3g%%", 100 * (refined - total) / total); }
                ImGui::TextWrapped("This is a change with resolution, not an error bound.");
            } else if (resolution.phase == "failed" && resolution.base == r.result) {
                ImGui::TextWrapped("Resolution check: %s", resolution.error.c_str());
            } else if (resolution.phase == "canceled") {
                ImGui::TextUnformatted("Resolution check canceled.");
            } else if (!checking) { ImGui::TextUnformatted("Resolution check: not run."); }
            ImGui::BeginDisabled(busy);
            if (ImGui::Button("Check at half the cell size")) { requestDifferenceResolutionCheck(state, id); }
            ImGui::EndDisabled();
        }
        if (ImGui::Button("Frame result")) {
            frameComparison(state, id);
        }
    }
    ImGui::TextWrapped("Uses the full geometry. Heatmap colors and acceptance tolerance do not change this mesh.");
    if (settings != initial) {
        setComparisonSettings(state, settings, id);
    }
}
void submitDifferenceMesh(graphics::ViewId view, const UiComparison &comparison, const DifferenceRuntime &r,
                          graphics::ProgramHandle program, graphics::ProgramHandle markerProgram)
{
    if (r.phase != "ready" || !r.result || !r.uploaded ||
        !sameDifferenceConstruction(r.result->construction, comparison.settings.difference)) {
        return;
    }
    const auto &s = comparison.settings.difference;
    float matrix[16];
    translationMatrix(matrix, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
    const std::array<bool, 3> show{s.showA, s.showB, s.showWalls};
    const std::array<std::array<float, 4>, 3> colors{
        {{.15f, .72f, .95f, s.opacity}, {1, .48f, .1f, s.opacity}, {.9f, .72f, .25f, s.opacity}}};
    for (size_t i = 0; i < 3; ++i) {
        const auto &g = r.gpu[i];
        if (!show[i] || !graphics::isValid(g.vertices) || !graphics::isValid(g.triangles)) {
            continue;
        }
        graphics::setTransform(matrix);
        graphics::setComparison({0, 0, 0, 0});
        graphics::setColor(colors[i]);
        graphics::setVertexBuffer(0, g.vertices);
        graphics::setIndexBuffer(g.triangles);
        setMarkerRenderState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_WRITE_Z |
                           WOBY_GPU_STATE_DEPTH_TEST_LEQUAL | WOBY_GPU_STATE_BLEND_ALPHA | WOBY_GPU_STATE_MSAA,
                           graphics::isValid(markerProgram));
        graphics::submit(view, graphics::isValid(markerProgram) ? markerProgram : program);
    }
}
void appendDifferencePickParts(std::vector<ScenePickPart> &parts, const UiComparison &comparison,
                               const DifferenceRuntime &r)
{
    if (r.phase != "ready" || !r.result ||
        !sameDifferenceConstruction(r.result->construction, comparison.settings.difference)) {
        return;
    }
    const auto &s = comparison.settings.difference;
    const std::array<bool, 3> show{s.showA, s.showB, s.showWalls};
    for (size_t i = 0; i < 3; ++i) {
        const auto &m = r.result->parts[i];
        if (!show[i] || meshGeometry(m).indices.empty()) {
            continue;
        }
        ScenePickPart p;
        p.objectId = comparison.objectId;
        p.mesh = &m;
        p.indexCount = meshGeometry(m).indices.size();
        p.bounds = m.bounds;
        p.solid = true;
        p.surfaceLessEqual = true;
        p.opacity = s.opacity;
        translationMatrix(p.model.data(), comparison.translation[0], comparison.translation[1],
                          comparison.translation[2]);
        parts.push_back(p);
    }
}
} // namespace woby
