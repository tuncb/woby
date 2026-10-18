#include "ui_bulk_controls.h"
#include "ui_icon_controls.h"
#include "scene_inspector_queries.h"

#include <algorithm>
#include <imgui_internal.h>

namespace woby {
namespace {
const ImGuiID selectionFocusKey = ImHashStr("woby_selection_focus");
}

void beginSelectionRows(UiSelectionRows& rows, const UiState& state)
{
    if (rows.owner != &state || rows.generation != state.sceneGeneration || rows.lastFrame != ImGui::GetFrameCount() - 1) {
        auto* storage = ImGui::GetStateStorage();
        if (storage->GetVoidPtr(selectionFocusKey) == &rows) { storage->SetVoidPtr(selectionFocusKey, nullptr); }
        rows = {};
        rows.owner = &state;
        rows.generation = state.sceneGeneration;
    }
    rows.lastFrame = ImGui::GetFrameCount();
    rows.current.clear();
    rows.top = ImGui::GetCursorScreenPos();
}

std::span<const uint64_t> registerSelectionRow(UiSelectionRows& rows, uint64_t id)
{
    rows.current.push_back(id);
    return selectionContains(rows.previous, id) ? std::span<const uint64_t>(rows.previous)
        : std::span<const uint64_t>(rows.current);
}

UiSelectionShortcuts endSelectionRows(UiSelectionRows& rows)
{
    const auto high = ImVec2(rows.top.x + ImGui::GetContentRegionAvail().x, ImGui::GetCursorScreenPos().y);
    const auto& io = ImGui::GetIO();
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::IsMouseHoveringRect(rows.top, high);
    const bool available = !io.WantTextInput && !ImGui::IsAnyItemActive()
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)
        && !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
    UiSelectionShortcuts shortcuts;
    shortcuts.selectAll = available && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && hovered && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false);
    auto* storage = ImGui::GetStateStorage();
    // Use window-owned adapter state so lists in the same pane cannot both delete.
    // Moving the pointer does not change the target; clicking another list does.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        if (hovered) { storage->SetVoidPtr(selectionFocusKey, &rows); }
        else if (storage->GetVoidPtr(selectionFocusKey) == &rows) { storage->SetVoidPtr(selectionFocusKey, nullptr); }
    }
    if (shortcuts.selectAll) { storage->SetVoidPtr(selectionFocusKey, &rows); }
    shortcuts.remove = available && ImGui::IsWindowFocused() && storage->GetVoidPtr(selectionFocusKey) == &rows
        && !rows.current.empty() && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper
        && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    rows.previous = rows.current;
    return shortcuts;
}

bool selectionContains(std::span<const uint64_t> selection, uint64_t id)
{
    return std::find(selection.begin(), selection.end(), id) != selection.end();
}

std::string bulkMenuLabel(const char* single, const char* verb, size_t count, const char* plural)
{
    return count == 1 ? single : std::string(verb) + " " + std::to_string(count) + " " + plural;
}

void drawBulkMenuHeader(size_t count, const char* noun)
{
    if (count == 1) { ImGui::TextDisabled("1 item selected"); }
    else { ImGui::TextDisabled("%zu %s selected", count, noun); }
    ImGui::Separator();
}

bool drawSelectAllMenu()
{
    ImGui::Separator();
    return ImGui::MenuItem("Select all in this list", "Ctrl+A");
}

void drawSceneVisibilityMenu(UiState& state)
{
    if (ImGui::MenuItem("\xef\x81\xae Show selected")) { setSceneSelectionVisible(state, true); }
    if (ImGui::MenuItem("\xef\x81\xb0 Hide selected")) { setSceneSelectionVisible(state, false); }
    if (ImGui::MenuItem("Fit Selection", nullptr, false, selectedSceneBounds(state).has_value())) { fitCameraToSelection(state); }
}

void drawModelBulkMenu(UiState& state)
{
    drawBulkMenuHeader(state.selectedSceneObjects.size());
    drawSceneVisibilityMenu(state);
    static SceneInspectorRuntime inspector;
    updateSceneInspector(inspector, state);
    const auto properties = inspector.snapshot.properties;
    const auto available = properties[static_cast<size_t>(UiObjectProperty::scale)].available;
    if (ImGui::BeginMenu("Display", available)) {
        for (const auto& [property, label] : {
                std::pair{UiObjectProperty::solidMesh, "Solid mesh"},
                std::pair{UiObjectProperty::triangles, "Triangle edges"},
                std::pair{UiObjectProperty::vertices, "Vertices"}}) {
            const auto value = properties[static_cast<size_t>(property)];
            const bool on = !value.mixed && value.value != 0;
            const auto text = std::string(on ? "Hide " : "Show ") + label;
            if (ImGui::MenuItem(text.c_str(), nullptr, false, value.available)) {
                setSelectedObjectProperty(state, property, on ? 0.0f : 1.0f);
            }
        }
        if (ImGui::BeginMenu("Opacity", properties[static_cast<size_t>(UiObjectProperty::opacity)].available)) {
            for (int percent : {25, 50, 75, 100}) {
                if (ImGui::MenuItem((std::to_string(percent) + "%").c_str())) {
                    setSelectedObjectProperty(state, UiObjectProperty::opacity, static_cast<float>(percent) / 100.0f);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Color", properties[static_cast<size_t>(UiObjectProperty::red)].available)) {
            std::array<float, 3> color{properties[static_cast<size_t>(UiObjectProperty::red)].value,
                properties[static_cast<size_t>(UiObjectProperty::green)].value,
                properties[static_cast<size_t>(UiObjectProperty::blue)].value};
            if (ImGui::ColorEdit3("Selected color", color.data(), ImGuiColorEditFlags_NoInputs)) {
                setObjectColor(state, state.selectedSceneObjects, color);
            }
            if (ImGui::MenuItem("Use source colors")) { setObjectColor(state, state.selectedSceneObjects, {}); }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
    if (!available) { setLastItemTooltip("Select only model files, folders, or parts to edit their display."); }
    if (ImGui::BeginMenu("Reset", available)) {
        for (const auto& [group, label] : {std::pair{UiPropertyGroup::transform, "Transform"},
                std::pair{UiPropertyGroup::translation, "Translation"}, std::pair{UiPropertyGroup::rotation, "Rotation"},
                std::pair{UiPropertyGroup::scale, "Scale"}, std::pair{UiPropertyGroup::appearance, "Appearance"}}) {
            if (ImGui::MenuItem(label)) { resetSelectedObjectProperties(state, group); }
        }
        ImGui::EndMenu();
    }
}

void drawAnnotationStyleMenu(UiState& state)
{
    const bool supported = sceneSelectionHasKind(state, SceneObjectKind::annotation);
    if (!ImGui::BeginMenu("Style", supported)) {
        if (!supported) { setLastItemTooltip("Select only annotations or cross sections to edit their style."); }
        return;
    }
    if (ImGui::MenuItem("Lock selected")) { setSelectedAnnotationStyle(state, true); }
    if (ImGui::MenuItem("Unlock selected")) { setSelectedAnnotationStyle(state, false); }
    const auto* first = findAnnotation(state, state.selectedSceneObjects.front());
    float width = first->settings.width;
    bool surfaceOnly = true, mixed = false, anySection = false, anyText = false;
    for (auto id : state.selectedSceneObjects) {
        const auto* item = findAnnotation(state, id);
        anySection = anySection || item->section.has_value();
        anyText = anyText || item->text.has_value();
        surfaceOnly = surfaceOnly && !item->section;
        mixed = mixed || item->settings.width != width;
    }
    ImGui::BeginDisabled(anyText);
    if (mixed) { ImGui::TextDisabled("Line widths differ"); }
    ImGui::SetNextItemWidth(uiSize(180));
    if (ImGui::SliderFloat("Line width", &width, 1, anySection ? 8.0f : 12.0f, "%.1f px")) {
        setSelectedAnnotationStyle(state, {}, width);
    }
    ImGui::EndDisabled();
    if (anyText) { setLastItemTooltip("Use Size in the text inspector to change text size."); }
    ImGui::BeginDisabled(!surfaceOnly);
    auto color = first->settings.color;
    if (ImGui::ColorEdit4("Color", color.data(), ImGuiColorEditFlags_NoInputs)) { setSelectedAnnotationStyle(state, {}, {}, color); }
    if (!surfaceOnly) { setLastItemTooltip("Cross sections use separate colors for their sources."); }
    ImGui::EndDisabled();
    ImGui::EndMenu();
}
} // namespace woby
