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

SettingsDialogResult drawSettingsDialog(UiState& state, bool requestOpen)
{
    if (requestOpen) { ImGui::OpenPopup("Settings"); }
    const auto* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(uiSize(320.0f), 0.0f), ImGuiCond_Always);
    bool open = true;
    SettingsDialogResult result;
    if (ImGui::BeginPopupModal("Settings", &open,
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
        ImGui::Separator();
        const bool escape = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
            && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)
            && !ImGui::IsAnyItemActive()
            && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (ImGui::Button("Close", ImVec2(uiSize(80.0f), 0.0f)) || escape) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return result;
}

} // namespace woby
