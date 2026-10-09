#include "math_utils.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>

namespace {

void checkPoint(const woby::Vec3& actual, const woby::Vec3& expected)
{
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(2e-5).scale(1));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(2e-5).scale(1));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(2e-5).scale(1));
}

void checkIdentity(const woby::Mat4& matrix)
{
    const auto identity = woby::identityMatrix();
    for (size_t i = 0; i < matrix.size(); ++i) {
        CAPTURE(i);
        CHECK(matrix[i] == doctest::Approx(identity[i]).epsilon(2e-5).scale(1));
    }
}

woby::Vec3 clipPosition(const woby::Mat4& matrix, const woby::Vec3& point)
{
    const std::array<float, 4> input{point.x, point.y, point.z, 1};
    std::array<float, 4> clip{};
    woby::transformVector4(clip.data(), input.data(), matrix.data());
    return {clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3]};
}

} // namespace

TEST_CASE("math vectors retain orientation length and angle units")
{
    CHECK((woby::cross({1, 0, 0}, {0, 1, 0}) == woby::Vec3{0, 0, 1}));
    CHECK((woby::cross({0, 1, 0}, {1, 0, 0}) == woby::Vec3{0, 0, -1}));
    CHECK((woby::subtractVectors({5, 7, 9}, {2, 3, 4}) == woby::Vec3{3, 4, 5}));
    CHECK((woby::scaleVector({1, -2, 3}, -2) == woby::Vec3{-2, 4, -6}));
    CHECK(woby::vectorLength({3, 4, 12}) == doctest::Approx(13));
    CHECK(woby::radians(180) == doctest::Approx(3.141592653589793));
    CHECK(woby::radians(-90) == doctest::Approx(-1.5707963267948966));
}

TEST_CASE("math translation moves positions without moving direction vectors")
{
    auto matrix = woby::identityMatrix();
    checkPoint(woby::transformPoint({2, -3, 4}, matrix.data()), {2, -3, 4});
    woby::translationMatrix(matrix.data(), 10, -20, 30);
    checkPoint(woby::transformPoint({2, -3, 4}, matrix.data()), {12, -23, 34});
    std::array<float, 4> direction{2, -3, 4, 0};
    woby::transformVector4(direction.data(), direction.data(), matrix.data());
    CHECK((direction == std::array<float, 4>{2, -3, 4, 0}));
}

TEST_CASE("math composition preserves renderer order for noncommuting transforms")
{
    woby::Mat4 translation{}, scaling{};
    woby::translationMatrix(translation.data(), 10, 0, 0);
    woby::transformMatrix(scaling.data(), 2, 3, 4, 0, 0, 0, 0, 0, 0);
    const auto translateThenScale = woby::multiplyMatrices(translation, scaling);
    const auto scaleThenTranslate = woby::multiplyMatrices(scaling, translation);
    checkPoint(woby::transformPoint({1, 2, 3}, translateThenScale.data()), {22, 6, 12});
    checkPoint(woby::transformPoint({1, 2, 3}, scaleThenTranslate.data()), {12, 6, 12});
    auto leftAlias = translation, rightAlias = scaling;
    woby::multiplyMatrices(leftAlias.data(), leftAlias.data(), scaling.data());
    woby::multiplyMatrices(rightAlias.data(), translation.data(), rightAlias.data());
    CHECK(leftAlias == translateThenScale);
    CHECK(rightAlias == translateThenScale);
}

TEST_CASE("math scene transforms preserve left handed Y X Z rotation and nonuniform scale")
{
    woby::Mat4 transform{};
    const float quarterTurn = woby::radians(90);
    woby::transformMatrix(transform.data(), 2, 3, 4,
        quarterTurn, quarterTurn, quarterTurn, 5, -7, 11);
    checkPoint(woby::transformPoint({1, 2, 3}, transform.data()), {3, 5, 17});
    checkPoint(woby::transformPoint({0, 0, 0}, transform.data()), {5, -7, 11});
    woby::rotateZMatrix(transform.data(), quarterTurn);
    checkPoint(woby::transformPoint({1, 0, 0}, transform.data()), {0, -1, 0});
    CHECK(woby::vectorLength(woby::transformPoint({3, 4, 12}, transform.data())) == doctest::Approx(13));
}

TEST_CASE("math inverse restores translated rotated and scaled geometry including aliasing")
{
    woby::Mat4 transform{}, inverse{};
    woby::transformMatrix(transform.data(), .25f, 3, 4,
        woby::radians(23), woby::radians(-47), woby::radians(68), 10, -12, 16);
    woby::inverseMatrix(inverse.data(), transform.data());
    for (const auto& point : std::array<woby::Vec3, 4>{{{0, 0, 0}, {1, 2, 3}, {-9, 7, -5}, {40, -30, 20}}}) {
        checkPoint(woby::transformPoint(woby::transformPoint(point, transform.data()), inverse.data()), point);
    }
    checkIdentity(woby::multiplyMatrices(transform, inverse));
    checkIdentity(woby::multiplyMatrices(inverse, transform));
    auto aliased = transform;
    woby::inverseMatrix(aliased.data(), aliased.data());
    CHECK(aliased == inverse);
}

TEST_CASE("math look at places the eye at the origin and the target on positive Z")
{
    for (const auto& up : std::array<woby::Vec3, 2>{{{0, 1, 0}, {0, 0, 1}}}) {
        woby::Mat4 view{};
        const woby::Vec3 eye{3, -4, -5}, target{8, 2, 7};
        woby::lookAtMatrix(view.data(), eye, target, up);
        checkPoint(woby::transformPoint(eye, view.data()), {0, 0, 0});
        checkPoint(woby::transformPoint(target, view.data()), {0, 0, woby::vectorLength(woby::subtractVectors(target, eye))});
        woby::Mat4 inverse{};
        woby::inverseMatrix(inverse.data(), view.data());
        checkPoint(woby::transformPoint({0, 0, 0}, inverse.data()), eye);
        checkIdentity(woby::multiplyMatrices(view, inverse));
    }
}

TEST_CASE("math look at preserves the parallel up vector fallback")
{
    woby::Mat4 view{};
    woby::lookAtMatrix(view.data(), {0, 0, 0}, {0, 1, 0}, {0, 1, 0});
    checkPoint(woby::transformPoint({1, 0, 0}, view.data()), {-1, 0, 0});
    checkPoint(woby::transformPoint({0, 1, 0}, view.data()), {0, 0, 1});
    for (const float value : view) { CHECK(std::isfinite(value)); }
}

TEST_CASE("math perspective maps frustum boundaries for both depth ranges and reversed depth")
{
    constexpr float nearPlane = 2, farPlane = 20;
    for (const bool homogeneous : {false, true}) {
        for (const bool reversed : {false, true}) {
            woby::Mat4 projection{};
            woby::perspectiveMatrix(projection.data(), 90, 2,
                reversed ? farPlane : nearPlane, reversed ? nearPlane : farPlane, homogeneous);
            const float low = homogeneous ? -1.0f : 0.0f;
            checkPoint(clipPosition(projection, {4, 2, nearPlane}), {1, 1, reversed ? 1 : low});
            checkPoint(clipPosition(projection, {-40, -20, farPlane}), {-1, -1, reversed ? low : 1});
            woby::Mat4 inverse{};
            woby::inverseMatrix(inverse.data(), projection.data());
            checkIdentity(woby::multiplyMatrices(projection, inverse));
            checkPoint(clipPosition(inverse, clipPosition(projection, {1, -2, 7})), {1, -2, 7});
        }
    }
}

TEST_CASE("math reversed depth retains separation for distant layers and tiny near planes")
{
    woby::Mat4 projection{};
    woby::perspectiveMatrix(projection.data(), 60, 1, 200000, .0001f, false);
    CHECK(clipPosition(projection, {0, 0, .0001f}).z == doctest::Approx(1));
    CHECK(clipPosition(projection, {0, 0, 200000}).z == doctest::Approx(0).epsilon(1e-6));
    for (const float distance : {20000.0f, 33000.0f, 80000.0f}) {
        const float front = clipPosition(projection, {0, 0, distance}).z;
        const float back = clipPosition(projection, {0, 0, distance + .5f}).z;
        CHECK(front > back);
        CHECK(front - back > 8 * (std::nextafter(back, 1.0f) - back));
    }
}

TEST_CASE("math orthographic maps the view box and retains the screen offset")
{
    for (const bool homogeneous : {false, true}) {
        for (const bool reversed : {false, true}) {
            woby::Mat4 projection{};
            woby::orthographicMatrix(projection.data(), -4, 6, -2, 8,
                reversed ? 12.0f : 2.0f, reversed ? 2.0f : 12.0f, 0, homogeneous);
            const float low = homogeneous ? -1.0f : 0.0f;
            checkPoint(clipPosition(projection, {-4, -2, 2}), {-1, -1, reversed ? 1 : low});
            checkPoint(clipPosition(projection, {6, 8, 12}), {1, 1, reversed ? low : 1});
        }
    }
    woby::Mat4 projection{};
    woby::orthographicMatrix(projection.data(), -4, 6, -2, 8, 2, 12, .125f, false);
    checkPoint(clipPosition(projection, {1, 3, 7}), {.125f, 0, .5f});
}

TEST_CASE("math viewport projection retains absolute W behind the camera")
{
    woby::Mat4 projection{};
    woby::perspectiveMatrix(projection.data(), 90, 1, 1, 10, false);
    const auto front = woby::projectPoint({1, 2, 4}, projection.data());
    const auto behind = woby::projectPoint({1, 2, -4}, projection.data());
    CHECK(front.x == doctest::Approx(.25));
    CHECK(front.y == doctest::Approx(.5));
    CHECK(behind.x == doctest::Approx(.25));
    CHECK(behind.y == doctest::Approx(.5));
    CHECK(behind.z < 0);
}
