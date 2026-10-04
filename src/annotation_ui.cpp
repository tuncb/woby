#include "annotation_ui.h"
#include "annotation_preparation.h"
#include "annotation_work.h"
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
std::vector<ScenePickPart> interactionParts(const UiState& state, AnnotationInteraction& interaction)
{
    std::vector<ScenePickPart> parts;
    if (interaction.queries) { resolveSceneParts(*interaction.queries, state, parts); }
    else { scenePickParts(state, parts); }
    return parts;
}
std::vector<PickPoint> handles(const UiAnnotation& item, std::span<const ScenePickPart> parts, const ScenePickView& view)
{
    std::vector<PickPoint> result;
    const auto transform = annotationCompose(view.view, view.projection);
    for (const auto& nearest : annotationControlWorldPositions(item, parts)) {
        const auto clip = annotationTransform(transform, {nearest[0], nearest[1], nearest[2], 1});
        const float minimumZ = view.homogeneousDepth ? -clip[3] : 0;
        if (clip[3] <= 0 || clip[2] < minimumZ || clip[2] > clip[3]) { result.push_back({-100000, -100000}); }
        else { result.push_back({(clip[0] / clip[3] + 1) * .5f * static_cast<float>(view.width),
            (1 - clip[1] / clip[3]) * .5f * static_cast<float>(view.height)}); }
    }
    return result;
}
void submitLines(woby::graphics::ViewId viewId, const AnnotationSettings& settings,
    std::span<const std::array<float, 3>> vertices, const woby::graphics::VertexLayout& layout,
    woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform, bool sampledPreview = false)
{
    if (vertices.empty()) { return; }
    const auto count = static_cast<uint32_t>(vertices.size());
    if (woby::graphics::getAvailTransientVertexBuffer(count, layout) < count) { return; }
    woby::graphics::TransientVertexBuffer buffer;
    woby::graphics::allocTransientVertexBuffer(&buffer, count, layout);
    std::memcpy(buffer.data, vertices.data(), vertices.size() * sizeof(vertices.front()));
    woby::graphics::setVertexBuffer(0, &buffer);
    woby::graphics::setUniform(colorUniform, settings.color.data());
    // Sampled chords can sink into curved surfaces. The temporary drag guide
    // stays visible; only the final, fully attached outline uses surface depth.
    woby::graphics::setState(WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | (sampledPreview ? 0 : WOBY_GPU_STATE_DEPTH_TEST_LEQUAL)
        | WOBY_GPU_STATE_BLEND_ALPHA | WOBY_GPU_STATE_MSAA);
    woby::graphics::submit(viewId, program);
}
} // namespace

void drawAnnotationTools(const UiState& state, AnnotationInteraction& interaction, bool disabled)
{
    const bool ready = annotationPreparationReady(state);
    ImGui::BeginDisabled(disabled || !ready || state.files.empty() || interaction.dragging);
    for (const auto shape : {AnnotationShape::line, AnnotationShape::rectangle}) {
        if (shape == AnnotationShape::rectangle) { ImGui::SameLine(); }
        const bool active = interaction.tool == shape;
        const bool clicked = drawRenderModeIconButton(
            shape == AnnotationShape::line ? "annotation_line" : "annotation_rectangle", "",
            !ready ? "Annotation tools are disabled while annotation data is being prepared."
                : shape == AnnotationShape::line ? "Surface line: drag on the model. Click again or press Escape to cancel."
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
void drawAnnotationObjects(UiState& state, AnnotationNameEdit& edit)
{
    if (edit.generation != state.sceneGeneration
        || (edit.objectId != invalidSceneObjectId && !findAnnotation(state, edit.objectId))) {
        edit = {};
        edit.generation = state.sceneGeneration;
    }
    const auto beginRename = [&](SceneObjectId id) {
        if (const auto* item = findAnnotation(state, id)) {
            edit.objectId = id;
            edit.text = item->settings.name;
            edit.focus = true;
            selectSceneObject(state, id);
        }
    };
    const bool canStartRename = ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput
        && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
    if (canStartRename && edit.objectId == invalidSceneObjectId && state.selectedSceneObjects.size() == 1
        && ImGui::IsKeyPressed(ImGuiKey_F2, false)) {
        beginRename(state.selectedSceneObjects.front());
    }
    bool open = false;
    if (ImGui::BeginTable("annotations_header", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("visibility", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        const auto visible = static_cast<size_t>(std::count_if(state.annotations.begin(), state.annotations.end(),
            [](const auto& item) { return item.settings.visible; }));
        if (drawTriStateVisibilityButton("annotations_visible", "Annotations", visible, state.annotations.size(), "annotations")) {
            setAllAnnotationsVisible(state, visible != state.annotations.size());
        }
        ImGui::TableNextColumn();
        open = ImGui::CollapsingHeader("Annotations", state.annotations.empty() ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::EndTable();
    }
    if (!open) { return; }
    SceneObjectId remove = 0;
    SceneObjectId duplicate = 0;
    for (const auto& item : state.annotations) {
        const auto id = item.objectId;
        ImGui::PushID(std::to_string(id).c_str());
        auto style = item.settings;
        if (drawVisibilityButton("visible", style.visible, item.settings.name.c_str())) {
            style.visible = !style.visible;
            setAnnotationSettings(state, id, style);
        }
        ImGui::SameLine();
        const bool missing = !item.targetValid || !findSceneObject(state, item.targetId);
        const std::string label = item.settings.name + (item.targetPending ? " [preparing]" : missing ? " [needs reattachment]" : "") + "##annotation";
        const float nameWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        if (edit.objectId == id) {
            const bool focusing = edit.focus;
            if (focusing) { ImGui::SetKeyboardFocusHere(); edit.focus = false; }
            ImGui::SetNextItemWidth(nameWidth);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x,
                std::max(0.0f, (renderModeButtonSize() - ImGui::GetTextLineHeight()) * 0.5f)));
            const bool entered = ImGui::InputText("##annotation_name_edit", &edit.text,
                ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopStyleVar();
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { edit.objectId = invalidSceneObjectId; }
            else if (entered || (!focusing && ImGui::IsItemDeactivated())) {
                renameAnnotation(state, id, edit.text);
                edit.objectId = invalidSceneObjectId;
            }
        } else {
            const bool selected = sceneObjectSelected(state, id);
            if (drawSceneItemButton(label.c_str(), nameWidth, selected)) {
                selectSceneObject(state, id, ImGui::GetIO().KeyCtrl);
            }
            if (selected) { drawSceneItemOutline(); }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s%s\nSelect this annotation. Double-click to rename.",
                    item.settings.name.c_str(), missing ? " [needs reattachment]" : "");
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                    && !ImGui::GetIO().KeyCtrl && !ImGui::GetIO().KeyShift) { beginRename(id); }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { selectSceneObject(state, id, false, true); }
            if (ImGui::BeginPopupContextItem("annotation_context")) {
                if (ImGui::MenuItem("Rename", "F2")) { beginRename(id); }
                if (ImGui::MenuItem("Duplicate")) { duplicate = id; }
                if (ImGui::MenuItem("Delete annotation")) { remove = id; }
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    if (remove) { deleteAnnotation(state, remove); }
    else if (duplicate) { duplicateAnnotation(state, duplicate); }
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
    if (item->targetPending) { ImGui::TextWrapped("Preparing annotation data..."); }
    else if (!item->targetValid || !findSceneObject(state, item->targetId)) {
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
    ImGui::TextWrapped(item->targetIds.empty() ? "Model coordinates, before scene transforms. Values use the model's units."
        : "World coordinates. Each vertex follows its attached source part.");
    const auto vertices = annotationOriginalVertices(state, *item);
    if (item->targetPending) { ImGui::TextWrapped("Preparing annotation data..."); }
    else if (vertices.empty()) { ImGui::TextWrapped("Coordinates unavailable: restore the original source model."); }
    else if (ImGui::BeginTable("vertices", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Vertex");
        ImGui::TableSetupColumn("X"); ImGui::TableSetupColumn("Y"); ImGui::TableSetupColumn("Z");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < vertices.size(); ++i) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (item->geometry.shape == AnnotationShape::line) { ImGui::TextUnformatted(i == 0 ? "Start" : "End"); }
            else { ImGui::Text("Corner %d", static_cast<int>(i + 1)); }
            for (const auto value : vertices[i]) { ImGui::TableNextColumn(); ImGui::Text("%.12g", value); }
        }
        ImGui::EndTable();
    }
    ImGui::TextWrapped("Drag an edge of this selected annotation to move the whole outline. Drag an endpoint or corner to reshape. Escape cancels a drag.");
    ImGui::PopID();
}
namespace {
struct PointerTargets {
    std::vector<SceneObjectId> allowed;
    std::vector<std::vector<SceneObjectId>> selected, files;
};
std::vector<SceneObjectId> pointerGroupTargets(const PointerTargets& targets, SceneObjectId target)
{
    std::vector<SceneObjectId> result;
    for (const auto* scopes : {&targets.selected, &targets.files}) {
        for (const auto& group : *scopes) {
            if (std::find(group.begin(), group.end(), target) != group.end()
                && (result.empty() || group.size() < result.size())) { result = group; }
        }
        if (!result.empty()) { break; }
    }
    std::erase(result, target); result.insert(result.begin(), target);
    return result;
}
bool beginPointerProjection(const UiState& state, AnnotationInteraction& interaction, const ScenePickView& view,
    PickPoint point, std::span<const ScenePickPart> parts, std::stop_token stop, const PointerTargets* scopes = nullptr)
{
    const auto* selected = selectedAnnotation(state);
    int handle = -1;
    if (!interaction.tool && selected && selected->targetValid && selected->settings.visible && !selected->settings.locked) {
        const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == selected->targetId; });
        if (target != parts.end()) {
            const auto positions = handles(*selected, parts, view);
            for (size_t i = 0; i < positions.size(); ++i) {
                if (std::hypot(point[0] - positions[i][0], point[1] - positions[i][1]) <= 9 * view.pixelScale) { handle = static_cast<int>(i); break; }
            }
        }
    }
    const bool moveWhole = !interaction.tool && handle < 0 && selected
        && annotationEdgeHit(*selected, parts, view, point);
    if (!interaction.tool && handle < 0 && !moveWhole) { return false; }
    interaction.error.clear();
    interaction.view = view;
    interaction.identity = annotationWorkIdentity(state);
    interaction.pointerStart = interaction.pointerEnd = point;
    interaction.editing = 0; interaction.handle = handle;
    try {
        if ((handle >= 0 || moveWhole) && selected) {
            interaction.currentProjection = annotationProjection(parts, view, selected->targetId, selected->targetIds,
                annotationRegion(annotationNdc(view, point), annotationNdc(view, point)), stop);
            const auto hit = annotationSurfaceHit(interaction.currentProjection, annotationNdc(view, point));
            if (!hit || !annotationHasTarget(*selected, hit->objectId)) { return false; }
            interaction.projection = annotationEditProjection(parts, *selected, annotationRegion(selected->geometry.start, selected->geometry.end), stop);
            interaction.preview = *selected; interaction.editing = selected->objectId;
            interaction.start = selected->geometry.start; interaction.end = selected->geometry.end;
            const auto source = annotationSourceIndex(interaction.projection, hit->objectId);
            const auto* target = annotationSourcePart(*selected, parts, source);
            const auto local = annotationPosition(*target->mesh, target->indexOffset, hit->triangle, hit->bary);
            const auto clip = annotationTransform(annotationSourceProjector(selected->geometry, source), {local[0], local[1], local[2], 1});
            if (clip[3] <= 0) { return false; }
            interaction.grabControl = {static_cast<float>(clip[0] / clip[3]), static_cast<float>(clip[1] / clip[3])};
        } else {
            const auto allowed = scopes ? scopes->allowed : comparisonObjectParts(state, state.selectedSceneObjects);
            const SceneObjectId initial = allowed.size() == 1 ? allowed.front() : 0;
            auto projection = annotationGestureProjection(parts, view, initial, annotationNdc(view, point), stop);
            const auto target = pickAnnotationSurface(projection, annotationNdc(view, point));
            if (!target || (!allowed.empty() && std::find(allowed.begin(), allowed.end(), target) == allowed.end())) {
                throw std::runtime_error("Start on a visible surface of the selected model.");
            }
            const auto targets = scopes ? pointerGroupTargets(*scopes, target) : annotationGroupTargets(state, target);
            if (targets.size() > 1) { setAnnotationProjectionTargets(projection, parts, view, target, targets); }
            else if (initial != target) { setAnnotationProjectionTarget(projection, parts, view, target); }
            interaction.projection = std::move(projection);
            interaction.preview = {};
            interaction.preview.targetId = target; interaction.preview.targetValid = true;
            interaction.preview.targetIds = interaction.projection.targetIds;
            interaction.preview.geometry = interaction.projection.definition;
            interaction.preview.geometry.shape = *interaction.tool;
            interaction.start = interaction.end = annotationNdc(view, point);
        }
        interaction.preview.geometry.segments.clear();
        interaction.sampledPreview = interaction.projection.triangles.size() > 50000
            || std::any_of(parts.begin(), parts.end(), [](const auto& part) { return part.indexCount >= 50000 * 3; });
        interaction.dragging = true;
    } catch (const std::exception& error) { interaction.error = error.what(); }
    return true;
}
void movePointerProjection(const UiState& state, AnnotationInteraction& interaction, PickPoint point, std::span<const ScenePickPart> parts)
{
    if (!interaction.dragging) { return; }
    interaction.pointerEnd = point;
    interaction.preview.geometry.segments.clear();
    try {
        if (!annotationWorkCurrent(interaction.identity, state, true)) { throw std::runtime_error("Scene changed while drawing. Start again."); }
        auto control = annotationNdc(interaction.view, point);
        if (interaction.editing) {
            const auto* selected = selectedAnnotation(state);
            if (!selected || selected->objectId != interaction.editing) { throw std::runtime_error("Annotation selection changed. Start again."); }
            expandAnnotationGestureProjection(interaction.currentProjection, parts, interaction.view, control, control);
            const auto hit = annotationSurfaceHit(interaction.currentProjection, control);
            if (!hit || !annotationHasTarget(interaction.preview, hit->objectId)) { throw std::runtime_error("Keep the annotation on its target surface."); }
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == hit->objectId; });
            if (target == parts.end()) { throw std::runtime_error("Target is unavailable."); }
            const auto p = annotationPosition(*target->mesh, target->indexOffset, hit->triangle, hit->bary);
            const auto clip = annotationTransform(annotationSourceProjector(interaction.projection.definition,
                annotationSourceIndex(interaction.projection, hit->objectId)), {p[0], p[1], p[2], 1});
            if (clip[3] <= 0) { throw std::runtime_error("Keep the handle in its original drawing view."); }
            control = {static_cast<float>(clip[0] / clip[3]), static_cast<float>(clip[1] / clip[3])};
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
        } else {
            interaction.end = control;
        }
        expandAnnotationGestureProjection(interaction.projection, parts, interaction.view,
            interaction.start, interaction.end);
        interaction.sampledPreview = interaction.sampledPreview || interaction.projection.triangles.size() > 50000;
        interaction.preview.geometry = interaction.sampledPreview
            ? previewAnnotation(interaction.projection, interaction.preview.geometry.shape, interaction.start, interaction.end)
            : projectAnnotation(interaction.projection, interaction.preview.geometry.shape, interaction.start, interaction.end);
        interaction.error.clear();
    } catch (const std::exception& error) { interaction.error = error.what(); }
}
} // namespace
struct AnnotationPointerInput {
    UiState metadata;
    PointerTargets targets;
    AnnotationPartsSnapshot sources;
    AnnotationWorkIdentity identity;
    ScenePickView view;
    PickPoint origin{};
    std::optional<AnnotationShape> tool;
};
struct AnnotationPointerResult {
    UiAnnotation preview;
    std::string error;
    SceneObjectId editing = 0;
    bool sampled = false;
};
struct AnnotationPointerRuntime {
    std::shared_ptr<const AnnotationPointerInput> input;
    std::shared_ptr<AnnotationWork> work;
    std::shared_ptr<AnnotationPointerResult> result;
    bool released = false;
    bool previewPublished = false;
};
namespace {
UiState pointerMetadata(const UiState& state)
{
    UiState result;
    result.sceneGeneration = state.sceneGeneration; result.sceneEditRevision = state.sceneEditRevision;
    result.selectedSceneObjects = state.selectedSceneObjects;
    if (const auto* selected = selectedAnnotation(state)) { result.annotations.push_back(*selected); }
    return result;
}
void queuePointerProjection(AnnotationInteraction& interaction)
{
    auto& pending = *interaction.pending;
    pending.work = std::make_shared<AnnotationWork>();
    pending.result = std::make_shared<AnnotationPointerResult>();
    pending.previewPublished = false;
    pending.work->compute = [input = pending.input, result = pending.result, end = interaction.pointerEnd,
        released = pending.released](std::stop_token stop) {
        AnnotationInteraction calculated;
        calculated.tool = input->tool;
        (void)beginPointerProjection(input->metadata, calculated, input->view,
            input->origin, input->sources.parts, stop, &input->targets);
        if (calculated.dragging && end != input->origin) {
            movePointerProjection(input->metadata, calculated, end, input->sources.parts);
            if (released && calculated.error.empty() && !calculated.preview.geometry.segments.empty()) {
                calculated.preview.geometry = projectAnnotation(calculated.projection,
                    calculated.preview.geometry.shape, calculated.start, calculated.end);
            }
        }
        result->preview = std::move(calculated.preview); result->error = std::move(calculated.error);
        result->editing = calculated.editing;
        result->sampled = calculated.sampledPreview && !released;
        // Both projections and their temporary indices are destroyed here.
    };
    submitAnnotationWork(*interaction.executor, pending.work);
}
}
void cancelAnnotationPointer(AnnotationInteraction& interaction)
{
    if (interaction.pending) { cancelAnnotationWork(interaction.pending->work); }
    auto* executor = interaction.executor;
    auto* queries = interaction.queries;
    interaction = {}; interaction.executor = executor; interaction.queries = queries;
}
bool beginAnnotationPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView& view, PickPoint point)
{
    if (!annotationPreparationReady(state)) { return false; }
    if (!interaction.executor) {
        return beginPointerProjection(state, interaction, view, point, interactionParts(state, interaction), {});
    }
    // Cheap outline/handle test avoids consuming unrelated camera/selection clicks.
    const auto parts = interactionParts(state, interaction);
    if (!interaction.tool) {
        const auto* selected = selectedAnnotation(state);
        if (!selected || !selected->targetValid || !selected->settings.visible || selected->settings.locked) { return false; }
        const auto positions = handles(*selected, parts, view);
        bool near = std::any_of(positions.begin(), positions.end(), [&](const auto& p) {
            return std::hypot(point[0] - p[0], point[1] - p[1]) <= 9 * view.pixelScale;
        });
        if (!near) {
            const auto lines = annotationWorldLines(*selected, parts);
            ScenePickPart outline; outline.objectId = selected->objectId; outline.diagnosticEdges = lines;
            bx::mtxIdentity(outline.model.data());
            near = pickSceneObject(std::span<const ScenePickPart>(&outline, 1), view, point) == selected->objectId;
        }
        if (!near) { return false; }
    }
    try {
        if (interaction.pending) { cancelAnnotationWork(interaction.pending->work); }
        auto input = std::make_shared<AnnotationPointerInput>();
        input->metadata = pointerMetadata(state); input->sources = snapshotAnnotationParts(parts);
        input->targets.allowed = comparisonObjectParts(state, state.selectedSceneObjects);
        for (const auto id : state.selectedSceneObjects) {
            const auto object = findSceneObject(state, id);
            if (object && (object->kind == SceneObjectKind::folder || object->kind == SceneObjectKind::file)) {
                input->targets.selected.push_back(comparisonObjectParts(state, {id}));
            }
        }
        for (const auto& file : state.files) { input->targets.files.push_back(comparisonObjectParts(state, {file.objectId})); }
        input->identity = annotationWorkIdentity(state); input->view = view; input->origin = point; input->tool = interaction.tool;
        interaction.pending = std::make_shared<AnnotationPointerRuntime>();
        interaction.pending->input = std::move(input);
        interaction.view = view; interaction.pointerStart = interaction.pointerEnd = point;
        interaction.identity = annotationWorkIdentity(state);
        interaction.dragging = true; interaction.error.clear(); interaction.preview = {};
        if (const auto* item = selectedAnnotation(state); !interaction.tool && item) { interaction.editing = item->objectId; }
        else { interaction.editing = 0; }
        queuePointerProjection(interaction);
    } catch (const std::exception& error) { interaction.error = error.what(); interaction.dragging = false; }
    return true;
}
void moveAnnotationPointer(const UiState& state, AnnotationInteraction& interaction, PickPoint point)
{
    if (!interaction.pending) { movePointerProjection(state, interaction, point, interactionParts(state, interaction)); return; }
    if (interaction.pending->released || point == interaction.pointerEnd) { return; }
    interaction.pointerEnd = point;
    queuePointerProjection(interaction); // Replaces a queued preview, cooperatively cancels the active one.
}
void updateAnnotationPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView* view)
{
    if (!interaction.pending) { return; }
    auto& pending = *interaction.pending;
    const auto& input = *pending.input;
    if (!annotationWorkCurrent(input.identity, state, true)
        || (view && (view->view != input.view.view || view->projection != input.view.projection
            || view->width != input.view.width || view->height != input.view.height))) {
        cancelAnnotationPointer(interaction);
        interaction.error = "Drawing canceled because the viewport or scene changed.";
        return;
    }
    if (!pending.work->done.load(std::memory_order_acquire)) { return; }
    if (pending.work->cancel.stop_requested()) { return; }
    if (pending.previewPublished && !pending.released) { return; }
    pending.previewPublished = true;
    const auto& result = *pending.result;
    interaction.preview = result.preview; interaction.editing = result.editing;
    interaction.sampledPreview = result.sampled;
    interaction.error = pending.work->error.empty() ? result.error : pending.work->error;
    if (!pending.released) { return; }
    if (interaction.error.empty() && !result.preview.geometry.segments.empty()
        && std::hypot(interaction.pointerEnd[0] - interaction.pointerStart[0],
            interaction.pointerEnd[1] - interaction.pointerStart[1]) >= 4 * interaction.view.pixelScale) {
        try {
            if (result.editing) { reshapeAnnotation(state, result.editing, result.preview.geometry); }
            else { createAnnotation(state, result.preview.targetId, result.preview.geometry, result.preview.targetIds); }
        } catch (const std::exception& error) { interaction.error = error.what(); }
    }
    auto error = std::move(interaction.error);
    cancelAnnotationPointer(interaction); interaction.error = std::move(error);
}
void endAnnotationPointer(UiState& state, AnnotationInteraction& interaction, bool allowed)
{
    if (interaction.pending) {
        if (!allowed || !annotationWorkCurrent(interaction.pending->input->identity, state, true)) {
            cancelAnnotationPointer(interaction); return;
        }
        interaction.pending->released = true;
        queuePointerProjection(interaction);
        return;
    }
    if (!interaction.dragging) { return; }
    interaction.dragging = false;
    if (!allowed || !annotationWorkCurrent(interaction.identity, state, true)) {
        interaction.error = "Drawing canceled because the viewport or scene changed."; return;
    }
    if (!interaction.error.empty() || interaction.preview.geometry.segments.empty()) { return; }
    if (std::hypot(interaction.pointerEnd[0] - interaction.pointerStart[0], interaction.pointerEnd[1] - interaction.pointerStart[1]) < 4 * interaction.view.pixelScale) { return; }
    try {
        if (interaction.sampledPreview) {
            interaction.preview.geometry = projectAnnotation(interaction.projection,
                interaction.preview.geometry.shape, interaction.start, interaction.end);
        }
        if (interaction.editing) {
            const auto* selected = selectedAnnotation(state);
            if (!selected || selected->objectId != interaction.editing) { throw std::runtime_error("Annotation selection changed. Start again."); }
            reshapeAnnotation(state, interaction.editing, interaction.preview.geometry);
        }
        else { createAnnotation(state, interaction.preview.targetId, interaction.preview.geometry, interaction.preview.targetIds); }
        cancelAnnotationPointer(interaction);
    } catch (const std::exception& error) { interaction.error = error.what(); }
}
float drawAnnotationOverlay(const UiState& state, AnnotationInteraction& interaction,
    const ScenePickView& view, float windowX, float pixelsToWindow, bool pointerAllowed, float windowY)
{
    pointerAllowed = pointerAllowed && annotationPreparationReady(state);
    auto* draw = ImGui::GetForegroundDrawList();
    const ImVec2 low{windowX, windowY}, high{windowX + static_cast<float>(view.width) * pixelsToWindow, windowY + static_cast<float>(view.height) * pixelsToWindow};
    draw->PushClipRect(low, high);
    const auto screenPoint = [&](PickPoint p) { return ImVec2{windowX + p[0] * pixelsToWindow, windowY + p[1] * pixelsToWindow}; };
    if (interaction.dragging && !interaction.error.empty()) {
        const auto a = screenPoint(interaction.pointerStart), b = screenPoint(interaction.pointerEnd);
        if (interaction.preview.geometry.shape == AnnotationShape::rectangle) { draw->AddRect(ImVec2{std::min(a.x,b.x),std::min(a.y,b.y)}, ImVec2{std::max(a.x,b.x),std::max(a.y,b.y)}, IM_COL32(255,90,90,255)); }
        else { draw->AddLine(a, b, IM_COL32(255,90,90,255), 2); }
    }
    if (!interaction.dragging) {
        const auto* item = selectedAnnotation(state);
        const auto selectedId = item ? item->objectId : 0;
        const bool changed = interaction.overlayView != view.view || interaction.overlayProjection != view.projection
            || interaction.overlayWidth != view.width || interaction.overlayHeight != view.height || interaction.overlayPixelScale != view.pixelScale
            || interaction.overlaySelection != selectedId || interaction.overlayStamp != sceneQueryStamp(state) || interaction.overlayAnnotations != state.revisions.annotations;
        if (changed) {
            interaction.overlayView = view.view; interaction.overlayProjection = view.projection;
            interaction.overlayWidth = view.width; interaction.overlayHeight = view.height; interaction.overlayPixelScale = view.pixelScale;
            interaction.overlaySelection = selectedId; interaction.overlayStamp = sceneQueryStamp(state);
            interaction.overlayAnnotations = state.revisions.annotations;
            interaction.overlayReady = false; interaction.overlayHandles.clear();
            interaction.overlayEdgePoint.reset(); interaction.overlayEdgeHit = false;
        } else if (pointerAllowed && !interaction.overlayReady && item && item->targetValid && item->settings.visible && !item->settings.locked) {
            // A drag can have frames with no mouse motion. Do not mistake one
            // of those frames for settled navigation and ray-pick the mesh.
            auto parts = interactionParts(state, interaction);
            const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& p) { return p.objectId == item->targetId; });
            if (target != parts.end()) {
                const auto positions = handles(*item, parts, view);
                // Only show handles on visible target surfaces, not through other models.
                for (auto& part : parts) { part.edges = false; part.vertices = false; }
                for (auto p : positions) {
                    if (!annotationHasTarget(*item, pickSceneObject(parts, view, p))) { continue; }
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
    float messageBottom = 0;
    if (interaction.tool || interaction.dragging || !interaction.error.empty()) {
        const char* text = !interaction.error.empty() ? interaction.error.c_str()
            : interaction.pending && interaction.pending->released ? "Attaching annotation... Escape cancels."
            : interaction.dragging && interaction.sampledPreview ? "Release to attach the outline to the surface. Escape cancels."
            : "Drag on the surface. Escape cancels.";
        const float margin = uiSize(12), paddingX = uiSize(12), paddingY = uiSize(8);
        const float wrapWidth = std::max(1.0f, high.x - low.x - 2 * (margin + paddingX));
        const auto size = ImGui::CalcTextSize(text, nullptr, false, wrapWidth);
        const ImVec2 messageLow{low.x + (high.x - low.x - size.x) * .5f - paddingX, windowY + margin};
        const ImVec2 messageHigh{messageLow.x + size.x + paddingX * 2, messageLow.y + size.y + paddingY * 2};
        draw->AddRectFilled(messageLow, messageHigh, IM_COL32(24,28,34,235), uiSize(4));
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            {messageLow.x + paddingX, messageLow.y + paddingY}, IM_COL32(255,230,170,255), text, nullptr, wrapWidth);
        messageBottom = messageHigh.y;
    }
    const auto mouse = ImGui::GetIO().MousePos;
    if (pointerAllowed && !ImGui::GetIO().WantCaptureMouse
        && mouse.x >= low.x && mouse.x < high.x && mouse.y >= low.y && mouse.y < high.y) {
        const PickPoint point{(mouse.x - windowX) / pixelsToWindow, (mouse.y - windowY) / pixelsToWindow};
        const bool overHandle = std::any_of(interaction.overlayHandles.begin(), interaction.overlayHandles.end(),
            [&](auto p) { return std::hypot(point[0] - p[0], point[1] - p[1]) <= 9 * view.pixelScale; });
        if (!interaction.tool && !interaction.dragging && interaction.overlayReady
            && interaction.overlayEdgePoint != point) {
            const auto* item = selectedAnnotation(state);
            if (interaction.queries) { updateSceneAnnotationQueries(*interaction.queries, state); }
            const auto* lines = item && interaction.queries ? &interaction.queries->annotations.objects.at(item->objectId).lines : nullptr;
            interaction.overlayEdgeHit = item && annotationEdgeHit(*item, interactionParts(state, interaction), view, point, lines);
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
    return messageBottom;
}
void prepareSceneAnnotations(const UiState& state, const ScenePickView& view,
    SceneRenderScratch& scratch, const AnnotationInteraction* interaction)
{
    updateSceneAnnotationQueries(scratch.queries, state);
    if (state.annotations.empty() && (!interaction || !interaction->dragging)) { return; }
    auto& parts = scratch.parts;
    resolveSceneParts(scratch.queries, state, parts);
    for (const auto& item : state.annotations) {
        updateAnnotationProjection(scratch.queries.annotations.objects.at(item.objectId), item, parts, view);
    }
    auto& preview = scratch.annotationPreview;
    preview.projected.clear();
    if (interaction && interaction->dragging && interaction->error.empty()) {
        preview.lines = annotationWorldLines(interaction->preview, parts);
        ++preview.builds;
        updateAnnotationProjection(preview, interaction->preview, parts, view);
    }
}

void submitSceneAnnotations(woby::graphics::ViewId viewId, const UiState& state,
    const woby::graphics::VertexLayout& layout, woby::graphics::ProgramHandle program, woby::graphics::UniformHandle colorUniform,
    const SceneRenderScratch& scratch, const AnnotationInteraction* interaction)
{
    for (const auto& item : state.annotations) {
        if (interaction && interaction->dragging && interaction->editing == item.objectId && interaction->error.empty()
            && !interaction->preview.geometry.segments.empty()) { continue; }
        const auto& query = scratch.queries.annotations.objects.at(item.objectId);
        submitLines(viewId, item.settings, query.projected, layout, program, colorUniform);
    }
    if (interaction && interaction->dragging && interaction->error.empty()) {
        submitLines(viewId, interaction->preview.settings, scratch.annotationPreview.projected,
            layout, program, colorUniform, interaction->sampledPreview);
    }
}
} // namespace woby
