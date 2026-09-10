#include "camera.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

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

SceneCamera normalizedSceneCamera(SceneCamera camera)
{
    for (const float value : {camera.target[0], camera.target[1], camera.target[2],
        camera.yawRadians, camera.pitchRadians, camera.rollRadians, camera.distance,
        camera.verticalFovDegrees, camera.nearPlane}) {
        if (!std::isfinite(value)) {
            throw std::runtime_error("Camera values must be finite.");
        }
    }
    // Leave headroom for squared distances and projection math in float precision.
    constexpr float maxCoordinate = 1.0e15f;
    for (auto& value : camera.target) { value = std::clamp(value, -maxCoordinate, maxCoordinate); }
    camera.distance = std::clamp(camera.distance, 0.001f, maxCoordinate);
    camera.pitchRadians = std::clamp(camera.pitchRadians, -pi * 0.5f, pi * 0.5f);
    camera.verticalFovDegrees = std::clamp(camera.verticalFovDegrees, 1.0f, 179.0f);
    // cameraFarPlane is at least max(distance, 10); keep a valid depth interval.
    camera.nearPlane = std::clamp(camera.nearPlane, 0.0001f, std::max(camera.distance, 10.0f) * 0.5f);
    return camera;
}

SceneCamera frameCameraBounds(const Bounds& bounds, SceneUpAxis upAxis)
{
    SceneCamera camera;
    camera.yawRadians = 0.48f;
    camera.pitchRadians = 0.28f;
    if (upAxis == SceneUpAxis::y) {
        camera.yawRadians = -0.48f;
    }

    return fitCameraBounds(camera, bounds);
}

SceneCamera fitCameraBounds(SceneCamera camera, const Bounds& bounds)
{
    camera = normalizedSceneCamera(camera);
    camera.target = bounds.center;
    const float radius = std::max(bounds.radius, 0.001f);
    const float halfFovRadians = camera.verticalFovDegrees * pi / 360.0f;
    camera.distance = std::max((radius / std::sin(halfFovRadians)) * 1.35f,
        radius + camera.nearPlane * 1.35f);
    return normalizedSceneCamera(camera);
}

SceneCamera cameraWithView(SceneCamera camera, CameraView view)
{
    // Z-up: front looks from -Y. Y-up: front looks from +Z.
    camera.yawRadians = -pi * 0.5f;
    camera.pitchRadians = 0.0f;
    camera.rollRadians = 0.0f;
    switch (view) {
    case CameraView::top: camera.pitchRadians = pi * 0.5f; break;
    case CameraView::bottom: camera.pitchRadians = -pi * 0.5f; break;
    case CameraView::front: break;
    case CameraView::back: camera.yawRadians = pi * 0.5f; break;
    case CameraView::left: camera.yawRadians = pi; break;
    case CameraView::right: camera.yawRadians = 0.0f; break;
    }
    return normalizedSceneCamera(camera);
}

float cameraViewportFov(const SceneCamera& camera, float aspectRatio)
{
    // Keep the camera field of view on the shorter canvas axis so framing a
    // bounding sphere remains valid with narrow canvases and open inspectors.
    const float aspect = std::isfinite(aspectRatio) ? std::clamp(aspectRatio, 0.0001f, 1.0f) : 1.0f;
    return 2.0f * std::atan(std::tan(camera.verticalFovDegrees * pi / 360.0f) / aspect) * 180.0f / pi;
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
    camera.pitchRadians = std::clamp(camera.pitchRadians + deltaY * sensitivity, -pi * 0.5f, pi * 0.5f);
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
    moveTarget(camera, rolledRightDirection(camera, upAxis), -deltaX * unitsPerPixel);
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
