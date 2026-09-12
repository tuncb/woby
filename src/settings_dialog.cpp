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

SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen, const UpdateUiState& update)
{
    if (requestOpen) { ImGui::OpenPopup("Settings"); }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(uiSize(400.0f), 0.0f), ImGuiCond_Always);
    const bool installing = update.activeCommand == UpdateCommand::install || update.closeRequested;
    bool open = true;
    SettingsDialogResult result;
    if (ImGui::BeginPopupModal("Settings", installing ? nullptr : &open,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoMove)) {
        result.open = true;
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
        ImGui::SeparatorText("Updates");
        ImGui::Text("Installed version: %s", update.currentVersion.c_str());
        if (!update.latestVersion.empty()) { ImGui::Text("Latest release: %s", update.latestVersion.c_str()); }
        if (!update.message.empty()) { ImGui::TextWrapped("%s", update.message.c_str()); }
        ImGui::BeginDisabled(updateBusy(update) || update.closeRequested);
        if (ImGui::Button("Check for updates")) { result.updateCommand = UpdateCommand::check; }
        ImGui::EndDisabled();
        if (!update.managedDeployment) {
            ImGui::TextWrapped("This build cannot install updates. Install a portable release to enable updating.");
        } else if (update.available || installing) {
            ImGui::TextWrapped("Save your scene and close other Woby windows using this installation. "
                "Woby will close to install the update and reopen after installation finishes.");
            if (state.isDirty) { ImGui::TextWrapped("Save your scene changes before installing."); }
            ImGui::BeginDisabled(!canInstallUpdate(update, state.isDirty));
            if (ImGui::Button("Install update and restart")) { result.updateCommand = UpdateCommand::install; }
            ImGui::EndDisabled();
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
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    return result;
}

} // namespace woby
