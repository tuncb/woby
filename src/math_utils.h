#pragma once

#include <array>
#include <numbers>

namespace woby {

struct Vec3 {
    float x = 0;
    float y = 0;
    float z = 0;

    friend bool operator==(const Vec3&, const Vec3&) = default;
};

using Mat4 = std::array<float, 16>;

[[nodiscard]] constexpr float radians(float degrees)
{
    return degrees * (std::numbers::pi_v<float> / 180.0f);
}

[[nodiscard]] Vec3 subtractVectors(const Vec3& a, const Vec3& b);
[[nodiscard]] Vec3 scaleVector(const Vec3& value, float scale);
[[nodiscard]] Vec3 cross(const Vec3& a, const Vec3& b);
[[nodiscard]] float vectorLength(const Vec3& value);

// Matrices are column-major; positive view-space Z is forward. Composition
// applies first, then second, matching the renderer and saved scene transforms.
// Pointer arguments refer to 16 floats (four for transformVector4's vectors).
// Output may alias input; all inputs are read before the output is written.
[[nodiscard]] Mat4 identityMatrix();
void identityMatrix(float* result);
void translationMatrix(float* result, float x, float y, float z);
void rotateZMatrix(float* result, float angleRadians);
void transformMatrix(float* result, float sx, float sy, float sz,
    float ax, float ay, float az, float tx, float ty, float tz);
// Apply first, then second (column-vector product second * first).
[[nodiscard]] Mat4 multiplyMatrices(const Mat4& first, const Mat4& second);
void multiplyMatrices(float* result, const float* first, const float* second);
void inverseMatrix(float* result, const float* matrix);
void lookAtMatrix(float* result, const Vec3& eye, const Vec3& target, const Vec3& up = {0, 1, 0});

// Homogeneous depth selects [-1, 1]; otherwise depth is [0, 1]. Reversed depth
// is built directly by exchanging the near and far distances.
void perspectiveMatrix(float* result, float verticalFovDegrees, float aspect,
    float nearPlane, float farPlane, bool homogeneousDepth);
void orthographicMatrix(float* result, float left, float right, float bottom, float top,
    float nearPlane, float farPlane, float offset, bool homogeneousDepth);

void transformVector4(float* result, const float* vector, const float* matrix);
[[nodiscard]] Vec3 transformPoint(const Vec3& point, const float* matrix);
// Retain the viewport helper's division by |w| for points behind the camera.
[[nodiscard]] Vec3 projectPoint(const Vec3& point, const float* matrix);

} // namespace woby
