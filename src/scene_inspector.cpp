#include "scene_inspector.h"
#include "scene_dimensions.h"
#include "ui_icon_controls.h"
#include "ui_operations.h"
#include "utf8_path.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

namespace woby {
namespace {

struct InspectorEditor {
    const SceneInspectorSnapshot& snapshot;
    std::vector<InspectorEdit>& edits;
};
void setSelectedObjectProperty(InspectorEditor& editor, UiObjectProperty property, float value) {
    editor.edits.push_back({InspectorEditKind::property, property, value, {}});
}
void resetSelectedObjectProperties(InspectorEditor& editor, UiPropertyGroup group) {
    editor.edits.push_back({InspectorEditKind::reset, {}, 0, group});
}
void setSelectedObjectsVisible(InspectorEditor& editor, bool visible) {
    editor.edits.push_back({InspectorEditKind::visibility, {}, visible ? 1.0f : 0.0f, {}});
}

const char* objectType(SceneObjectKind kind)
{
    switch (kind) {
    case SceneObjectKind::folder: return "Folder";
    case SceneObjectKind::file: return "File";
    case SceneObjectKind::group: return "Part";
    case SceneObjectKind::comparison: return "Analysis";
    case SceneObjectKind::annotation: return "Annotation";
    }
    return "Object";
}

void numericInput(InspectorEditor& editor, UiObjectProperty property, float displayScale = 1.0f)
{
    const auto current = inspectorProperty(editor.snapshot, property);
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
        if (end != text && *end == '\0') { setSelectedObjectProperty(editor, property, value / displayScale); }
    }
    ImGui::PopID();
}

void scalarField(InspectorEditor& editor, const char* label, UiObjectProperty property, float displayScale = 1.0f)
{
    if (!inspectorProperty(editor.snapshot, property).available) { return; }
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    numericInput(editor, property, displayScale);
}

void resetButton(InspectorEditor& editor, const char* tooltip, UiPropertyGroup group)
{
    ImGui::PushID(static_cast<int>(group));
    if (drawResetIconButton("reset", tooltip)) { resetSelectedObjectProperties(editor, group); }
    ImGui::PopID();
}

bool propertyHeading(InspectorEditor& editor, const char* title, const char* tooltip,
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
        resetButton(editor, tooltip, group);
        ImGui::EndTable();
    }
    ImGui::PopID();
    return open;
}

void axisFields(InspectorEditor& editor, const char* title, const char* tooltip,
    UiObjectProperty first, UiPropertyGroup group)
{
    ImGui::PushID(title);
    propertyHeading(editor, title, tooltip, group, false);
    if (ImGui::BeginTable("axes", 3)) {
        // Keep labels in their own row so input frame padding cannot shift
        // the text baseline of subsequent columns.
        ImGui::TableNextRow();
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(axis == 0 ? "X" : axis == 1 ? "Y" : "Z");
        }
        ImGui::TableNextRow();
        for (int axis = 0; axis < 3; ++axis) {
            ImGui::TableNextColumn();
            numericInput(editor, static_cast<UiObjectProperty>(static_cast<int>(first) + axis));
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void renderModeField(InspectorEditor& editor, const char* label, const char* icon, UiObjectProperty property)
{
    const auto current = inspectorProperty(editor.snapshot, property);
    bool value = current.value != 0.0f;
    ImGui::BeginDisabled(!current.available);
    if (drawRenderModeField(label, icon, value, current.mixed)) {
        setSelectedObjectProperty(editor, property, value ? 1.0f : 0.0f);
    }
    ImGui::EndDisabled();
}

void drawGeometry(const SceneInspectorSnapshot& snapshot)
{
    const auto& dimensions = snapshot.dimensions;
    if (dimensions) {
        ImGui::Text("Size  X:%.3g  Y:%.3g  Z:%.3g", dimensions->lengths[0], dimensions->lengths[1], dimensions->lengths[2]);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Size\nX: %.9g\nY: %.9g\nZ: %.9g", dimensions->lengths[0], dimensions->lengths[1], dimensions->lengths[2]);
        }
        ImGui::Spacing();
    }
    for (const auto& statistic : snapshot.statistics) { ImGui::TextUnformatted(statistic.c_str()); }
    const auto& bounds = snapshot.bounds;
    if (bounds) {
        std::array<double, 3> minimum{}, maximum{};
        for (size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = (*bounds)[0][axis];
            maximum[axis] = (*bounds)[1][axis];
            // Hide double-precision roundoff relative to this axis's extent,
            // without erasing legitimately tiny models or changing their bounds.
            const double tolerance = std::abs(maximum[axis] - minimum[axis])
                * 16.0 * std::numeric_limits<double>::epsilon();
            if (std::abs(minimum[axis]) <= tolerance) { minimum[axis] = 0.0; }
            if (std::abs(maximum[axis]) <= tolerance) { maximum[axis] = 0.0; }
        }
        ImGui::Text("Local bounds  X:%.3g to %.3g  Y:%.3g to %.3g  Z:%.3g to %.3g",
            minimum[0], maximum[0], minimum[1], maximum[1], minimum[2], maximum[2]);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Original model coordinates\nX: %.17g to %.17g\nY: %.17g to %.17g\nZ: %.17g to %.17g",
                (*bounds)[0][0], (*bounds)[1][0],
                (*bounds)[0][1], (*bounds)[1][1],
                (*bounds)[0][2], (*bounds)[1][2]);
        }
    } else if (!dimensions) {
        ImGui::TextDisabled("No geometry selected");
    }
}

} // namespace

void drawSceneInspectorSnapshot(const SceneInspectorSnapshot& snapshot, std::vector<InspectorEdit>& edits)
{
    if (snapshot.targets.empty()) { ImGui::TextDisabled("No selection"); return; }
    InspectorEditor editor{snapshot, edits};
    ImGui::PushID(snapshot.selectionId.c_str());
    if (snapshot.targets.size() > 1) { ImGui::Text("%zu objects selected", snapshot.targets.size()); }
    const bool many = snapshot.targets.size() > 1;
    if (many) { ImGui::BeginChild("targets", ImVec2(0.0f, ImGui::GetTextLineHeightWithSpacing() * 3.0f)); }
    ImGuiListClipper targets;
    targets.Begin(static_cast<int>(snapshot.targets.size()), ImGui::GetTextLineHeightWithSpacing());
    while (targets.Step()) {
        for (int i = targets.DisplayStart; i < targets.DisplayEnd; ++i) {
            const auto& target = snapshot.targets[static_cast<size_t>(i)];
            drawObjectIdentityRow(objectType(target.kind), target.name.c_str(),
                target.fileName.empty() ? nullptr : target.fileName.c_str(), target.path.c_str());
        }
    }
    if (many) { ImGui::EndChild(); }
    if (!inspectorProperty(editor.snapshot, UiObjectProperty::opacity).available) {
        ImGui::TextDisabled("No shared properties");
        ImGui::PopID();
        return;
    }
    // Keep the target identity visible while scrolling through its properties.
    // The selection-scoped child also starts new targets at the top.
    if (ImGui::BeginChild("property_fields")) {
        if (propertyHeading(editor, "Transform",
                "Reset translation, rotation, and scale on selected objects.", UiPropertyGroup::transform, true)) {
            axisFields(editor, "Translation", "Reset translation on selected objects to zero.",
                UiObjectProperty::translationX, UiPropertyGroup::translation);
            axisFields(editor, "Rotation (degrees)", "Reset rotation on selected objects to zero.",
                UiObjectProperty::rotationX, UiPropertyGroup::rotation);
            if (ImGui::BeginTable("scale", 3)) {
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                scalarField(editor, "Uniform scale (x)", UiObjectProperty::scale);
                ImGui::TableNextColumn();
                resetButton(editor, "Reset scale on selected objects to 1.", UiPropertyGroup::scale);
                ImGui::EndTable();
            }
        }
        if (propertyHeading(editor, "Appearance",
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
                scalarField(editor, "Opacity (0-100%)", UiObjectProperty::opacity, 100.0f);
                ImGui::EndTable();
            }
            const auto visibility = editor.snapshot.visibility;
            if (visibility.available) {
                bool visible = visibility.value != 0.0f;
                if (drawVisibilityIconField("selected objects", visible, visibility.mixed)) {
                    setSelectedObjectsVisible(editor, visible);
                }
            }
            if (visibility.available) { ImGui::SameLine(); }
            renderModeField(editor, "Solid mesh", solidMeshIcon, UiObjectProperty::solidMesh);
            ImGui::SameLine();
            renderModeField(editor, "Triangle edges", trianglesIcon, UiObjectProperty::triangles);
            ImGui::SameLine();
            renderModeField(editor, "Vertices", verticesIcon, UiObjectProperty::vertices);
            const auto vertexSize = inspectorProperty(editor.snapshot, UiObjectProperty::vertexSize);
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
                    setSelectedObjectProperty(editor, UiObjectProperty::vertexSize, value);
                }
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Vertex size multiplier on selected files or parts; folders edit contained file multipliers. Ctrl-click to enter a value.");
                }
                ImGui::EndDisabled();
            }
            const auto red = inspectorProperty(editor.snapshot, UiObjectProperty::red);
            const auto lineDepth = inspectorProperty(editor.snapshot, UiObjectProperty::lineDepthTest);
            if (lineDepth.available) {
                ImGui::Spacing();
                if (ImGui::BeginTable("line_style", 2)) {
                    scalarField(editor, "Line width (1-12 px)", UiObjectProperty::lineWidth);
                    ImGui::EndTable();
                }
                bool onTop = lineDepth.value == 0.0f;
                if (lineDepth.mixed) { ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true); }
                if (ImGui::Checkbox("Draw lines on top", &onTop)) {
                    setSelectedObjectProperty(editor, UiObjectProperty::lineDepthTest, (lineDepth.mixed || onTop) ? 0.0f : 1.0f);
                }
                if (lineDepth.mixed) { ImGui::PopItemFlag(); }
            }
            const auto uv = inspectorProperty(editor.snapshot, UiObjectProperty::uvGrid);
            ImGui::Spacing();
            ImGui::BeginDisabled(!uv.available);
            bool uvEnabled = uv.value != 0.0f;
            if (uv.mixed) { ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true); }
            if (ImGui::Checkbox("UV coloring", &uvEnabled)) {
                setSelectedObjectProperty(editor, UiObjectProperty::uvGrid, uv.mixed || uvEnabled ? 1.0f : 0.0f);
            }
            if (uv.mixed) { ImGui::PopItemFlag(); }
            ImGui::EndDisabled();
            if (!uv.available) { ImGui::TextDisabled("No UV coordinates in this selection"); }
            else {
                ImGui::SameLine();
                drawInformationIcon("uv_help", "UV grid",
                    "Shows supplied UV coordinates on solid surfaces. Cyan lines mark constant U; orange lines mark constant V.\n\n"
                    "Density is cells per UV unit (0.1 to 1000). U and V can be changed independently. "
                    "Files and folders edit all parts with complete UVs; other parts keep normal shading. "
                    "Enable Solid mesh to see the grid.");
                const auto colorMode = inspectorProperty(editor.snapshot,UiObjectProperty::uvColorMode);
                int mode = static_cast<int>(colorMode.value);
                const char* modes[] = {"Grid", "U gradient", "V gradient"};
                if (ImGui::Combo("UV color",&mode,modes,3)) { setSelectedObjectProperty(editor,UiObjectProperty::uvColorMode,static_cast<float>(mode)); }
                if (colorMode.mixed) { ImGui::TextDisabled("Mixed coloring modes"); }
                if (mode != 0 && ImGui::BeginTable("uv_range",2)) {
                    scalarField(editor,"Blue: range minimum",UiObjectProperty::uvMinimum);
                    scalarField(editor,"Yellow: range maximum",UiObjectProperty::uvMaximum);
                    ImGui::EndTable();
                }
                if (mode == 0 && (uvEnabled || uv.mixed) && ImGui::BeginTable("uv_density", 2)) {
                    scalarField(editor, "U cells / UV unit", UiObjectProperty::uvDensityU);
                    scalarField(editor, "V cells / UV unit", UiObjectProperty::uvDensityV);
                    ImGui::EndTable();
                }
            }
            ImGui::Spacing();
            {
                ImGui::BeginDisabled(!red.available);
                const auto green = inspectorProperty(editor.snapshot, UiObjectProperty::green);
                const auto blue = inspectorProperty(editor.snapshot, UiObjectProperty::blue);
                std::array<float, 3> color{red.value, green.value, blue.value};
                if (ImGui::ColorEdit3("Color picker", color.data(), ImGuiColorEditFlags_NoInputs)) {
                    setSelectedObjectProperty(editor, UiObjectProperty::red, color[0]);
                    setSelectedObjectProperty(editor, UiObjectProperty::green, color[1]);
                    setSelectedObjectProperty(editor, UiObjectProperty::blue, color[2]);
                }
                if (red.mixed || green.mixed || blue.mixed) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("Mixed colors");
                }
                ImGui::EndDisabled();
            }
        }
        if (drawInformationHeader("Geometry", "Geometry",
                "Size includes transforms and visible selected parts.\n\n"
                "Select one file or part to inspect its mesh statistics and original local bounds.")) {
            drawGeometry(snapshot);
        }
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void drawSceneInspector(UiState& state, SceneInspectorRuntime& runtime)
{
    updateSceneInspector(runtime, state);
    std::vector<InspectorEdit> edits;
    drawSceneInspectorSnapshot(runtime.snapshot, edits);
    applyInspectorEdits(state, edits);
}

} // namespace woby
