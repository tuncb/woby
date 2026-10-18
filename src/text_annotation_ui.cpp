#include "text_annotation_ui.h"
#include "annotation_ui.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cmath>

namespace woby {
namespace {
constexpr float layoutSize = 32;
bool hitText(const UiAnnotation& item, const ScenePickView& view, PickPoint point)
{
    if (!item.text || !item.settings.visible || item.settings.color[3] <= 0) { return false; }
    const auto transform = textAnnotationTransform(*item.text, view);
    const auto size = ImGui::GetFont()->CalcTextSizeA(layoutSize, FLT_MAX, 0, item.text->text.c_str());
    const std::array<PickPoint, 4> local{{{0, 0}, {size.x / layoutSize, 0},
        {size.x / layoutSize, size.y / layoutSize}, {0, size.y / layoutSize}}};
    std::array<PickPoint, 4> corners;
    for (size_t i = 0; i < corners.size(); ++i) {
        const auto p = projectTextAnnotationPoint(transform, view, local[i]);
        if (!p) { return false; }
        corners[i] = *p;
    }
    if (std::hypot(point[0] - corners[0][0], point[1] - corners[0][1]) <= 8 * view.pixelScale) { return true; }
    float minimum = FLT_MAX, maximum = -FLT_MAX;
    for (size_t i = 0; i < corners.size(); ++i) {
        const auto a = corners[i], b = corners[(i + 1) % corners.size()];
        const float cross = (b[0] - a[0]) * (point[1] - a[1]) - (b[1] - a[1]) * (point[0] - a[0]);
        minimum = std::min(minimum, cross); maximum = std::max(maximum, cross);
    }
    return (minimum >= 0 || maximum <= 0) && maximum != minimum;
}
const TextAnnotationLayout& textLayout(TextAnnotationDrawCache& cache, SceneObjectId id, const std::string& text)
{
    auto& entry = cache.layouts[id];
    auto* atlas = ImGui::GetIO().Fonts;
    if (!entry.ready || entry.text != text || entry.font != ImGui::GetFont()
        || entry.atlasId != atlas->TexData->UniqueID || entry.atlasWidth != atlas->TexData->Width || entry.atlasHeight != atlas->TexData->Height) {
        auto layout = std::make_shared<ImDrawList>(ImGui::GetDrawListSharedData());
        layout->_ResetForNewFrame();
        layout->PushClipRect({-FLT_MAX, -FLT_MAX}, {FLT_MAX, FLT_MAX});
        layout->PushTexture(atlas->TexRef);
        layout->AddText(ImGui::GetFont(), layoutSize, {0, 0}, IM_COL32_WHITE, text.c_str());
        layout->PopTexture(); layout->PopClipRect();
        entry.glyphs.clear();
        for (const auto& command : layout->CmdBuffer) {
            for (unsigned int i = 0; i + 5 < command.ElemCount; i += 6) {
                TextAnnotationGlyph glyph; glyph.texture = command.TexRef;
                constexpr std::array<unsigned int, 4> indices{0, 1, 2, 5};
                for (size_t k = 0; k < indices.size(); ++k) {
                    const auto& vertex = layout->VtxBuffer[static_cast<int>(command.VtxOffset + layout->IdxBuffer[static_cast<int>(command.IdxOffset + i + indices[k])])];
                    glyph.positions[k] = {vertex.pos.x / layoutSize, vertex.pos.y / layoutSize};
                    glyph.uv[k] = {vertex.uv.x, vertex.uv.y};
                }
                entry.glyphs.push_back(glyph);
            }
        }
        entry.ready = true; entry.text = text; entry.font = ImGui::GetFont();
        entry.atlasId = atlas->TexData->UniqueID; entry.atlasWidth = atlas->TexData->Width; entry.atlasHeight = atlas->TexData->Height;
        ++cache.builds;
    }
    return entry;
}
void drawText(ImDrawList& draw, const UiAnnotation& item, const ScenePickView& view, PickPoint origin, float scale, TextAnnotationDrawCache& cache)
{
    if (!item.text || !item.settings.visible || item.settings.color[3] <= 0 || item.text->text.empty()) { return; }
    // Use the application font atlas for both the viewport and exported images.
    // Layout in a small local plane, then project each glyph into the scene.
    const auto& layout = textLayout(cache, item.objectId, item.text->text);
    const auto transform = textAnnotationTransform(*item.text, view);
    const auto& c = item.settings.color;
    const auto color = ImGui::ColorConvertFloat4ToU32({c[0], c[1], c[2], c[3]});
    for (const auto& glyph : layout.glyphs) {
        std::array<ImVec2, 4> positions, uv;
        bool visible = true;
        for (size_t k = 0; k < positions.size(); ++k) {
            const auto p = projectTextAnnotationPoint(transform, view, glyph.positions[k]);
            if (!p) { visible = false; break; }
            positions[k] = {origin[0] + (*p)[0] * scale, origin[1] + (*p)[1] * scale};
            uv[k] = {glyph.uv[k][0], glyph.uv[k][1]};
        }
        if (visible) { draw.AddImageQuad(glyph.texture, positions[0], positions[1], positions[2], positions[3], uv[0], uv[1], uv[2], uv[3], color); }
    }
}
} // namespace
void drawTextAnnotations(ImDrawList& draw, const UiState& state, const ScenePickView& view,
    PickPoint origin, float pixelsToWindow, TextAnnotationDrawCache& cache, const AnnotationInteraction* interaction)
{
    if (cache.context != ImGui::GetCurrentContext()) { cache = {}; cache.context = ImGui::GetCurrentContext(); }
    std::erase_if(cache.layouts, [&](const auto& entry) { return entry.first != 0 && !findAnnotation(state, entry.first); });
    draw.PushClipRect({origin[0], origin[1]},
        {origin[0] + static_cast<float>(view.width) * pixelsToWindow, origin[1] + static_cast<float>(view.height) * pixelsToWindow}, true);
    for (const auto& item : state.annotations) {
        if (interaction && interaction->dragging && interaction->preview.text && interaction->editing == item.objectId) { continue; }
        drawText(draw, item, view, origin, pixelsToWindow, cache);
    }
    if (interaction && interaction->dragging) { drawText(draw, interaction->preview, view, origin, pixelsToWindow, cache); }
    draw.PopClipRect();
}
void drawTextAnnotationInspector(UiState& state)
{
    const auto* item = selectedAnnotation(state);
    if (!item || !item->text) { return; }
    const auto id = item->objectId;
    auto text = *item->text;
    auto settings = item->settings;
    ImGui::PushID(std::to_string(id).c_str());
    ImGui::TextUnformatted("Text annotation");
    bool style = ImGui::InputText("Name", &settings.name, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::TextUnformatted("Text");
    bool changed = ImGui::InputTextMultiline("##text", &text.text, {-1, ImGui::GetTextLineHeight() * 4});
    style |= ImGui::ColorEdit4("Color", settings.color.data(), ImGuiColorEditFlags_NoInputs);
    changed |= ImGui::DragFloat("Size", &text.size, text.size * .01f, 1e-6f, 1e6f, "%.6g units", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::Checkbox("Always face camera", &text.faceCamera);
    style |= drawVisibilityField("Visible", settings.visible);
    style |= ImGui::Checkbox("Lock position", &settings.locked);
    ImGui::BeginDisabled(settings.locked);
    changed |= ImGui::InputFloat3("Position", text.position.data(), "%.6g", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(text.faceCamera);
    changed |= ImGui::InputFloat3("Rotation", text.rotationDegrees.data(), "%.3f", ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::EndDisabled(); ImGui::EndDisabled();
    ImGui::TextWrapped("Size is the line height in scene units. Drag the text to move it. Fixed text uses its local XY plane. Rotation is in degrees.");
    if (style) { setAnnotationSettings(state, id, std::move(settings)); }
    if (changed) {
        try { setTextAnnotationSettings(state, id, std::move(text)); }
        catch (const std::exception& error) { ImGui::TextWrapped("%s", error.what()); }
    }
    ImGui::PopID();
}
bool beginTextAnnotationPointer(UiState& state, AnnotationInteraction& interaction, const ScenePickView& view, PickPoint point)
{
    const UiAnnotation* hit = nullptr;
    if (!interaction.textTool) {
        if (!ImGui::GetCurrentContext()) { return false; }
        for (auto i = state.annotations.rbegin(); i != state.annotations.rend(); ++i) {
            if (hitText(*i, view, point)) { hit = &*i; break; }
        }
        if (!hit) { return false; }
    }
    UiAnnotation preview;
    if (hit) {
        preview = *hit;
        selectSceneObject(state, hit->objectId);
    } else {
        TextAnnotationSettings text;
        const auto p = textAnnotationPlacement(scenePickParts(state), view, point, state.camera.target);
        if (!p) { interaction.error = "Text cannot be placed behind the camera."; return true; }
        text.position = *p;
        const auto right = textAnnotationPlanePoint(view, {point[0] + 24 * view.pixelScale, point[1]}, *p);
        if (right) {
            double length = 0;
            for (size_t k = 0; k < 3; ++k) { length += std::pow((*right)[k] - (*p)[k], 2); }
            text.size = static_cast<float>(std::sqrt(length));
        }
        text.rotationDegrees = state.upAxis == SceneUpAxis::z ? std::array<float, 3>{-90, 0, 0} : std::array<float, 3>{};
        preview.text = normalizedTextAnnotationSettings(std::move(text));
        preview.targetValid = true;
    }
    interaction.preview = std::move(preview);
    interaction.editing = hit ? hit->objectId : 0;
    interaction.textStart = interaction.preview.text;
    interaction.view = view;
    interaction.pointerStart = interaction.pointerEnd = point;
    interaction.identity = annotationWorkIdentity(state);
    interaction.dragging = true; interaction.error.clear();
    return true;
}
void moveTextAnnotationPointer(AnnotationInteraction& interaction, PickPoint point)
{
    if (!interaction.dragging || !interaction.textStart || interaction.preview.settings.locked) { return; }
    const auto& start = *interaction.textStart;
    const auto a = textAnnotationPlanePoint(interaction.view, interaction.pointerStart, start.position);
    const auto b = textAnnotationPlanePoint(interaction.view, point, start.position);
    if (!a || !b) { return; }
    for (size_t k = 0; k < 3; ++k) { interaction.preview.text->position[k] = start.position[k] + (*b)[k] - (*a)[k]; }
    interaction.pointerEnd = point;
}
void endTextAnnotationPointer(UiState& state, AnnotationInteraction& interaction, bool allowed)
{
    if (allowed && interaction.preview.text && annotationWorkCurrent(interaction.identity, state, true)) {
        if (interaction.editing) { setTextAnnotationSettings(state, interaction.editing, *interaction.preview.text); }
        else { createTextAnnotation(state, *interaction.preview.text); }
    }
    cancelAnnotationPointer(interaction);
}
} // namespace woby
