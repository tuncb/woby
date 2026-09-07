#include "scene_inspector.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "utf8_path.h"

#include <imgui.h>
#include <charconv>
#include <cstdlib>
#include <string>

namespace woby {
namespace {

const char* objectType(SceneObjectKind kind)
{
    switch (kind) {
    case SceneObjectKind::folder: return "Folder";
    case SceneObjectKind::file: return "File";
    case SceneObjectKind::group: return "Part";
    case SceneObjectKind::comparison: return "Comparison";
    }
    return "Object";
}

void numericInput(UiState& state, UiObjectProperty property, float displayScale = 1.0f)
{
    const auto current = selectedObjectProperty(state, property);
    if (!current.available) { return; }
    ImGui::PushID(static_cast<int>(property));
    char text[64]{};
    if (!current.mixed) {
        const auto result = std::to_chars(text, text + sizeof(text) - 1, current.value * displayScale);
        *result.ptr = '\0';
    }
    ImGui::SetNextItemWidth(-1.0f);
    // Empty mixed fields accept a precise number without suggesting that the
    // first target's value applies to all targets. Commit on Enter.
    if (ImGui::InputTextWithHint("##value", "Mixed", text, sizeof(text),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsScientific | ImGuiInputTextFlags_AutoSelectAll)) {
        char* end = nullptr;
        const float value = std::strtof(text, &end);
        if (end != text && *end == '\0') { setSelectedObjectProperty(state, property, value / displayScale); }
    }
    ImGui::PopID();
}

void scalarField(UiState& state, const char* label, UiObjectProperty property, float displayScale = 1.0f)
{
    if (!selectedObjectProperty(state, property).available) { return; }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    numericInput(state, property, displayScale);
}

void resetButton(UiState& state, const char* label, UiPropertyGroup group)
{
    if (ImGui::SmallButton(label)) { resetSelectedObjectProperties(state, group); }
}

void axisFields(UiState& state, const char* title, UiObjectProperty first, UiPropertyGroup group)
{
    ImGui::PushID(title);
    ImGui::TextUnformatted(title);
    ImGui::SameLine();
    resetButton(state, "Reset", group);
    if (ImGui::BeginTable("axes", 3)) {
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(axis == 0 ? "X" : axis == 1 ? "Y" : "Z");
            numericInput(state, static_cast<UiObjectProperty>(static_cast<int>(first) + axis));
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void renderModeField(UiState& state, const char* label, UiObjectProperty property)
{
    const auto current = selectedObjectProperty(state, property);
    if (!current.available) { return; }
    bool value = current.value != 0.0f;
    if (ImGui::Checkbox(label, &value)) {
        setSelectedObjectProperty(state, property, current.mixed || value ? 1.0f : 0.0f);
    }
    if (current.mixed) { ImGui::SameLine(); ImGui::TextDisabled("Mixed"); }
}

void drawGeometry(const UiState& state)
{
    if (state.selectedSceneObjects.size() != 1) {
        ImGui::TextDisabled("No geometry selected");
        return;
    }
    const auto id = state.selectedSceneObjects.front();
    const Bounds* bounds = nullptr;
    for (const auto& file : state.files) {
        if (file.objectId == id) {
            ImGui::Text("%zu parts | %zu vertices | %zu triangles", file.groupSettings.size(),
                file.mesh.vertices.size(), file.mesh.indices.size() / 3u);
            bounds = &file.mesh.bounds;
            break;
        }
        for (size_t i = 0; i < file.groupSettings.size() && i < file.mesh.nodes.size(); ++i) {
            if (file.groupSettings[i].objectId == id) {
                ImGui::Text("%u triangles", file.mesh.nodes[i].indexCount / 3u);
                if (file.groupSettings[i].localBoundsValid) { bounds = &file.groupSettings[i].localBounds; }
                break;
            }
        }
    }
    if (bounds) {
        ImGui::TextUnformatted("Local bounds (model units)");
        for (size_t axis = 0; axis < 3; ++axis) {
            ImGui::Text("%s: %.6g to %.6g", axis == 0 ? "X" : axis == 1 ? "Y" : "Z",
                static_cast<double>(bounds->min[axis]), static_cast<double>(bounds->max[axis]));
        }
    } else {
        ImGui::TextDisabled("No geometry selected");
    }
}

} // namespace

void drawSceneInspector(UiState& state)
{
    ImGui::TextUnformatted("Local settings");
    ImGui::SameLine();
    drawInformationIcon("settings_info", "How settings apply",
        "Select a folder, file, part, or comparison in the tree to edit its properties. Ctrl-click selects multiple objects.\n\n"
        "Settings are local. Parent transforms compose with part transforms; parent and part opacity multiply. "
        "Translation uses model units, with no assumed physical unit. Rotation uses degrees; scale is a uniform multiplier.\n\n"
        "Edits set only the entered field on each selected object. Selected parents and children both change. "
        "Mixed means values differ. Enter applies numeric entry; Escape cancels it. "
        "Resets affect only their named property group on selected objects.\n\n"
        "Select one comparison to edit its properties, or select only folders, files, and parts to edit shared scene properties.");
    if (state.selectedSceneObjects.empty()) {
        ImGui::TextDisabled("No selection");
        return;
    }
    // Changing targets must not carry an in-progress numeric edit to another object.
    std::string selectionKey;
    for (const auto id : state.selectedSceneObjects) { selectionKey += std::to_string(id) + ":"; }
    ImGui::PushID(selectionKey.c_str());
    if (state.selectedSceneObjects.size() > 1) { ImGui::Text("%zu objects selected", state.selectedSceneObjects.size()); }
    const bool many = state.selectedSceneObjects.size() > 1;
    if (many) { ImGui::BeginChild("targets", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f)); }
    for (const auto id : state.selectedSceneObjects) {
        if (const auto object = findSceneObject(state, id)) {
            ImGui::TextWrapped("%s: %s", objectType(object->kind), object->name.c_str());
            if (object->kind == SceneObjectKind::group) {
                if (const auto parent = findSceneObject(state, object->fileId)) {
                    ImGui::TextWrapped("In file: %s", parent->name.c_str());
                }
            }
            if (!object->path.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", pathToUtf8(object->path).c_str());
            }
        }
    }
    if (many) { ImGui::EndChild(); }
    if (!selectedObjectProperty(state, UiObjectProperty::opacity).available) {
        ImGui::TextDisabled("No shared properties");
        ImGui::PopID();
        return;
    }
    // Keep the target identity visible while scrolling through its properties.
    // The selection-scoped child also starts new targets at the top.
    if (ImGui::BeginChild("property_fields")) {
        if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            resetButton(state, "Reset transform", UiPropertyGroup::transform);
            axisFields(state, "Translation (model units)", UiObjectProperty::translationX, UiPropertyGroup::translation);
            axisFields(state, "Rotation (degrees)", UiObjectProperty::rotationX, UiPropertyGroup::rotation);
            if (ImGui::BeginTable("scale", 2)) {
                scalarField(state, "Uniform scale (x)", UiObjectProperty::scale);
                ImGui::EndTable();
            }
            resetButton(state, "Reset scale", UiPropertyGroup::scale);
        }
        if (drawInformationHeader("Appearance", "Appearance overrides",
                "Unselected children keep their overrides.\n\n"
                "Color and render modes belong to parts. Select parts to edit these overrides. "
                "For mixed render modes, click to enable all selected parts.")) {
            resetButton(state, "Reset appearance", UiPropertyGroup::appearance);
            if (ImGui::BeginTable("appearance", 2)) {
                scalarField(state, "Opacity (0-100%)", UiObjectProperty::opacity, 100.0f);
                scalarField(state, "Vertex size (x)", UiObjectProperty::vertexSize);
                scalarField(state, "Red (0-1)", UiObjectProperty::red);
                scalarField(state, "Green (0-1)", UiObjectProperty::green);
                scalarField(state, "Blue (0-1)", UiObjectProperty::blue);
                ImGui::EndTable();
            }
            renderModeField(state, "Solid mesh", UiObjectProperty::solidMesh);
            renderModeField(state, "Triangle edges", UiObjectProperty::triangles);
            renderModeField(state, "Vertices", UiObjectProperty::vertices);
            const auto red = selectedObjectProperty(state, UiObjectProperty::red);
            if (red.available) {
                const auto green = selectedObjectProperty(state, UiObjectProperty::green);
                const auto blue = selectedObjectProperty(state, UiObjectProperty::blue);
                std::array<float, 3> color{red.value, green.value, blue.value};
                if (ImGui::ColorEdit3("Color picker", color.data(), ImGuiColorEditFlags_NoInputs)) {
                    setSelectedObjectProperty(state, UiObjectProperty::red, color[0]);
                    setSelectedObjectProperty(state, UiObjectProperty::green, color[1]);
                    setSelectedObjectProperty(state, UiObjectProperty::blue, color[2]);
                }
                if (red.mixed || green.mixed || blue.mixed) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Mixed colors");
                }
            }
        }
        if (drawInformationHeader("Geometry", "Geometry",
                "Select one file or part to inspect its mesh statistics and local bounds. "
                "For folders, select a child file or part. Bounds use model units.")) {
            drawGeometry(state);
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

} // namespace woby
