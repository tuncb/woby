#include "settings_dialog.h"

#include "ui_icon_controls.h"
#include "ui_operations.h"
#include "ui_state.h"
#include "utf8_path.h"

#include <imgui.h>
#include <cmath>

namespace woby {

bool drawSettingsButton(bool disabled)
{
    return drawRenderModeIconButton("settings", "\xef\x80\x93", "Settings",
        RenderModeState::off, disabled);
}

namespace {
void associationTooltip(const FileAssociationStatus& status, bool model)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || !ImGui::BeginTooltip()) { return; }
    ImGui::PushTextWrapPos(uiSize(360.0f));
    ImGui::TextUnformatted("Select to add this copy to Open with. Clear to remove it. Choose the default app in Windows.");
    ImGui::Text("Default app: %s", status.defaultApp.c_str());
    if (!status.registeredExecutable.empty()) {
        ImGui::Text("Registered copy: %s", pathToUtf8(status.registeredExecutable).c_str());
    }
    if (model) { ImGui::TextUnformatted("Windows also uses .obj for compiled object files."); }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

void drawFileAssociations(const FileAssociationsState& associations, SettingsDialogResult& result)
{
    if (!associations.supported) { return; }
    ImGui::SeparatorText("File associations");
    ImGui::BeginDisabled(associations.busy || !associations.checked);
    for (size_t index = 0; index < associations.types.size(); ++index) {
        ImGui::PushID(static_cast<int>(index));
        const auto type = index == 0 ? FileAssociationType::scene : FileAssociationType::obj;
        const auto& status = associations.types[index];
        bool registered = status.registeredCurrentCopy;
        if (ImGui::Checkbox(index == 0 ? "Woby scenes (.woby)" : "OBJ models (.obj)", &registered)) {
            result.fileAssociationRequest = {registered
                ? FileAssociationAction::registerCopy : FileAssociationAction::removeRegistration, type};
        }
        associationTooltip(status, index == 1);
        const char* label = status.defaultCurrentCopy ? "Default"
            : status.registeredCurrentCopy ? "Open with"
            : !status.registeredExecutable.empty() ? "Re-register" : nullptr;
        if (label && associations.checked) {
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(label).x);
            ImGui::TextDisabled("%s", label);
            associationTooltip(status, index == 1);
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(associations.busy || !associations.checked
        || (!associations.types[0].registeredCurrentCopy && !associations.types[1].registeredCurrentCopy));
    if (ImGui::Button("Windows defaults...")) {
        result.fileAssociationRequest.action = FileAssociationAction::openDefaults;
    }
    setLastItemTooltip("Select the default app for .woby and .obj in Windows.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    drawInformationIcon("associations_info", "File associations",
        "The checkboxes add this copy to Open with for the selected file types. "
        "Use Windows defaults to select the default app. These settings apply to this Windows user. "
        "If you move Woby, select the file types again to register the new copy.");
    if (associations.busy) { ImGui::TextDisabled("Checking..."); }
    if (associations.failed) {
        ImGui::TextUnformatted("Could not update file associations.");
        setLastItemTooltip(associations.message.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(associations.busy);
        if (ImGui::SmallButton("Retry")) { result.fileAssociationRequest.action = FileAssociationAction::refresh; }
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
}

SettingsDialogResult drawPreferencesDialog(UiState& state, bool requestOpen, const UpdateUiState& update,
    bool updates, ImVec2 anchor, const FileAssociationsState& associations = {})
{
    const char* title = updates ? "Updates" : "Settings";
    if (requestOpen) { ImGui::OpenPopup(title); }
    const auto* viewport = ImGui::GetMainViewport();
    const auto position = anchor.x >= 0 && anchor.y >= 0 ? anchor : viewport->GetCenter();
    const auto pivot = anchor.x >= 0 && anchor.y >= 0 ? ImVec2(0, 0) : ImVec2(0.5f, 0.5f);
    if (requestOpen) { ImGui::SetNextWindowPos(position, ImGuiCond_Appearing, pivot); }
    ImGui::SetNextWindowSize(ImVec2(uiSize(400.0f), 0.0f), ImGuiCond_Always);
    if (!updates && associations.supported) {
        // File paths can wrap. Keep Close reachable on small displays.
        ImGui::SetNextWindowSizeConstraints(ImVec2(uiSize(400.0f), 0.0f),
            ImVec2(uiSize(400.0f), viewport->WorkSize.y - uiSize(24.0f)));
    }
    const bool installing = updates && (update.activeCommand == UpdateCommand::install || update.closeRequested);
    bool open = true;
    SettingsDialogResult result;
    if (ImGui::BeginPopupModal(title, installing ? nullptr : &open,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoMove)) {
        result.open = true;
        if (!updates) {
            ImGui::SeparatorText("Interface");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("UI scale");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-informationIconSize() - ImGui::GetStyle().ItemSpacing.x);
            int scaleIndex = static_cast<int>(std::round((state.uiScale - 1.0f) * 4.0f));
            if (ImGui::Combo("##ui_scale", &scaleIndex, "100%\0" "125%\0" "150%\0" "175%\0" "200%\0")) {
                setUiScale(state, 1.0f + static_cast<float>(scaleIndex) * 0.25f);
                result.scaleChanged = true;
            }
            ImGui::SameLine();
            drawInformationIcon("interface_info", "Interface scale",
                "Text and control size, in addition to Windows display scaling. Saved for this user.");
            ImGui::Spacing();
            ImGui::SeparatorText("Camera");
            float fov = state.camera.verticalFovDegrees;
            ImGui::SetNextItemWidth(uiSize(100.0f));
            if (ImGui::DragFloat("Field of view", &fov, 0.5f, 1.0f, 179.0f, "%.1f deg")) {
                CameraPlacement placement;
                placement.fovDegrees = fov;
                setUiCamera(state, placement);
            }
            setLastItemTooltip("Field of view on the shorter viewport axis, in degrees.");
            bool automatic = state.camera.automaticNearPlane;
            if (ImGui::Checkbox("Automatic near clipping", &automatic)) {
                CameraPlacement placement;
                placement.automaticNearPlane = automatic;
                setUiCamera(state, placement);
            }
            if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                ImGui::PushTextWrapPos(uiSize(360.0f));
                ImGui::TextUnformatted("Adapt clipping to the scene bounds and camera position. Clear to set the near distance manually.");
                const auto depth = cameraDepthRange(state.camera, state.sceneBounds, state.upAxis);
                ImGui::Text("Near distance: %.6g", depth.nearPlane);
                ImGui::Text("Far distance: %.6g", depth.farPlane);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }
            ImGui::SameLine();
            drawInformationIcon("camera_info", "Camera settings", "Saved with the scene and views.");
            if (!state.camera.automaticNearPlane) {
                float nearPlane = state.camera.nearPlane;
                ImGui::SetNextItemWidth(uiSize(100.0f));
                if (ImGui::InputFloat("Manual near distance", &nearPlane, 0, 0, "%.6g")) {
                    CameraPlacement placement;
                    placement.nearPlane = nearPlane;
                    setUiCamera(state, placement);
                }
                setLastItemTooltip("Minimum visible distance ahead of the camera, in model units. Larger values cut into nearby surfaces.");
            }
            ImGui::Spacing();
            drawFileAssociations(associations, result);
        } else {
            ImGui::SeparatorText("Updates");
            ImGui::Text("Installed version: %s", update.currentVersion.c_str());
            if (!update.latestVersion.empty()) { ImGui::Text("Latest release: %s", update.latestVersion.c_str()); }
            if (!update.message.empty()) { ImGui::TextWrapped("%s", update.message.c_str()); }
            ImGui::BeginDisabled(updateBusy(update) || update.closeRequested);
            if (ImGui::Button("Check for updates")) { result.updateCommand = UpdateCommand::check; }
            setLastItemTooltip("Check whether a newer Woby release is available.");
            ImGui::EndDisabled();
            if (!update.managedDeployment) {
                ImGui::TextWrapped("This build cannot install updates. Install a portable release to enable updating.");
            } else if (update.available || installing) {
                ImGui::TextWrapped("Save your scene and close other Woby windows using this installation. "
                    "Woby will close to install the update and reopen after installation finishes.");
                if (state.isDirty) { ImGui::TextWrapped("Save your scene changes before installing."); }
                ImGui::BeginDisabled(!canInstallUpdate(update, state.isDirty));
                if (ImGui::Button("Install update and restart")) { result.updateCommand = UpdateCommand::install; }
                setLastItemTooltip("Install the latest release and restart Woby. Save your scene first.");
                ImGui::EndDisabled();
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        const bool escape = !installing && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)
            && !ImGui::IsAnyItemActive()
            && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        ImGui::BeginDisabled(installing);
        if (ImGui::Button("Close", ImVec2(uiSize(80.0f), 0.0f)) || escape) {
            ImGui::CloseCurrentPopup();
        }
        setLastItemTooltip("Close and return to the scene (Esc).");
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    return result;
}

} // namespace

SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen, ImVec2 anchor,
    const FileAssociationsState& associations)
{
    return drawPreferencesDialog(state, requestOpen, {}, false, anchor, associations);
}

SettingsDialogResult drawUpdatesDialog(UiState& state, bool requestOpen, const UpdateUiState& update, ImVec2 anchor)
{
    return drawPreferencesDialog(state, requestOpen, update, true, anchor);
}

} // namespace woby
