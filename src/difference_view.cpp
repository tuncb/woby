#include "difference_runtime.h"
#include "math_utils.h"
#include "graphics_helpers.h"
#include "marker_pick.h"
#include "ui_icon_controls.h"
#include "ui_operations.h"
#include "utf8_path.h"
#include <imgui.h>
#include <imgui_stdlib.h>

namespace woby
{
void drawDifferenceInspector(UiState &state, ComparisonRuntime &runtime, SceneObjectId id)
{
    auto settings = comparisonSettings(state, id);
    const auto initial = settings;
    auto &d = settings.difference;
    auto &r = runtime.difference;
    ImGui::SeparatorText("Difference mesh");
    const bool busy = r.phase == "queued" || r.phase == "running";
    ImGui::BeginDisabled(busy);
    const bool rebuild = ImGui::Button(d.created ? "Rebuild difference mesh" : "Create difference mesh");
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
        ImGui::TextWrapped("Building and validating the difference mesh...");
    }
    if (r.phase == "outdated") {
        ImGui::TextWrapped("Out of date. Inputs changed. Rebuild to see the current difference.");
    }
    if (r.phase == "canceled") {
        ImGui::TextUnformatted("Difference creation canceled.");
    }
    if (!r.error.empty()) {
        ImGui::TextWrapped("Cannot create difference: %s", r.error.c_str());
    }
    if (r.phase == "ready" && r.result) {
        const auto &v = *r.result;
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
        ImGui::SeparatorText("Volume (model units cubed)");
        if (!v.constructed) {
            ImGui::Text("A only: %.9g", v.volumes[0]);
            ImGui::Text("B only: %.9g", v.volumes[1]);
            ImGui::Text("Net change: %.9g", v.volumes[1] - v.volumes[0]);
        }
        ImGui::Text("%s: %.9g", v.constructed ? "Constructed gap" : "Total changed", v.volumes[0] + v.volumes[1]);
        if (v.constructed) {
            ImGui::Text("Closure triangles: %zu", v.closureTriangles);
        }
        if (ImGui::Button("Frame result")) {
            frameComparison(state, id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Export OBJ...")) {
            ImGui::OpenPopup("difference_export");
        }
        ImGui::SetNextWindowPos(ImGui::GetItemRectMin(), ImGuiCond_Appearing);
        if (ImGui::BeginPopup("difference_export")) {
            ImGui::TextUnformatted("Absolute OBJ path (new file)");
            ImGui::InputText("##path", &r.exportPath);
            if (ImGui::Button("Export")) {
                try {
                    requestDifferenceExport(r, pathFromUtf8(r.exportPath));
                    ImGui::CloseCurrentPopup();
                } catch (const std::exception &e) {
                    r.exportError = e.what();
                }
            }
            ImGui::EndPopup();
        }
    }
    if (r.exportState == "running") {
        ImGui::TextUnformatted("Exporting OBJ...");
    }
    if (r.exportState == "complete") {
        ImGui::TextUnformatted("OBJ export complete.");
    }
    if (!r.exportError.empty()) {
        ImGui::TextWrapped("Export: %s", r.exportError.c_str());
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
    if (r.phase != "ready" || !r.result) {
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
