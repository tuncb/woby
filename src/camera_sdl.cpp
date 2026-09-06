#include "camera.h"
#include "ui_operations.h"

#include <imgui.h>

#include <algorithm>

namespace woby {

void updateCameraFromKeyboard(UiState& state, float deltaSeconds)
{
    const auto& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard || io.WantTextInput || ImGui::IsAnyItemActive()
        || io.KeyCtrl || io.KeyAlt || io.KeySuper
        || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
        return;
    }

    float moveSpeed = std::max(state.sceneBounds.radius, state.camera.distance * 0.35f) * deltaSeconds;
    if (io.KeyShift) {
        moveSpeed *= 4.0f;
    }

    float right = 0.0f;
    float up = 0.0f;
    float forward = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_A)) {
        right -= moveSpeed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_D)) {
        right += moveSpeed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_Q)) {
        up -= moveSpeed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_E)) {
        up += moveSpeed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_W)) {
        forward += moveSpeed;
    }
    if (ImGui::IsKeyDown(ImGuiKey_S)) {
        forward -= moveSpeed;
    }
    if (right != 0.0f || up != 0.0f || forward != 0.0f) {
        navigateUiCamera(state, {.right = right, .up = up, .forward = forward});
    }

    constexpr float orbitPixelsPerSecond = 180.0f;
    float orbitX = 0.0f;
    float orbitY = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) {
        orbitX -= orbitPixelsPerSecond * deltaSeconds;
    }
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) {
        orbitX += orbitPixelsPerSecond * deltaSeconds;
    }
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) {
        orbitY -= orbitPixelsPerSecond * deltaSeconds;
    }
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) {
        orbitY += orbitPixelsPerSecond * deltaSeconds;
    }
    orbitUiCamera(state, orbitX, orbitY);

    float zoom = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_Equal) || ImGui::IsKeyDown(ImGuiKey_KeypadAdd)) {
        zoom -= 1.4f * deltaSeconds;
    }
    if (ImGui::IsKeyDown(ImGuiKey_Minus) || ImGui::IsKeyDown(ImGuiKey_KeypadSubtract)) {
        zoom += 1.4f * deltaSeconds;
    }
    dollyUiCamera(state, zoom);
}

} // namespace woby
