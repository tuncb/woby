#include "settings_dialog.h"

#include "ui_icon_controls.h"
#include "ui_operations.h"
#include "ui_state.h"

#include <imgui.h>
#include <cmath>

namespace woby {

bool drawSettingsButton(bool disabled)
{
    return drawRenderModeIconButton("settings", "\xef\x80\x93", "Settings",
        RenderModeState::off, disabled);
}

namespace {
SettingsDialogResult drawPreferencesDialog(UiState& state, bool requestOpen, const UpdateUiState& update,
    bool updates, ImVec2 anchor)
{
    const char* title = updates ? "Updates" : "Settings";
    if (requestOpen) { ImGui::OpenPopup(title); }
    const auto* viewport = ImGui::GetMainViewport();
    const auto position = anchor.x >= 0 && anchor.y >= 0 ? anchor : viewport->GetCenter();
    const auto pivot = anchor.x >= 0 && anchor.y >= 0 ? ImVec2(0, 0) : ImVec2(0.5f, 0.5f);
    if (requestOpen) { ImGui::SetNextWindowPos(position, ImGuiCond_Appearing, pivot); }
    ImGui::SetNextWindowSize(ImVec2(uiSize(400.0f), 0.0f), ImGuiCond_Always);
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

SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen, ImVec2 anchor)
{
    return drawPreferencesDialog(state, requestOpen, {}, false, anchor);
}

SettingsDialogResult drawUpdatesDialog(UiState& state, bool requestOpen, const UpdateUiState& update, ImVec2 anchor)
{
    return drawPreferencesDialog(state, requestOpen, update, true, anchor);
}

} // namespace woby
