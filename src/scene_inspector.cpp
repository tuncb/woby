#include "scene_inspector.h"
#include "ui_icon_controls.h"
#include "ui_operations.h"
#include "utf8_path.h"

#include <imgui.h>
#include <algorithm>
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

void resetButton(UiState& state, const char* tooltip, UiPropertyGroup group)
{
    ImGui::PushID(static_cast<int>(group));
    if (drawResetIconButton("reset", tooltip)) { resetSelectedObjectProperties(state, group); }
    ImGui::PopID();
}

bool propertyHeading(UiState& state, const char* title, const char* tooltip,
    UiPropertyGroup group, bool collapsible, const char* informationTitle = nullptr,
    const char* informationText = nullptr)
{
    bool open = false;
    ImGui::PushID(static_cast<int>(group));
    // Separate fixed-width cells keep the passive hint and reset hit target outside the header.
    if (ImGui::BeginTable("heading", informationText ? 3 : 2, ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
        if (informationText) {
            ImGui::TableSetupColumn("Information", ImGuiTableColumnFlags_WidthFixed,
                informationIconSize() + ImGui::GetStyle().ItemSpacing.x);
        }
        ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableNextColumn();
        if (collapsible) {
            open = ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen);
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(title);
            open = true;
        }
        if (informationText) {
            ImGui::TableNextColumn();
            drawInformationIcon("info", informationTitle, informationText);
        }
        ImGui::TableNextColumn();
        resetButton(state, tooltip, group);
        ImGui::EndTable();
    }
    ImGui::PopID();
    return open;
}

void axisFields(UiState& state, const char* title, const char* tooltip,
    UiObjectProperty first, UiPropertyGroup group)
{
    ImGui::PushID(title);
    propertyHeading(state, title, tooltip, group, false);
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

void renderModeField(UiState& state, const char* label, const char* icon, UiObjectProperty property)
{
    const auto current = selectedObjectProperty(state, property);
    bool value = current.value != 0.0f;
    ImGui::BeginDisabled(!current.available);
    if (drawRenderModeField(label, icon, value, current.mixed)) {
        setSelectedObjectProperty(state, property, value ? 1.0f : 0.0f);
    }
    ImGui::EndDisabled();
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
        ImGui::TextUnformatted("Local bounds");
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
            std::string fileName;
            std::string path = object->path.empty() ? std::string{} : pathToUtf8(object->path);
            if (object->kind == SceneObjectKind::group) {
                if (const auto parent = findSceneObject(state, object->fileId)) {
                    fileName = parent->name;
                    path = pathToUtf8(parent->path);
                }
            }
            drawObjectIdentityRow(objectType(object->kind), object->name.c_str(),
                fileName.empty() ? nullptr : fileName.c_str(), path.c_str());
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
        if (propertyHeading(state, "Transform",
                "Reset translation, rotation, and scale on selected objects.", UiPropertyGroup::transform, true)) {
            axisFields(state, "Translation", "Reset translation on selected objects to zero.",
                UiObjectProperty::translationX, UiPropertyGroup::translation);
            axisFields(state, "Rotation (degrees)", "Reset rotation on selected objects to zero.",
                UiObjectProperty::rotationX, UiPropertyGroup::rotation);
            if (ImGui::BeginTable("scale", 3)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                scalarField(state, "Uniform scale (x)", UiObjectProperty::scale);
                ImGui::TableNextColumn();
                resetButton(state, "Reset scale on selected objects to 1.", UiPropertyGroup::scale);
                ImGui::EndTable();
            }
        }
        if (propertyHeading(state, "Appearance",
                "Reset selected objects' opacity and their own and contained parts' vertex size, color, and render modes.",
                UiPropertyGroup::appearance, true,
                "Appearance overrides",
                "Color and render modes apply to selected parts and all parts inside selected files or folders. "
                "Mixed means these parts differ; click a mixed render mode to enable it for all targets.\n\n"
                "Opacity stays local. File and part vertex size multipliers combine; folder vertex size edits contained file multipliers. "
                "Empty containers keep unavailable controls disabled.\n\n"
                "Visibility includes file and folder contents. Hiding objects keeps them selected; "
                "click mixed visibility to show all selected objects.")) {
            if (ImGui::BeginTable("appearance", 2)) {
                scalarField(state, "Opacity (0-100%)", UiObjectProperty::opacity, 100.0f);
                ImGui::EndTable();
            }
            const auto visibility = selectedObjectVisibility(state);
            if (visibility.available) {
                bool visible = visibility.value != 0.0f;
                if (drawVisibilityIconField("selected objects", visible, visibility.mixed)) {
                    setSelectedObjectsVisible(state, visible);
                }
            }
            if (visibility.available) { ImGui::SameLine(); }
            renderModeField(state, "Solid mesh", solidMeshIcon, UiObjectProperty::solidMesh);
            ImGui::SameLine();
            renderModeField(state, "Triangle edges", trianglesIcon, UiObjectProperty::triangles);
            ImGui::SameLine();
            renderModeField(state, "Vertices", verticesIcon, UiObjectProperty::vertices);
            const auto vertexSize = selectedObjectProperty(state, UiObjectProperty::vertexSize);
            {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::BeginDisabled(!vertexSize.available);
                float value = vertexSize.available ? vertexSize.value : 1.0f;
                ImGui::SetNextItemWidth(std::min(uiSize(100.0f), ImGui::GetContentRegionAvail().x));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                    ImVec2(ImGui::GetStyle().FramePadding.x,
                        std::max(0.0f, (renderModeButtonSize() - ImGui::GetFontSize()) * 0.5f)));
                if (ImGui::DragFloat("##vertex_size", &value, 0.05f, minVertexSizeScale, maxVertexSizeScale,
                        vertexSize.mixed ? "Mixed" : "%.2g x", ImGuiSliderFlags_AlwaysClamp)) {
                    setSelectedObjectProperty(state, UiObjectProperty::vertexSize, value);
                }
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Vertex size multiplier on selected files or parts; folders edit contained file multipliers. Ctrl-click to enter a value.");
                }
                ImGui::EndDisabled();
            }
            const auto red = selectedObjectProperty(state, UiObjectProperty::red);
            {
                ImGui::BeginDisabled(!red.available);
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
                ImGui::EndDisabled();
            }
        }
        if (drawInformationHeader("Geometry", "Geometry",
                "Select one file or part to inspect its mesh statistics and local bounds. "
                "For folders, select a child file or part.")) {
            drawGeometry(state);
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

} // namespace woby
