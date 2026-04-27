#include "camera.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace woby {
namespace {

constexpr float pi = 3.14159265358979323846f;

bx::Vec3 toVec3(const std::array<float, 3>& value)
{
    return bx::Vec3(value[0], value[1], value[2]);
}

std::array<float, 3> viewDirection(const SceneCamera& camera)
{
    const float pitchCos = std::cos(camera.pitchRadians);
    return {
        std::sin(camera.yawRadians) * pitchCos,
        std::sin(camera.pitchRadians),
        std::cos(camera.yawRadians) * pitchCos,
    };
}

std::array<float, 3> rightDirection(const SceneCamera& camera)
{
    return {std::cos(camera.yawRadians), 0.0f, -std::sin(camera.yawRadians)};
}

std::array<float, 3> upDirection(const SceneCamera& camera)
{
    const auto view = viewDirection(camera);
    const auto right = rightDirection(camera);
    return {
        right[1] * view[2] - right[2] * view[1],
        right[2] * view[0] - right[0] * view[2],
        right[0] * view[1] - right[1] * view[0],
    };
}

void moveTarget(SceneCamera& camera, const std::array<float, 3>& direction, float amount)
{
    for (size_t axis = 0; axis < camera.target.size(); ++axis) {
        camera.target[axis] += direction[axis] * amount;
    }
}

} // namespace

SceneCamera frameCameraBounds(const Bounds& bounds)
{
    SceneCamera camera;
    camera.target = bounds.center;

    const float radius = std::max(bounds.radius, 0.001f);
    const float halfFovRadians = (camera.verticalFovDegrees * 0.5f) * pi / 180.0f;
    camera.distance = (radius / std::sin(halfFovRadians)) * 1.35f;
    camera.yawRadians = 0.48f;
    camera.pitchRadians = 0.28f;

    return camera;
}

bx::Vec3 cameraEye(const SceneCamera& camera)
{
    const std::array<float, 3> view = viewDirection(camera);
    return bx::Vec3(
        camera.target[0] + view[0] * camera.distance,
        camera.target[1] + view[1] * camera.distance,
        camera.target[2] + view[2] * camera.distance);
}

bx::Vec3 cameraLookAt(const SceneCamera& camera)
{
    return toVec3(camera.target);
}

float cameraFarPlane(const SceneCamera& camera, const Bounds& bounds)
{
    return std::max(camera.distance + bounds.radius * 4.0f, 10.0f);
}

void orbitCamera(SceneCamera& camera, float deltaX, float deltaY)
{
    constexpr float sensitivity = 0.006f;
    camera.yawRadians += deltaX * sensitivity;
    camera.pitchRadians = std::clamp(camera.pitchRadians + deltaY * sensitivity, -1.45f, 1.45f);
}

void panCamera(SceneCamera& camera, float deltaX, float deltaY, float viewportHeight)
{
    const float unitsPerPixel = std::max(camera.distance, 0.001f)
        * std::tan(camera.verticalFovDegrees * 0.5f * pi / 180.0f)
        * 2.0f
        / std::max(viewportHeight, 1.0f);
    moveTarget(camera, rightDirection(camera), deltaX * unitsPerPixel);
    moveTarget(camera, upDirection(camera), -deltaY * unitsPerPixel);
}

void dollyCamera(SceneCamera& camera, float amount)
{
    camera.distance = std::max(camera.distance * std::exp(amount), 0.001f);
}

void moveCameraLocal(SceneCamera& camera, float rightAmount, float upAmount, float forwardAmount)
{
    moveTarget(camera, rightDirection(camera), rightAmount);
    moveTarget(camera, upDirection(camera), upAmount);

    const auto forward = viewDirection(camera);
    moveTarget(camera, forward, -forwardAmount);
}

void updateCameraFromKeyboard(SceneCamera& camera, const Bounds& bounds, float deltaSeconds)
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
    moveCameraLocal(camera, right, up, forward);

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
    orbitCamera(camera, orbitX, orbitY);

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
