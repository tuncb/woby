#include "manual_path_dialog.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace woby {

std::optional<std::vector<std::filesystem::path>> drawManualPathDialog(const char* title, ManualPathDialog& dialog)
{
    if (!dialog.active) { return std::nullopt; }
    const bool appearing = dialog.requestOpen;
    const auto* viewport = ImGui::GetMainViewport();
    const float scale = ImGui::GetFontSize() / 13.0f;
    const float width = std::min(520.0f * scale, viewport->WorkSize.x);
    if (appearing) {
        ImGui::OpenPopup(title);
        const float x = std::clamp(dialog.anchorX, viewport->WorkPos.x,
            viewport->WorkPos.x + viewport->WorkSize.x - width);
        const float y = std::clamp(dialog.anchorY, viewport->WorkPos.y,
            viewport->WorkPos.y + std::max(0.0f, viewport->WorkSize.y - 350.0f * scale));
        ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Appearing);
        dialog.requestOpen = false;
    }
    ImGui::SetNextWindowSize(ImVec2(width, 0), ImGuiCond_Always);
    bool open = true;
    std::optional<std::vector<std::filesystem::path>> result;
    if (ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("The system file chooser could not be opened. You can paste a path below to continue.");
        const bool multiple = dialog.kind == ManualPathKind::models;
        ImGui::TextWrapped(multiple ? "Paste absolute file paths, one per line. Spaces and surrounding quotes are supported."
            : dialog.kind == ManualPathKind::folder ? "Paste one absolute folder path."
            : "Paste one absolute file path, including the file name.");
        if (dialog.kind == ManualPathKind::saveScene) { ImGui::TextWrapped("The file will be saved with a .woby extension."); }
        if (dialog.kind == ManualPathKind::saveScreenshot) { ImGui::TextWrapped("The file will be saved with a .png extension."); }
        if (appearing) { ImGui::SetKeyboardFocusHere(); }
        // Multiline also for single selections so pasted extra paths can be rejected explicitly.
        if (ImGui::InputTextMultiline("##paths", &dialog.input, ImVec2(-1, ImGui::GetTextLineHeight() * (multiple ? 5.0f : 2.5f)))) {
            dialog.error.clear();
            dialog.overwritePath.reset();
        }
        if (!dialog.error.empty()) { ImGui::TextWrapped("%s", dialog.error.c_str()); }
        if (dialog.overwritePath) { ImGui::TextWrapped("This file already exists. Overwrite it?"); }
        const bool saving = dialog.kind == ManualPathKind::saveScene || dialog.kind == ManualPathKind::saveScreenshot;
        if (ImGui::Button(dialog.overwritePath ? "Overwrite" : saving ? "Save" : "Open")) {
            result = submitManualPathDialog(dialog);
            if (result) { ImGui::CloseCurrentPopup(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            open = false;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::TreeNode("Details")) {
            ImGui::TextWrapped("%s", dialog.nativeError.c_str());
            ImGui::TreePop();
        }
        ImGui::EndPopup();
    }
    if (!open) {
        dialog.active = false;
        dialog.overwritePath.reset();
        result = std::vector<std::filesystem::path>{};
    }
    return result;
}

} // namespace woby
