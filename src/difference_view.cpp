#include "difference_runtime.h"
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
    const bool busy = r.phase == "queued" || r.phase == "running";
    ImGui::TextWrapped("Uses the input triangles. Requires closed solids. Surface intersections are not checked; volumes are estimates.");
    ImGui::BeginDisabled(busy);
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
            requestDifferenceMesh(state, id, true);
        }
        ImGui::TextWrapped("%s", "Building the difference mesh...");
    }
    const bool current = differenceResultCurrent(r, state, id);
    if (r.phase == "outdated" || (r.phase == "ready" && !current)) {
        ImGui::TextWrapped("Out of date. Inputs changed. Rebuild to see the current difference.");
    }
    if (r.phase == "canceled") {
        ImGui::TextUnformatted("Difference creation canceled.");
    }
    if (!r.error.empty()) {
        ImGui::TextWrapped("Cannot create difference: %s", r.error.c_str());
    }
    if (current) {
        const auto &v = *r.result;
        ImGui::SeparatorText("Approximate result");
        ImGui::TextWrapped("%s", v.message.c_str());
        const auto visibility = [&](const char *label, bool &value) {
            if (drawVisibilityButton(label, value, label)) {
                value = !value;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
        };
        visibility("A only (removed from A)", d.showA);
        visibility("B only (added in B)", d.showB);
        ImGui::SliderFloat("Opacity", &d.opacity, .05f, 1, "%.2f");
        ImGui::SeparatorText("Approximate volume (model units cubed)");
        ImGui::Text("A only: %.9g", v.volumes[0]);
        ImGui::Text("B only: %.9g", v.volumes[1]);
        ImGui::Text("Net change: %.9g", v.volumes[1] - v.volumes[0]);
        ImGui::Text("Total changed: %.9g", v.volumes[0] + v.volumes[1]);
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
    if (r.phase != "ready" || !r.result || !r.uploaded) {
        return;
    }
    const auto &s = comparison.settings.difference;
    float matrix[16];
    translationMatrix(matrix, comparison.translation[0], comparison.translation[1], comparison.translation[2]);
    const std::array<bool, 2> show{s.showA, s.showB};
    const std::array<std::array<float, 4>, 2> colors{
        {{.15f, .72f, .95f, s.opacity}, {1, .48f, .1f, s.opacity}}};
    for (size_t i = 0; i < r.result->parts.size(); ++i) {
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
    if (r.phase != "ready" || !r.result) {
        return;
    }
    const auto &s = comparison.settings.difference;
    const std::array<bool, 2> show{s.showA, s.showB};
    for (size_t i = 0; i < r.result->parts.size(); ++i) {
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
