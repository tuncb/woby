#include "annotation_ui.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"

#include <bx/math.h>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace woby {
namespace {
std::vector<PickPoint> handles(const UiAnnotation& item, const ScenePickPart& target, const ScenePickView& view)
{
    std::vector<PickPoint> result;
    const auto transform = annotationCompose(target.model, annotationCompose(view.view, view.projection));
    for (const auto& nearest : annotationControlPositions(*target.mesh, target.indexOffset, item.geometry)) {
        const auto clip = annotationTransform(transform, {nearest[0], nearest[1], nearest[2], 1});
        const float minimumZ = view.homogeneousDepth ? -clip[3] : 0;
        if (clip[3] <= 0 || clip[2] < minimumZ || clip[2] > clip[3]) { result.push_back({-100000, -100000}); }
        else { result.push_back({(clip[0] / clip[3] + 1) * .5f * static_cast<float>(view.width),
            (1 - clip[1] / clip[3]) * .5f * static_cast<float>(view.height)}); }
    }
    return result;
}
void submitLines(bgfx::ViewId viewId, const std::vector<DiagnosticEdge>& lines, const ScenePickView& view,
    const ScenePickPart* target, const AnnotationGeometry& geometry,
    const AnnotationSettings& settings, const bgfx::VertexLayout& layout, bgfx::ProgramHandle program,
    bgfx::UniformHandle colorUniform)
{
    const auto vp = annotationCompose(view.view, view.projection);
    PickMatrix inverse;
    bx::mtxInverse(inverse.data(), vp.data());
    std::vector<std::array<float, 3>> vertices;
    size_t segmentIndex = 0;
    for (const auto& line : lines) {
        const auto& segment = geometry.segments[segmentIndex++];
        double slopeX = 0, slopeY = 0;
        if (target && target->mesh) {
            const auto transform = annotationCompose(target->model, vp);
            std::array<std::array<float, 4>, 3> corners;
            for (size_t k = 0; k < 3; ++k) {
                const auto index = target->mesh->indices[target->indexOffset + static_cast<size_t>(segment.triangle) * 3 + k];
                const auto& p = target->mesh->vertices[index].position;
                corners[k] = annotationTransform(transform, {p[0], p[1], p[2], 1});
            }
            // NDC depth is affine over the projected face. Extend the stroke in
            // that plane, preventing its uphill half from sinking into the mesh.
            if (std::all_of(corners.begin(), corners.end(), [](const auto& p) { return std::abs(p[3]) > 1e-12f; })) {
                const double x0 = corners[0][0] / corners[0][3], y0 = corners[0][1] / corners[0][3], z0 = corners[0][2] / corners[0][3];
                const double dx1 = corners[1][0] / corners[1][3] - x0, dy1 = corners[1][1] / corners[1][3] - y0, dz1 = corners[1][2] / corners[1][3] - z0;
                const double dx2 = corners[2][0] / corners[2][3] - x0, dy2 = corners[2][1] / corners[2][3] - y0, dz2 = corners[2][2] / corners[2][3] - z0;
                const double determinant = dx1 * dy2 - dx2 * dy1;
                if (std::abs(determinant) > 1e-15) {
                    slopeX = (dz1 * dy2 - dz2 * dy1) / determinant;
                    slopeY = (dx1 * dz2 - dx2 * dz1) / determinant;
                }
            }
        }
        auto a = annotationTransform(vp, {line.a[0], line.a[1], line.a[2], 1});
        auto b = annotationTransform(vp, {line.b[0], line.b[1], line.b[2], 1});
        // Clip line centers before expanding, including lines crossing the near plane.
        bool visible = true;
        for (size_t plane = 0; plane < 6; ++plane) {
            const auto distance = [&](const auto& p) {
                if (plane < 4) { return p[3] + (plane % 2 == 0 ? p[plane / 2] : -p[plane / 2]); }
                return plane == 4 ? (view.homogeneousDepth ? p[3] + p[2] : p[2]) : p[3] - p[2];
            };
            const float da = distance(a), db = distance(b);
            if (da < 0 && db < 0) { visible = false; break; }
            if ((da < 0) != (db < 0)) {
                const float t = da / (da - db);
                std::array<float, 4> p;
                for (size_t k = 0; k < 4; ++k) { p[k] = a[k] + t * (b[k] - a[k]); }
                (da < 0 ? a : b) = p;
            }
        }
        if (!visible || a[3] <= 0 || b[3] <= 0) { continue; }
        const float dx = (b[0] / b[3] - a[0] / a[3]) * static_cast<float>(view.width);
        const float dy = (b[1] / b[3] - a[1] / a[3]) * static_cast<float>(view.height);
        const float length = std::hypot(dx, dy);
        if (length < 1e-6f) { continue; }
        const float ox = -dy / length * settings.width * view.pixelScale / static_cast<float>(view.width);
        const float oy = dx / length * settings.width * view.pixelScale / static_cast<float>(view.height);
        std::array<std::array<float, 3>, 4> corners;
        for (size_t i = 0; i < 4; ++i) {
            auto p = i < 2 ? a : b;
            const float sign = i % 2 == 0 ? 1.0f : -1.0f;
            p[0] += sign * ox * p[3]; p[1] += sign * oy * p[3];
            p[2] += static_cast<float>(sign * (slopeX * ox + slopeY * oy) - 1e-6) * p[3];
            p = annotationTransform(inverse, p);
            corners[i] = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
        }
        for (size_t i : {0u, 1u, 2u, 2u, 1u, 3u}) { vertices.push_back(corners[i]); }
    }
    if (vertices.empty()) { return; }
    const auto count = static_cast<uint32_t>(vertices.size());
    if (bgfx::getAvailTransientVertexBuffer(count, layout) < count) { return; }
    bgfx::TransientVertexBuffer buffer;
    bgfx::allocTransientVertexBuffer(&buffer, count, layout);
    std::memcpy(buffer.data, vertices.data(), vertices.size() * sizeof(vertices.front()));
    PickMatrix identity;
    bx::mtxIdentity(identity.data());
    bgfx::setTransform(identity.data());
    bgfx::setVertexBuffer(0, &buffer);
    bgfx::setUniform(colorUniform, settings.color.data());
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_DEPTH_TEST_LEQUAL
        | BGFX_STATE_BLEND_ALPHA | BGFX_STATE_MSAA);
    bgfx::submit(viewId, program);
}
} // namespace

void drawAnnotationTools(const UiState& state, AnnotationInteraction& interaction, bool disabled)
{
    ImGui::BeginDisabled(disabled || state.files.empty() || interaction.dragging);
    for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
        if (shape == AnnotationShape::rectangle) { ImGui::SameLine(); }
        const bool active = interaction.tool == shape;
        const bool clicked = drawRenderModeIconButton(
            shape == AnnotationShape::line ? "annotation_line" : "annotation_rectangle", "",
            shape == AnnotationShape::line ? "Surface line: drag on the model. Click again or press Escape to cancel."
                : "Surface rectangle: drag on the model. Click again or press Escape to cancel.",
            active ? RenderModeState::on : RenderModeState::off, false);
        const auto low = ImGui::GetItemRectMin(), high = ImGui::GetItemRectMax();
        const float inset = uiSize(7), stroke = uiSize(1.5f);
        const auto color = ImGui::GetColorU32(active ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        auto* draw = ImGui::GetWindowDrawList();
        if (shape == AnnotationShape::line) {
            draw->AddLine({low.x + inset, high.y - inset}, {high.x - inset, low.y + inset}, color, stroke);
            draw->AddCircleFilled({low.x + inset, high.y - inset}, uiSize(2), color);
            draw->AddCircleFilled({high.x - inset, low.y + inset}, uiSize(2), color);
        } else { draw->AddRect({low.x + inset, low.y + inset}, {high.x - inset, high.y - inset}, color, 0, 0, stroke); }
        if (clicked) {
            cancelAnnotationPointer(interaction);
            if (!active) { interaction.tool = shape; }
        }
    }
    ImGui::EndDisabled();
}
void drawAnnotationObjects(UiState& state)
{
    ImGui::SeparatorText("Annotations");
    SceneObjectId remove = 0;
    for (const auto& item : state.annotations) {
        ImGui::PushID(std::to_string(item.objectId).c_str());
        const float removeX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - renderModeButtonSize();
        auto style = item.settings;
        if (drawVisibilityButton("visible", style.visible, item.settings.name.c_str())) {
            style.visible = !style.visible;
            setAnnotationSettings(state, item.objectId, style);
        }
        ImGui::SameLine();
        const bool missing = !item.targetValid || !findSceneObject(state, item.targetId);
        const std::string label = item.settings.name + (missing ? " [needs reattachment]" : "") + "##annotation";
        const float nameWidth = std::max(1.0f, removeX - ImGui::GetCursorPosX() - ImGui::GetStyle().ItemSpacing.x);
        const bool selected = sceneObjectSelected(state, item.objectId);
        if (drawSceneItemButton(label.c_str(), nameWidth, selected)) { selectSceneObject(state, item.objectId, ImGui::GetIO().KeyCtrl); }
        if (selected) { drawSceneItemOutline(); }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s%s", item.settings.name.c_str(), missing ? " [needs reattachment]" : ""); }
        if (ImGui::BeginPopupContextItem("annotation_context")) {
            if (ImGui::MenuItem("Delete annotation")) { remove = item.objectId; }
            ImGui::EndPopup();
        }
        ImGui::SameLine(removeX, 0.0f);
        if (drawRemoveButton("remove", "Remove annotation from scene")) { remove = item.objectId; }
        ImGui::PopID();
    }
    if (remove) { deleteAnnotation(state, remove); }
}
void drawAnnotationInspector(UiState& state)
{
    const auto* item = selectedAnnotation(state);
    if (!item) { return; }
    const auto id = item->objectId;
    ImGui::PushID(std::to_string(id).c_str());
    auto settings = item->settings;
    ImGui::TextUnformatted(item->geometry.shape == AnnotationShape::line ? "Surface line" : "Surface rectangle");
    ImGui::TextWrapped("Target: %s", item->targetName.c_str());
    if (!item->targetValid || !findSceneObject(state, item->targetId)) {
        ImGui::TextWrapped("Needs reattachment: the source is missing or its geometry changed. Restore the original source or draw a new annotation.");
    }
    char name[512]{};
    std::memcpy(name, settings.name.data(), std::min(settings.name.size(), sizeof(name) - 1));
    bool changed = false;
    if (ImGui::InputText("Name", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue)) { settings.name = name; changed = true; }
    ImGui::TextUnformatted("Comments");
    changed |= ImGui::InputTextMultiline("##comments", &settings.note,
        ImVec2(-1, ImGui::GetTextLineHeight() * 5));
    changed |= drawVisibilityField("Visible", settings.visible);
    changed |= ImGui::Checkbox("Lock shape", &settings.locked);
    changed |= ImGui::ColorEdit4("Color", settings.color.data(), ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::SliderFloat("Line width", &settings.width, 1, 12, "%.1f px");
    if (changed) { setAnnotationSettings(state, id, std::move(settings)); }
    ImGui::SeparatorText("Vertex coordinates");
    ImGui::TextWrapped("Model coordinates, before scene transforms. Values use the model's units.");
    const auto vertices = annotationVertices(state, *item);
    if (vertices.empty()) { ImGui::TextWrapped("Coordinates unavailable: restore the original source model."); }
    else if (ImGui::BeginTable("vertices", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Vertex");
        ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y"); ImGui::TableSetupColumn("Z");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < vertices.size(); ++i) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (item->geometry.shape == AnnotationShape::line) { ImGui::TextUnformatted(i == 0 ? "Start" : "End"); }
            else { ImGui::Text("Corner %d", static_cast<int>(i + 1)); }
            for (const auto value : vertices[i]) { ImGui::TableNextColumn(); ImGui::Text("%.6g", static_cast<double>(value)); }
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Drag an edge of this selected annotation to move the whole outline. Drag an endpoint or corner to reshape. Escape cancels a drag.");
    ImGui::PopID();
}
void cancelAnnotationPointer(AnnotationInteraction& interaction) { interaction = {}; }

bool beginAnnotationPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView& view, PickPoint point)
{
    auto parts = scenePickParts(state);
    const auto* selected = selectedAnnotation(state);
    int handle = -1;
    if (!interaction.tool && selected && selected->targetValid && selected->settings.visible && !selected->settings.locked) {
        const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == selected->targetId; });
        if (target != parts.end()) {
            const auto positions = handles(*selected, *target, view);
            for (size_t i = 0; i < positions.size(); ++i) {
                if (std::hypot(point[0] - positions[i][0], point[1] - positions[i][1]) <= 9 * view.pixelScale) { handle = static_cast<int>(i); break; }
            }
        }
    }
    const bool moveWhole = !interaction.tool && handle < 0 && selected
        && annotationEdgeHit(*selected, parts, view, point);
    if (!interaction.tool && handle < 0 && !moveWhole) { return false; }
    interaction.error.clear();
    interaction.view = view; interaction.generation = state.sceneGeneration; interaction.revision = state.sceneEditRevision;
    interaction.pointerStart = interaction.pointerEnd = point;
    interaction.editing = 0; interaction.handle = handle;
    try {
        if ((handle >= 0 || moveWhole) && selected) {
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == selected->targetId; });
            interaction.currentProjection = annotationProjection(parts, view, selected->targetId);
            const auto hit = annotationSurfaceHit(interaction.currentProjection, annotationNdc(view, point));
            if (!hit || hit->objectId != selected->targetId) { return false; }
            interaction.projection = annotationEditProjection(*target, selected->geometry);
            interaction.preview = *selected; interaction.editing = selected->objectId;
            interaction.start = selected->geometry.start; interaction.end = selected->geometry.end;
            const auto local = annotationPosition(*target->mesh, target->indexOffset, hit->triangle, hit->bary);
            const auto clip = annotationTransform(selected->geometry.projector, {local[0], local[1], local[2], 1});
            if (clip[3] <= 0) { return false; }
            interaction.grabControl = {clip[0] / clip[3], clip[1] / clip[3]};
        } else {
            const auto allowed = comparisonObjectParts(state, state.selectedSceneObjects);
            const SceneObjectId initial = allowed.size() == 1 ? allowed.front() : 0;
            auto projection = annotationProjection(parts, view, initial);
            const auto target = pickAnnotationSurface(projection, annotationNdc(view, point));
            if (!target || (!allowed.empty() && std::find(allowed.begin(), allowed.end(), target) == allowed.end())) {
                throw std::runtime_error("Start on a visible surface of the selected model.");
            }
            if (initial != target) {
                const auto part = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == target; });
                projection.targetId = target;
                projection.definition.projector = annotationCompose(part->model, annotationCompose(view.view, view.projection));
                projection.definition.fingerprint = annotationFingerprint(*part->mesh, part->indexOffset, part->indexCount);
            }
            interaction.projection = std::move(projection);
            interaction.preview = {};
            interaction.preview.targetId = target; interaction.preview.targetValid = true;
            interaction.preview.geometry = interaction.projection.definition;
            interaction.preview.geometry.shape = *interaction.tool;
            interaction.start = interaction.end = annotationNdc(view, point);
        }
        interaction.preview.geometry.segments.clear();
        interaction.dragging = true;
    } catch (const std::exception& error) { interaction.error = error.what(); }
    return true;
}
void moveAnnotationPointer(const UiState& state, AnnotationInteraction& interaction, PickPoint point)
{
    if (!interaction.dragging) { return; }
    interaction.pointerEnd = point;
    interaction.preview.geometry.segments.clear();
    try {
        if (state.sceneGeneration != interaction.generation || state.sceneEditRevision != interaction.revision) { throw std::runtime_error("Scene changed while drawing. Start again."); }
        auto control = annotationNdc(interaction.view, point);
        if (interaction.editing) {
            const auto* selected = selectedAnnotation(state);
            if (!selected || selected->objectId != interaction.editing) { throw std::runtime_error("Annotation selection changed. Start again."); }
            const auto hit = annotationSurfaceHit(interaction.currentProjection, control);
            if (!hit || hit->objectId != interaction.preview.targetId) { throw std::runtime_error("Keep the annotation on its target surface."); }
            const auto parts = scenePickParts(state);
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == hit->objectId; });
            if (target == parts.end()) { throw std::runtime_error("Target is unavailable."); }
            const auto p = annotationPosition(*target->mesh, target->indexOffset, hit->triangle, hit->bary);
            const auto clip = annotationTransform(interaction.projection.definition.projector, {p[0], p[1], p[2], 1});
            if (clip[3] <= 0) { throw std::runtime_error("Keep the handle in its original drawing view."); }
            control = {clip[0] / clip[3], clip[1] / clip[3]};
            if (interaction.handle < 0) {
                for (size_t axis = 0; axis < 2; ++axis) {
                    const float delta = control[axis] - interaction.grabControl[axis];
                    interaction.start[axis] = interaction.projection.definition.start[axis] + delta;
                    interaction.end[axis] = interaction.projection.definition.end[axis] + delta;
                }
            } else if (interaction.preview.geometry.shape == AnnotationShape::line) {
                (interaction.handle == 0 ? interaction.start : interaction.end) = control;
            } else {
                (interaction.handle == 0 || interaction.handle == 3 ? interaction.start[0] : interaction.end[0]) = control[0];
                (interaction.handle == 0 || interaction.handle == 1 ? interaction.start[1] : interaction.end[1]) = control[1];
            }
        } else { interaction.end = control; }
        interaction.preview.geometry = projectAnnotation(interaction.projection, interaction.preview.geometry.shape, interaction.start, interaction.end);
        interaction.error.clear();
    } catch (const std::exception& error) { interaction.error = error.what(); }
}
void endAnnotationPointer(UiState& state, AnnotationInteraction& interaction, bool allowed)
{
    if (!interaction.dragging) { return; }
    interaction.dragging = false;
    if (!allowed || state.sceneGeneration != interaction.generation || state.sceneEditRevision != interaction.revision) {
        interaction.error = "Drawing canceled because the viewport or scene changed."; return;
    }
    if (!interaction.error.empty() || interaction.preview.geometry.segments.empty()) { return; }
    if (std::hypot(interaction.pointerEnd[0] - interaction.pointerStart[0], interaction.pointerEnd[1] - interaction.pointerStart[1]) < 4 * interaction.view.pixelScale) { return; }
    try {
        if (interaction.editing) {
            const auto* selected = selectedAnnotation(state);
            if (!selected || selected->objectId != interaction.editing) { throw std::runtime_error("Annotation selection changed. Start again."); }
            reshapeAnnotation(state, interaction.editing, interaction.preview.geometry);
        }
        else { createAnnotation(state, interaction.preview.targetId, interaction.preview.geometry); }
        cancelAnnotationPointer(interaction);
    } catch (const std::exception& error) { interaction.error = error.what(); }
}
void drawAnnotationOverlay(const UiState& state, AnnotationInteraction& interaction,
    const ScenePickView& view, float windowX, float pixelsToWindow, bool pointerAllowed)
{
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 low{windowX, 0}, high{windowX + static_cast<float>(view.width) * pixelsToWindow, static_cast<float>(view.height) * pixelsToWindow};
    draw->PushClipRect(low, high);
    const auto screenPoint = [&](PickPoint p) { return ImVec2{windowX + p[0] * pixelsToWindow, p[1] * pixelsToWindow}; };
    if (interaction.dragging && !interaction.error.empty()) {
        const auto a = screenPoint(interaction.pointerStart), b = screenPoint(interaction.pointerEnd);
        if (interaction.preview.geometry.shape == AnnotationShape::rectangle) { draw->AddRect(ImVec2{std::min(a.x,b.x),std::min(a.y,b.y)}, ImVec2{std::max(a.x,b.x),std::max(a.y,b.y)}, IM_COL32(255,90,90,255)); }
        else { draw->AddLine(a, b, IM_COL32(255,90,90,255), 2); }
    }
    if (!interaction.dragging) {
        const auto* item = selectedAnnotation(state);
        const auto selectedId = item ? item->objectId : 0;
        const bool changed = interaction.overlayView != view.view || interaction.overlayProjection != view.projection
            || interaction.overlayWidth != view.width || interaction.overlayHeight != view.height
            || interaction.overlaySelection != selectedId || interaction.overlayRevision != state.sceneEditRevision;
        if (changed) {
            interaction.overlayView = view.view; interaction.overlayProjection = view.projection;
            interaction.overlayWidth = view.width; interaction.overlayHeight = view.height;
            interaction.overlaySelection = selectedId; interaction.overlayRevision = state.sceneEditRevision;
            interaction.overlayReady = false; interaction.overlayHandles.clear();
            interaction.overlayEdgePoint.reset(); interaction.overlayEdgeHit = false;
        } else if (!interaction.overlayReady && item && item->targetValid && item->settings.visible && !item->settings.locked) {
            auto parts = scenePickParts(state);
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == item->targetId; });
            if (target != parts.end()) {
                const auto positions = handles(*item, *target, view);
                // Only show handles on visible target surfaces, not through other models.
                for (auto& part : parts) { part.edges = false; part.vertices = false; }
                for (auto p : positions) {
                    if (pickSceneObject(parts, view, p) != item->targetId) { continue; }
                    interaction.overlayHandles.push_back(p);
                }
            }
            interaction.overlayReady = true;
        }
        for (auto p : interaction.overlayHandles) {
            draw->AddCircleFilled(screenPoint(p), 5, IM_COL32(255,230,140,255));
            draw->AddCircle(screenPoint(p), 5, IM_COL32(35,35,35,255), 0, 1.5f);
        }
    }
    if (interaction.tool || interaction.dragging || !interaction.error.empty()) {
        const char* text = !interaction.error.empty() ? interaction.error.c_str() : "Drag on the surface. Escape cancels.";
        draw->AddText({low.x + 16, high.y - 42}, IM_COL32(255,230,170,255), text);
    }
    const auto mouse = ImGui::GetIO().MousePos;
    if (pointerAllowed && !ImGui::GetIO().WantCaptureMouse
        && mouse.x >= low.x && mouse.x < high.x && mouse.y >= low.y && mouse.y < high.y) {
        const PickPoint point{(mouse.x - windowX) / pixelsToWindow, mouse.y / pixelsToWindow};
        const bool overHandle = std::any_of(interaction.overlayHandles.begin(), interaction.overlayHandles.end(),
            [&](auto p) { return std::hypot(point[0] - p[0], point[1] - p[1]) <= 9 * view.pixelScale; });
        if (!interaction.tool && !interaction.dragging && interaction.overlayReady
            && interaction.overlayEdgePoint != point) {
            const auto* item = selectedAnnotation(state);
            interaction.overlayEdgeHit = item && annotationEdgeHit(*item, scenePickParts(state), view, point);
            interaction.overlayEdgePoint = point;
        }
        if ((interaction.dragging && interaction.editing)
            || (!interaction.tool && !interaction.dragging && (overHandle || interaction.overlayEdgeHit))) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (interaction.tool || interaction.dragging) {
            // ImGui has no crosshair cursor. Hide the platform cursor and draw
            // a contrast-outlined crosshair in window coordinates at every DPI.
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            for (const float thickness : {uiSize(3), uiSize(1)}) {
                const auto color = thickness == uiSize(3) ? IM_COL32(20,20,20,255) : IM_COL32(255,255,255,255);
                const float radius = uiSize(9);
                draw->AddLine({mouse.x - radius, mouse.y}, {mouse.x + radius, mouse.y}, color, thickness);
                draw->AddLine({mouse.x, mouse.y - radius}, {mouse.x, mouse.y + radius}, color, thickness);
            }
        }
    }
    draw->PopClipRect();
}
void submitSceneAnnotations(bgfx::ViewId viewId, const UiState& state, const ScenePickView& view,
    const bgfx::VertexLayout& layout, bgfx::ProgramHandle program, bgfx::UniformHandle colorUniform,
    const AnnotationInteraction* interaction)
{
    if (state.annotations.empty() && (!interaction || !interaction->dragging)) { return; }
    const auto parts = scenePickParts(state);
    const auto targetOf = [&](const UiAnnotation& item) -> const ScenePickPart* {
        const auto found = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == item.targetId; });
        return found == parts.end() ? nullptr : &*found;
    };
    for (const auto& item : state.annotations) {
        if (interaction && interaction->dragging && interaction->editing == item.objectId && interaction->error.empty()) { continue; }
        submitLines(viewId, annotationWorldLines(item, parts), view, targetOf(item), item.geometry, item.settings, layout, program, colorUniform);
    }
    if (interaction && interaction->dragging && interaction->error.empty()) {
        submitLines(viewId, annotationWorldLines(interaction->preview, parts), view, targetOf(interaction->preview), interaction->preview.geometry, interaction->preview.settings, layout, program, colorUniform);
    }
}
} // namespace woby
