#include "math_utils.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace woby {
namespace {

float dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 normalized(const Vec3& value)
{
    return scaleVector(value, 1.0f / vectorLength(value));
}

double minorDeterminant(const float* matrix, size_t omittedRow, size_t omittedColumn)
{
    std::array<double, 9> minor{};
    size_t index = 0;
    for (size_t row = 0; row < 4; ++row) {
        if (row == omittedRow) { continue; }
        for (size_t column = 0; column < 4; ++column) {
            if (column != omittedColumn) { minor[index++] = matrix[column * 4 + row]; }
        }
    }
    return minor[0] * (minor[4] * minor[8] - minor[5] * minor[7])
        - minor[1] * (minor[3] * minor[8] - minor[5] * minor[6])
        + minor[2] * (minor[3] * minor[7] - minor[4] * minor[6]);
}

} // namespace

Vec3 subtractVectors(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 scaleVector(const Vec3& value, float scale)
{
    return {value.x * scale, value.y * scale, value.z * scale};
}

Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float vectorLength(const Vec3& value)
{
    return std::sqrt(dot(value, value));
}

Mat4 identityMatrix()
{
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

void identityMatrix(float* result)
{
    const auto matrix = identityMatrix();
    std::copy(matrix.begin(), matrix.end(), result);
}

void translationMatrix(float* result, float x, float y, float z)
{
    identityMatrix(result);
    result[12] = x;
    result[13] = y;
    result[14] = z;
}

void rotateZMatrix(float* result, float angleRadians)
{
    identityMatrix(result);
    const float sine = std::sin(angleRadians), cosine = std::cos(angleRadians);
    result[0] = result[5] = cosine;
    result[1] = -sine;
    result[4] = sine;
}

void transformMatrix(float* result, float sx, float sy, float sz,
    float ax, float ay, float az, float tx, float ty, float tz)
{
    // Left-handed rotations: Y * X * Z, followed by translation. Scale applies
    // to the input axes, so each rotation column uses its corresponding scale.
    const float sinX = std::sin(ax), cosX = std::cos(ax);
    const float sinY = std::sin(ay), cosY = std::cos(ay);
    const float sinZ = std::sin(az), cosZ = std::cos(az);
    const float sinXsinZ = sinX * sinZ, cosYcosZ = cosY * cosZ;
    const Mat4 matrix{
        sx * (cosYcosZ - sinXsinZ * sinY), sx * -cosX * sinZ,
        sx * (cosZ * sinY + cosY * sinXsinZ), 0,
        sy * (cosZ * sinX * sinY + cosY * sinZ), sy * cosX * cosZ,
        sy * (sinY * sinZ - cosYcosZ * sinX), 0,
        sz * -cosX * sinY, sz * sinX, sz * cosX * cosY, 0,
        tx, ty, tz, 1,
    };
    std::copy(matrix.begin(), matrix.end(), result);
}

void transformVector4(float* result, const float* vector, const float* matrix)
{
    std::array<float, 4> transformed{};
    for (size_t row = 0; row < 4; ++row) {
        transformed[row] = vector[0] * matrix[row] + vector[1] * matrix[4 + row]
            + vector[2] * matrix[8 + row] + vector[3] * matrix[12 + row];
    }
    std::copy(transformed.begin(), transformed.end(), result);
}

Mat4 multiplyMatrices(const Mat4& first, const Mat4& second)
{
    Mat4 result{};
    multiplyMatrices(result.data(), first.data(), second.data());
    return result;
}

void multiplyMatrices(float* result, const float* first, const float* second)
{
    Mat4 product{};
    for (size_t column = 0; column < 4; ++column) {
        transformVector4(product.data() + column * 4, first + column * 4, second);
    }
    std::copy(product.begin(), product.end(), result);
}

void inverseMatrix(float* result, const float* matrix)
{
    // Double intermediates limit cancellation in camera and projection inverses.
    std::array<double, 16> cofactors{};
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row) {
            cofactors[column * 4 + row] = minorDeterminant(matrix, row, column)
                * ((row + column) % 2 == 0 ? 1.0 : -1.0);
        }
    }
    double determinant = 0;
    for (size_t column = 0; column < 4; ++column) {
        determinant += matrix[column * 4] * cofactors[column * 4];
    }
    Mat4 inverse{};
    for (size_t column = 0; column < 4; ++column) {
        for (size_t row = 0; row < 4; ++row) {
            inverse[column * 4 + row] = static_cast<float>(cofactors[row * 4 + column] / determinant);
        }
    }
    std::copy(inverse.begin(), inverse.end(), result);
}

void lookAtMatrix(float* result, const Vec3& eye, const Vec3& target, const Vec3& up)
{
    const auto forward = normalized(subtractVectors(target, eye));
    const auto perpendicular = cross(up, forward);
    const auto right = dot(perpendicular, perpendicular) == 0 ? Vec3{-1, 0, 0} : normalized(perpendicular);
    const auto cameraUp = cross(forward, right);
    const Mat4 matrix{
        right.x, cameraUp.x, forward.x, 0,
        right.y, cameraUp.y, forward.y, 0,
        right.z, cameraUp.z, forward.z, 0,
        -dot(right, eye), -dot(cameraUp, eye), -dot(forward, eye), 1,
    };
    std::copy(matrix.begin(), matrix.end(), result);
}

void perspectiveMatrix(float* result, float verticalFovDegrees, float aspect,
    float nearPlane, float farPlane, bool homogeneousDepth)
{
    const float height = 1.0f / std::tan(radians(verticalFovDegrees) * 0.5f);
    const float range = farPlane - nearPlane;
    const float depth = homogeneousDepth ? (farPlane + nearPlane) / range : farPlane / range;
    const float offset = homogeneousDepth ? (2.0f * farPlane * nearPlane) / range : nearPlane * depth;
    Mat4 matrix{};
    matrix[0] = height * (1.0f / aspect);
    matrix[5] = height;
    matrix[10] = depth;
    matrix[11] = 1;
    matrix[14] = -offset;
    std::copy(matrix.begin(), matrix.end(), result);
}

void orthographicMatrix(float* result, float left, float right, float bottom, float top,
    float nearPlane, float farPlane, float offset, bool homogeneousDepth)
{
    Mat4 matrix{};
    matrix[0] = 2.0f / (right - left);
    matrix[5] = 2.0f / (top - bottom);
    matrix[10] = (homogeneousDepth ? 2.0f : 1.0f) / (farPlane - nearPlane);
    matrix[12] = (left + right) / (left - right) + offset;
    matrix[13] = (top + bottom) / (bottom - top);
    matrix[14] = homogeneousDepth ? (nearPlane + farPlane) / (nearPlane - farPlane) : nearPlane / (nearPlane - farPlane);
    matrix[15] = 1;
    std::copy(matrix.begin(), matrix.end(), result);
}

Vec3 transformPoint(const Vec3& point, const float* matrix)
{
    return {
        point.x * matrix[0] + point.y * matrix[4] + point.z * matrix[8] + matrix[12],
        point.x * matrix[1] + point.y * matrix[5] + point.z * matrix[9] + matrix[13],
        point.x * matrix[2] + point.y * matrix[6] + point.z * matrix[10] + matrix[14],
    };
}

Vec3 projectPoint(const Vec3& point, const float* matrix)
{
    const auto transformed = transformPoint(point, matrix);
    const float w = point.x * matrix[3] + point.y * matrix[7] + point.z * matrix[11] + matrix[15];
    return scaleVector(transformed, 1.0f / std::abs(w));
}

} // namespace woby
