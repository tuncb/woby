#include "camera.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>

namespace woby {

void updateCameraFromKeyboard(
    SceneCamera& camera,
    const Bounds& bounds,
    float deltaSeconds,
    SceneUpAxis upAxis)
{
    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }

    const bool* keys = SDL_GetKeyboardState(nullptr);
    float moveSpeed = std::max(bounds.radius, camera.distance * 0.35f) * deltaSeconds;
    if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) {
        moveSpeed *= 4.0f;
    }
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) {
        moveSpeed *= 0.25f;
    }

    float right = 0.0f;
    float up = 0.0f;
    float forward = 0.0f;
    if (keys[SDL_SCANCODE_A]) {
        right -= moveSpeed;
    }
    if (keys[SDL_SCANCODE_D]) {
        right += moveSpeed;
    }
    if (keys[SDL_SCANCODE_Q]) {
        up -= moveSpeed;
    }
    if (keys[SDL_SCANCODE_E]) {
        up += moveSpeed;
    }
    if (keys[SDL_SCANCODE_W]) {
        forward += moveSpeed;
    }
    if (keys[SDL_SCANCODE_S]) {
        forward -= moveSpeed;
    }
    moveCameraLocal(camera, right, up, forward, upAxis);

    constexpr float orbitPixelsPerSecond = 180.0f;
    float orbitX = 0.0f;
    float orbitY = 0.0f;
    if (keys[SDL_SCANCODE_LEFT]) {
        orbitX -= orbitPixelsPerSecond * deltaSeconds;
    }
    if (keys[SDL_SCANCODE_RIGHT]) {
        orbitX += orbitPixelsPerSecond * deltaSeconds;
    }
    if (keys[SDL_SCANCODE_UP]) {
        orbitY -= orbitPixelsPerSecond * deltaSeconds;
    }
    if (keys[SDL_SCANCODE_DOWN]) {
        orbitY += orbitPixelsPerSecond * deltaSeconds;
    }
    orbitCamera(camera, orbitX, orbitY, upAxis);

    float zoom = 0.0f;
    if (keys[SDL_SCANCODE_EQUALS] || keys[SDL_SCANCODE_KP_PLUS]) {
        zoom -= 1.4f * deltaSeconds;
    }
    if (keys[SDL_SCANCODE_MINUS] || keys[SDL_SCANCODE_KP_MINUS]) {
        zoom += 1.4f * deltaSeconds;
    }
    dollyCamera(camera, zoom);
}

} // namespace woby
