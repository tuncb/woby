#pragma once

#include "model_mesh.h"
#include "scene_up_axis.h"

#include <array>

#include <bx/math.h>

namespace woby {

struct SceneCamera {
    std::array<float, 3> target{};
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
    float rollRadians = 0.0f;
    float distance = 1.0f;
    float verticalFovDegrees = 60.0f;
    float nearPlane = 0.1f;
};

struct CameraInput {
    bool orbiting = false;
    bool rolling = false;
    bool panning = false;
};

[[nodiscard]] SceneCamera frameCameraBounds(
    const Bounds& bounds,
    SceneUpAxis upAxis = SceneUpAxis::z);
[[nodiscard]] bx::Vec3 cameraEye(
    const SceneCamera& camera,
    SceneUpAxis upAxis = SceneUpAxis::z);
[[nodiscard]] bx::Vec3 cameraLookAt(const SceneCamera& camera);
[[nodiscard]] bx::Vec3 cameraUp(
    const SceneCamera& camera,
    SceneUpAxis upAxis = SceneUpAxis::z);
[[nodiscard]] float cameraFarPlane(const SceneCamera& camera, const Bounds& bounds);

void orbitCamera(
    SceneCamera& camera,
    float deltaX,
    float deltaY,
    SceneUpAxis upAxis = SceneUpAxis::z);
void rollCamera(SceneCamera& camera, float deltaX);
void panCamera(
    SceneCamera& camera,
    float deltaX,
    float deltaY,
    float viewportHeight,
    SceneUpAxis upAxis = SceneUpAxis::z);
void dollyCamera(SceneCamera& camera, float amount);
void moveCameraLocal(
    SceneCamera& camera,
    float rightAmount,
    float upAmount,
    float forwardAmount,
    SceneUpAxis upAxis = SceneUpAxis::z);
void updateCameraFromKeyboard(
    SceneCamera& camera,
    const Bounds& bounds,
    float deltaSeconds,
    SceneUpAxis upAxis = SceneUpAxis::z);

} // namespace woby
