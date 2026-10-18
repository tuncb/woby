#include "cross_section_ui.h"
#include "cross_section_runtime.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <limits>

namespace woby {
namespace {
std::string sourceLabel(const UiState& state, SceneObjectId id)
{
    const auto object = findSceneObject(state, id);
    if (!object) {
        return "Missing source";
    }
    const auto file = findSceneObject(state, object->fileId);
    return file ? file->name + " / " + object->name : object->name;
}
std::optional<PickPoint> screenPoint(const ScenePickView& view, const std::array<float, 3>& p)
{
    const auto clip = annotationTransform(annotationCompose(view.view, view.projection), {p[0], p[1], p[2], 1});
    if (clip[3] <= 0) {
        return {};
    }
    return PickPoint{(clip[0] / clip[3] + 1) * .5f * static_cast<float>(view.width),
                     (1 - clip[1] / clip[3]) * .5f * static_cast<float>(view.height)};
}
void drawProfile(const CrossSectionSettings& s, const CrossSectionResult& result)
{
    ImGui::TextUnformatted("Face-on 2D | equal axis scale");
    const auto low = ImGui::GetCursorScreenPos();
    const ImVec2 size{std::max(120.0f, ImGui::GetContentRegionAvail().x), uiSize(250)};
    ImGui::InvisibleButton("section_plot", size);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 high{low.x + size.x, low.y + size.y};
    draw->AddRectFilled(low, high, ImGui::GetColorU32(ImGuiCol_FrameBg));
    std::array<double, 2> min{std::numeric_limits<double>::max(), std::numeric_limits<double>::max()},
        max{-min[0], -min[1]};
    bool any = false;
    for (size_t i = 0; i < result.contours.size(); ++i) {
        const auto& contour = result.contours[i];
        if (!s.sourceVisible[i] || contour.lines.empty()) {
            continue;
        }
        for (size_t k = 0; k < 2; ++k) {
            min[k] = std::min(min[k], contour.minimum[k]);
            max[k] = std::max(max[k], contour.maximum[k]);
        }
        any = true;
    }
    if (!any) {
        draw->AddText({low.x + 10, low.y + 10}, ImGui::GetColorU32(ImGuiCol_Text), "No visible cut contours");
        return;
    }
    const double scale = std::min((size.x - uiSize(36)) / std::max(max[0] - min[0], 1e-6),
                                  (size.y - uiSize(65)) / std::max(max[1] - min[1], 1e-6));
    const auto map = [&](const auto& p) {
        const auto xy = sectionCoordinates(s, p);
        return ImVec2{low.x + size.x * .5f + static_cast<float>((xy[0] - (min[0] + max[0]) * .5) * scale),
                      low.y + size.y * .5f - static_cast<float>((xy[1] - (min[1] + max[1]) * .5) * scale)};
    };
    draw->PushClipRect(low, high, true);
    for (size_t i = 0; i < result.contours.size(); ++i) {
        if (!s.sourceVisible[i]) {
            continue;
        }
        const auto c = sectionSourceColor(i);
        const auto color = ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], 1});
        for (const auto& line : result.contours[i].lines) {
            draw->AddLine(map(line.a), map(line.b), color, uiSize(2));
        }
    }
    if (s.showDifferences && result.largestGap && s.sourceVisible[static_cast<size_t>(s.reference)] &&
        s.sourceVisible[static_cast<size_t>(s.compared)]) {
        const auto a = map(result.largestGap->a), b = map(result.largestGap->b);
        draw->AddLine(a, b, ImGui::GetColorU32(ImGuiCol_Text), uiSize(2));
        draw->AddCircleFilled(a, uiSize(3), ImGui::GetColorU32(ImGuiCol_Text));
        draw->AddCircleFilled(b, uiSize(3), ImGui::GetColorU32(ImGuiCol_Text));
    }
    char text[128];
    std::snprintf(text, sizeof(text), "Along section: %.5g to %.5g", min[0], max[0]);
    draw->AddText({low.x + 8, high.y - ImGui::GetTextLineHeight() * 2 - 8}, ImGui::GetColorU32(ImGuiCol_Text), text);
    std::snprintf(text, sizeof(text), "Height: %.5g to %.5g | model units", min[1], max[1]);
    draw->AddText({low.x + 8, high.y - ImGui::GetTextLineHeight() - 5}, ImGui::GetColorU32(ImGuiCol_Text), text);
    draw->PopClipRect();
}
} // namespace
void drawCrossSectionInspector(UiState& state, SceneQueryRuntime* queries)
{
    const auto* item = selectedAnnotation(state);
    if (!item || !item->section) {
        return;
    }
    const auto id = item->objectId;
    auto s = *item->section;
    auto settings = item->settings;
    ImGui::PushID(static_cast<int>(id));
    ImGui::TextUnformatted("Cross section");
    bool style = ImGui::InputText("Name", &settings.name, ImGuiInputTextFlags_EnterReturnsTrue);
    style |= drawVisibilityField("Visible", settings.visible);
    style |= ImGui::Checkbox("Lock cut plane", &settings.locked);
    style |= ImGui::SliderFloat("Line width", &settings.width, 1, 8, "%.1f px");
    if (style) {
        setAnnotationSettings(state, id, settings);
    }
    bool changed = false;
    ImGui::SeparatorText("Cut plane");
    ImGui::Text("Vertical | model %s", s.upAxis == SceneUpAxis::z ? "Z" : "Y");
    ImGui::BeginDisabled(settings.locked);
    const auto normal = sectionNormal(s);
    float offset = 0;
    for (size_t k = 0; k < 3; ++k) {
        offset += s.a[k] * normal[k];
    }
    float moved = offset;
    if (ImGui::DragFloat("Position", &moved, std::max(state.sceneBounds.radius * .001f, 1e-5f), 0, 0, "%.6g")) {
        for (size_t k = 0; k < 3; ++k) {
            s.a[k] += (moved - offset) * normal[k];
            s.b[k] += (moved - offset) * normal[k];
        }
        changed = true;
    }
    auto a = s.a, b = s.b;
    if (ImGui::InputFloat3("Start", a.data(), "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.a = a;
        changed = true;
    }
    if (ImGui::InputFloat3("End", b.data(), "%.6g", ImGuiInputTextFlags_EnterReturnsTrue)) {
        s.b = b;
        changed = true;
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped(
        "Drag the cut line to move it. Drag an endpoint to rotate it. The square handle changes display lift.");
    std::shared_ptr<const CrossSectionResult> result;
    if (queries) {
        updateSceneAnnotationQueries(*queries, state);
        result = crossSectionResult(*queries, id);
    }
    ImGui::SeparatorText("Cut meshes");
    ImGui::TextWrapped("Captured sources. Source visibility does not change this set.");
    for (size_t i = 0; i < item->targetIds.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (drawVisibilityButton("contour", s.sourceVisible[i], "Contour")) {
            s.sourceVisible[i] = !s.sourceVisible[i];
            changed = true;
        }
        ImGui::SameLine();
        const auto color = sectionSourceColor(i);
        ImGui::ColorButton("source_color", {color[0], color[1], color[2], 1}, ImGuiColorEditFlags_NoTooltip,
                           ImVec2(uiSize(12), uiSize(12)));
        ImGui::SameLine();
        const auto object = findSceneObject(state, item->targetIds[i]);
        const char* status = !object                             ? "Missing source"
                             : !result                           ? "Updating"
                             : result->contours[i].lines.empty() ? "No cut"
                                                                 : "Cut";
        ImGui::TextWrapped("%s | %s", sourceLabel(state, item->targetIds[i]).c_str(), status);
        ImGui::PopID();
    }
    if (!result && queries) {
        const auto error = crossSectionError(*queries);
        if (!error.empty()) {
            ImGui::TextWrapped("%s", error.c_str());
        }
    }
    ImGui::SeparatorText("Display");
    changed |= ImGui::DragFloat("Display lift", &s.lift, std::max(state.sceneBounds.radius * .002f, 1e-5f), 0,
                                std::numeric_limits<float>::max(), "%.6g", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::Checkbox("Show section plane", &s.showPlane);
    changed |= ImGui::Checkbox("Show connection to cut", &s.showConnections);
    changed |= ImGui::Checkbox("Face-on 2D", &s.faceOn);
    ImGui::SeparatorText("2D differences");
    changed |= ImGui::Checkbox("Show 2D differences", &s.showDifferences);
    if (s.showDifferences) {
        for (const bool reference : {true, false}) {
            int& chosen = reference ? s.reference : s.compared;
            const auto label = item->targetIds.empty()
                                   ? std::string("Missing source")
                                   : sourceLabel(state, item->targetIds[static_cast<size_t>(chosen)]);
            if (ImGui::BeginCombo(reference ? "Reference" : "Compare", label.c_str())) {
                for (size_t i = 0; i < item->targetIds.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::Selectable(sourceLabel(state, item->targetIds[i]).c_str(),
                                          chosen == static_cast<int>(i))) {
                        chosen = static_cast<int>(i);
                        changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
        }
        if (s.reference == s.compared) {
            ImGui::TextWrapped("Choose two different meshes.");
        } else if (changed) {
            ImGui::TextUnformatted("Updating section...");
        } else if (result && result->largestGap) {
            ImGui::Text("Largest sampled 2D gap: %.6g", result->gap);
        } else {
            ImGui::TextUnformatted(result ? "Gap unavailable: both meshes must have a cut." : "Updating section...");
        }
        ImGui::TextWrapped("Both directions. Up to 512 endpoints per mesh. Model units. Measures only this plane.");
    }
    if (changed) {
        try {
            setCrossSectionSettings(state, id, s);
        } catch (const std::exception& error) {
            ImGui::TextWrapped("%s", error.what());
        }
    }
    if (s.faceOn && result && !changed) {
        drawProfile(s, *result);
    }
    if (ImGui::CollapsingHeader("Comments")) {
        if (ImGui::InputTextMultiline("##note", &settings.note)) {
            setAnnotationSettings(state, id, settings);
        }
    }
    ImGui::PopID();
}
bool beginSectionPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView& view, PickPoint point)
{
    const auto* selected = selectedAnnotation(state);
    const bool creating = interaction.sectionTool;
    if (!creating && (!selected || !selected->section || !selected->settings.visible || selected->settings.locked)) {
        return false;
    }
    CrossSectionSettings s;
    int handle = -1;
    if (creating) {
        s.upAxis = state.upAxis;
        const size_t up = s.upAxis == SceneUpAxis::z ? 2 : 1;
        s.a = state.sceneBounds.center;
        s.a[up] = state.sceneBounds.min[up];
        s.lift = state.sceneBounds.max[up] - s.a[up] + state.sceneBounds.radius * .3f;
    } else {
        s = *selected->section;
        const auto positions = annotationControlWorldPositions(*selected, {});
        for (size_t i = 0; i < positions.size(); ++i) {
            if (const auto screen = screenPoint(view, positions[i]);
                screen && std::hypot(point[0] - (*screen)[0], point[1] - (*screen)[1]) < 10 * view.pixelScale) {
                handle = static_cast<int>(i);
                break;
            }
        }
        if (handle < 0) {
            const auto a = screenPoint(view, s.a), b = screenPoint(view, s.b);
            if (!a || !b) {
                return false;
            }
            const float dx = (*b)[0] - (*a)[0], dy = (*b)[1] - (*a)[1], length = dx * dx + dy * dy;
            const float t =
                length > 0 ? std::clamp(((point[0] - (*a)[0]) * dx + (point[1] - (*a)[1]) * dy) / length, 0.0f, 1.0f)
                           : 0;
            if (std::hypot(point[0] - (*a)[0] - t * dx, point[1] - (*a)[1] - t * dy) > 8 * view.pixelScale) {
                return false;
            }
            handle = 3;
        }
    }
    const auto world = sectionWorkplanePoint(view, point, s.upAxis, s.a[s.upAxis == SceneUpAxis::z ? 2 : 1]);
    if (!world && handle != 2) {
        interaction.error = "Use an angled view or Top view to draw on the section workplane.";
        return true;
    }
    interaction.view = view;
    interaction.pointerStart = interaction.pointerEnd = point;
    interaction.identity = annotationWorkIdentity(state);
    interaction.handle = handle;
    interaction.editing = creating ? 0 : selected->objectId;
    interaction.error.clear();
    interaction.preview = creating ? UiAnnotation{} : *selected;
    interaction.preview.targetValid = true;
    if (creating) {
        s.a = *world;
        s.b = *world;
        for (const auto& part : scenePickParts(state)) {
            if (part.indexCount && part.mesh) {
                interaction.preview.targetIds.push_back(part.objectId);
            }
        }
        if (interaction.preview.targetIds.empty()) {
            interaction.error = "No visible triangle meshes.";
            return true;
        }
        interaction.preview.targetId = interaction.preview.targetIds.front();
    }
    interaction.sectionStart = s;
    interaction.sectionGrab = world.value_or(s.a);
    interaction.preview.section = s;
    interaction.dragging = true;
    return true;
}
void moveSectionPointer(const UiState&, AnnotationInteraction& interaction, PickPoint point, bool lockDirection)
{
    if (!interaction.dragging || !interaction.sectionStart) {
        return;
    }
    interaction.pointerEnd = point;
    auto s = *interaction.sectionStart;
    const size_t up = s.upAxis == SceneUpAxis::z ? 2 : 1;
    if (interaction.handle == 2) {
        auto p = s.a;
        for (size_t k = 0; k < 3; ++k) {
            p[k] = (s.a[k] + s.b[k]) * .5f;
        }
        p[up] += s.lift;
        auto q = p;
        q[up] += 1;
        const auto a = screenPoint(interaction.view, p), b = screenPoint(interaction.view, q);
        if (a && b) {
            const float dx = (*b)[0] - (*a)[0], dy = (*b)[1] - (*a)[1], length = dx * dx + dy * dy;
            if (length > 1e-12f) {
                s.lift = std::max(0.0f, s.lift + ((point[0] - interaction.pointerStart[0]) * dx +
                                                  (point[1] - interaction.pointerStart[1]) * dy) /
                                                     length);
            }
        }
    } else if (const auto p = sectionWorkplanePoint(interaction.view, point, s.upAxis, s.a[up])) {
        if (!interaction.editing || interaction.handle == 1) {
            s.b = lockDirection ? snapSectionEndpoint(s.a, *p, s.upAxis) : *p;
        } else if (interaction.handle == 0) {
            s.a = lockDirection ? snapSectionEndpoint(s.b, *p, s.upAxis) : *p;
        } else {
            for (size_t k = 0; k < 3; ++k) {
                s.a[k] += (*p)[k] - interaction.sectionGrab[k];
                s.b[k] += (*p)[k] - interaction.sectionGrab[k];
            }
        }
    }
    interaction.preview.section = s;
}
void endSectionPointer(UiState& state, AnnotationInteraction& interaction, bool allowed)
{
    if (!interaction.preview.section) {
        return;
    }
    std::string error;
    if (allowed && annotationWorkCurrent(interaction.identity, state, true)) {
        try {
            if (interaction.editing) {
                setCrossSectionSettings(state, interaction.editing, *interaction.preview.section);
            } else if (std::hypot(interaction.pointerEnd[0] - interaction.pointerStart[0],
                                  interaction.pointerEnd[1] - interaction.pointerStart[1]) >=
                       4 * interaction.view.pixelScale) {
                createCrossSection(state, *interaction.preview.section, interaction.preview.targetIds);
            }
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    cancelAnnotationPointer(interaction);
    interaction.error = std::move(error);
}
} // namespace woby
