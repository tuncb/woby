#pragma once

#include "obj_mesh.h"

#include <array>

#include <bx/math.h>

namespace woby {

struct SceneCamera {
    std::array<float, 3> target{};
    float yawRadians = 0.0f;
    float pitchRadians = 0.0f;
    float distance = 1.0f;
    float verticalFovDegrees = 60.0f;
    float nearPlane = 0.1f;
};

struct CameraInput {
    bool orbiting = false;
    bool panning = false;
};

[[nodiscard]] SceneCamera frameCameraBounds(const Bounds& bounds);
[[nodiscard]] bx::Vec3 cameraEye(const SceneCamera& camera);
[[nodiscard]] bx::Vec3 cameraLookAt(const SceneCamera& camera);
[[nodiscard]] float cameraFarPlane(const SceneCamera& camera, const Bounds& bounds);

void orbitCamera(SceneCamera& camera, float deltaX, float deltaY);
void panCamera(SceneCamera& camera, float deltaX, float deltaY, float viewportHeight);
void dollyCamera(SceneCamera& camera, float amount);
void moveCameraLocal(SceneCamera& camera, float rightAmount, float upAmount, float forwardAmount);
void updateCameraFromKeyboard(SceneCamera& camera, const Bounds& bounds, float deltaSeconds);

} // namespace woby
