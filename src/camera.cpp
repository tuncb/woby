#include "camera.h"

#include <algorithm>
#include <cmath>

namespace woby {
namespace {

constexpr float pi = 3.14159265358979323846f;

bx::Vec3 toVec3(const std::array<float, 3>& value)
{
    return bx::Vec3(value[0], value[1], value[2]);
}

std::array<float, 3> viewDirection(const SceneCamera& camera, SceneUpAxis upAxis)
{
    const float pitchCos = std::cos(camera.pitchRadians);
    if (upAxis == SceneUpAxis::y) {
        return {
            std::cos(camera.yawRadians) * pitchCos,
            std::sin(camera.pitchRadians),
            -std::sin(camera.yawRadians) * pitchCos,
        };
    }

    return {
        std::cos(camera.yawRadians) * pitchCos,
        std::sin(camera.yawRadians) * pitchCos,
        std::sin(camera.pitchRadians),
    };
}

std::array<float, 3> rightDirection(const SceneCamera& camera, SceneUpAxis upAxis)
{
    if (upAxis == SceneUpAxis::y) {
        return {std::sin(camera.yawRadians), 0.0f, std::cos(camera.yawRadians)};
    }

    return {std::sin(camera.yawRadians), -std::cos(camera.yawRadians), 0.0f};
}

std::array<float, 3> upDirection(const SceneCamera& camera, SceneUpAxis upAxis)
{
    const auto view = viewDirection(camera, upAxis);
    const auto right = rightDirection(camera, upAxis);
    return {
        right[1] * view[2] - right[2] * view[1],
        right[2] * view[0] - right[0] * view[2],
        right[0] * view[1] - right[1] * view[0],
    };
}

std::array<float, 3> rolledRightDirection(const SceneCamera& camera, SceneUpAxis upAxis)
{
    const auto right = rightDirection(camera, upAxis);
    const auto up = upDirection(camera, upAxis);
    const float rollCos = std::cos(camera.rollRadians);
    const float rollSin = std::sin(camera.rollRadians);
    return {
        right[0] * rollCos - up[0] * rollSin,
        right[1] * rollCos - up[1] * rollSin,
        right[2] * rollCos - up[2] * rollSin,
    };
}

std::array<float, 3> rolledUpDirection(const SceneCamera& camera, SceneUpAxis upAxis)
{
    const auto right = rightDirection(camera, upAxis);
    const auto up = upDirection(camera, upAxis);
    const float rollCos = std::cos(camera.rollRadians);
    const float rollSin = std::sin(camera.rollRadians);
    return {
        up[0] * rollCos + right[0] * rollSin,
        up[1] * rollCos + right[1] * rollSin,
        up[2] * rollCos + right[2] * rollSin,
    };
}

void moveTarget(SceneCamera& camera, const std::array<float, 3>& direction, float amount)
{
    for (size_t axis = 0; axis < camera.target.size(); ++axis) {
        camera.target[axis] += direction[axis] * amount;
    }
}

} // namespace

SceneCamera frameCameraBounds(const Bounds& bounds, SceneUpAxis upAxis)
{
    SceneCamera camera;
    camera.target = bounds.center;

    const float radius = std::max(bounds.radius, 0.001f);
    const float halfFovRadians = (camera.verticalFovDegrees * 0.5f) * pi / 180.0f;
    camera.distance = (radius / std::sin(halfFovRadians)) * 1.35f;
    camera.yawRadians = 0.48f;
    camera.pitchRadians = 0.28f;
    if (upAxis == SceneUpAxis::y) {
        camera.yawRadians = -0.48f;
    }

    return camera;
}

bx::Vec3 cameraEye(const SceneCamera& camera, SceneUpAxis upAxis)
{
    const std::array<float, 3> view = viewDirection(camera, upAxis);
    return bx::Vec3(
        camera.target[0] + view[0] * camera.distance,
        camera.target[1] + view[1] * camera.distance,
        camera.target[2] + view[2] * camera.distance);
}

bx::Vec3 cameraLookAt(const SceneCamera& camera)
{
    return toVec3(camera.target);
}

bx::Vec3 cameraUp(const SceneCamera& camera, SceneUpAxis upAxis)
{
    return toVec3(rolledUpDirection(camera, upAxis));
}

float cameraFarPlane(const SceneCamera& camera, const Bounds& bounds)
{
    return std::max(camera.distance + bounds.radius * 4.0f, 10.0f);
}

void orbitCamera(SceneCamera& camera, float deltaX, float deltaY, SceneUpAxis upAxis)
{
    constexpr float sensitivity = 0.006f;
    const float yawSign = upAxis == SceneUpAxis::y ? -1.0f : 1.0f;
    camera.yawRadians += deltaX * sensitivity * yawSign;
    camera.pitchRadians = std::clamp(camera.pitchRadians + deltaY * sensitivity, -1.45f, 1.45f);
}

void rollCamera(SceneCamera& camera, float deltaX)
{
    constexpr float sensitivity = 0.006f;
    camera.rollRadians += deltaX * sensitivity;
}

void panCamera(
    SceneCamera& camera,
    float deltaX,
    float deltaY,
    float viewportHeight,
    SceneUpAxis upAxis)
{
    const float unitsPerPixel = std::max(camera.distance, 0.001f)
        * std::tan(camera.verticalFovDegrees * 0.5f * pi / 180.0f)
        * 2.0f
        / std::max(viewportHeight, 1.0f);
    moveTarget(camera, rolledRightDirection(camera, upAxis), deltaX * unitsPerPixel);
    moveTarget(camera, rolledUpDirection(camera, upAxis), deltaY * unitsPerPixel);
}

void dollyCamera(SceneCamera& camera, float amount)
{
    camera.distance = std::max(camera.distance * std::exp(amount), 0.001f);
}

void moveCameraLocal(
    SceneCamera& camera,
    float rightAmount,
    float upAmount,
    float forwardAmount,
    SceneUpAxis upAxis)
{
    moveTarget(camera, rolledRightDirection(camera, upAxis), rightAmount);
    moveTarget(camera, rolledUpDirection(camera, upAxis), upAmount);

    const auto forward = viewDirection(camera, upAxis);
    moveTarget(camera, forward, -forwardAmount);
}

} // namespace woby
