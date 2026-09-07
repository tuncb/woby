#pragma once

#include <imgui.h>
#include <imgui_internal.h>

namespace woby {

// Call after widgets handle input, before scene shortcuts. Keyboard navigation
// stays disabled so arrow keys remain available for the camera.
inline void dismissPopupOnEscape()
{
    auto& context = *ImGui::GetCurrentContext();
    if (!ImGui::IsKeyPressed(ImGuiKey_Escape, ImGuiInputFlags_None, ImGuiKeyOwner_NoOwner)
        || context.ActiveId != 0 || context.ActiveIdPreviousFrame != 0
        || context.OpenPopupStack.empty()) {
        return;
    }
    const auto* popup = context.OpenPopupStack.back().Window;
    if (popup == nullptr || (popup->Flags & ImGuiWindowFlags_Modal) != 0) { return; }
    // Internal API is needed for built-in ColorEdit/Combo popups, whose Begin/
    // End scopes belong to ImGui. Close only the innermost popup; modal dialogs
    // retain their own cancellation logic. Active edits get Escape first.
    ImGui::ClosePopupToLevel(context.OpenPopupStack.Size - 1, true);
}

} // namespace woby
