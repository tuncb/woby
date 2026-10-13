#include "coordinate_simd.h"
#include "hash_utils.h"
#include "model_mesh.h"
#include "simd_bounds.h"
#include "ui_state.h"
#include <doctest/doctest.h>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
template<class T> void checkBits(T actual, T expected)
{
    CHECK(std::bit_cast<std::array<unsigned char, sizeof(T)>>(actual)
        == std::bit_cast<std::array<unsigned char, sizeof(T)>>(expected));
}

template<class T, class Reduction> void checkReduction(const std::vector<std::array<T, 3>>& points, Reduction reduction)
{
    std::array<std::array<T, 3>, 2> expected{points.front(), points.front()};
    for (const auto& point : points) {
        for (size_t k = 0; k < 3; ++k) {
            expected[0][k] = std::min(expected[0][k], point[k]);
            expected[1][k] = std::max(expected[1][k], point[k]);
        }
        woby::expand(reduction, point);
    }
    const auto actual = woby::boundsResult(reduction);
    for (size_t side = 0; side < 2; ++side) {
        for (size_t k = 0; k < 3; ++k) { checkBits(actual[side][k], expected[side][k]); }
    }
}
}

TEST_CASE("SIMD bounds retain scalar comparison semantics on all three axes")
{
    std::vector<woby::Coordinate> points;
    for (int i = 0; i < 4099; ++i) {
        points.push_back({double((i * 37) % 1009) - 500.25, double((i * 71) % 997) - 499.5, double(i % 31) - 16.75});
    }
    SUBCASE("finite values") {}
    SUBCASE("equal signed zeros") {
        points.assign(17, {0.0, -0.0, 0.0});
        points[0] = {-0.0, 0.0, -0.0};
    }
    SUBCASE("infinities and later NaNs") {
        points[1] = {INFINITY, -INFINITY, std::numeric_limits<double>::quiet_NaN()};
        points.back() = {std::numeric_limits<double>::quiet_NaN(), INFINITY, -INFINITY};
    }
    SUBCASE("first NaN is retained") { points[0][1] = std::numeric_limits<double>::quiet_NaN(); }
    checkReduction(points, woby::doubleBoundsReduction(points.front()));
    std::vector<std::array<float, 3>> floats;
    for (const auto& point : points) {
        floats.push_back({static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])});
    }
    checkReduction(floats, woby::floatBoundsReduction(floats.front(), floats.front()));
}

TEST_CASE("SIMD coordinate rebasing handles every tail and leaves adjacent points intact")
{
    for (size_t count = 0; count < 35; ++count) {
        CAPTURE(count);
        std::vector<woby::Coordinate> input(count + 2);
        for (size_t i = 0; i < input.size(); ++i) {
            input[i] = {8000000.0 + double(i) * .125, -9000000.0 + double(i) * .25, double(i) - 17};
        }
        const woby::Coordinate origin{8000000.0, -9000000.0, -0.0};
        auto expected = input;
        for (size_t i = 1; i <= count; ++i) {
            for (size_t k = 0; k < 3; ++k) { expected[i][k] -= origin[k]; }
        }
        const auto check = [&](const auto& actual) {
            for (size_t i = 0; i < actual.size(); ++i) {
                for (size_t k = 0; k < 3; ++k) { checkBits(actual[i][k], expected[i][k]); }
            }
        };
        auto native = input;
        woby::rebaseCoordinatesNative(std::span(native).subspan(1, count), origin);
        check(native);
        auto dispatched = input;
        woby::rebaseCoordinates(std::span(dispatched).subspan(1, count), origin);
        check(dispatched);
#ifdef WOBY_ENABLE_AVX512_REBASE
        if (woby::avx512CoordinateRebaseAvailable()) {
            auto wide = input;
            woby::rebaseCoordinatesAvx512(std::span(wide).subspan(1, count), origin);
            check(wide);
        }
#endif
    }
}

TEST_CASE("coordinate rebasing preserves signed zero and precise subtraction")
{
    for (const woby::Coordinate origin : {woby::Coordinate{0.0, -0.0, 1e16}, {1e16, -1e16, 0.0}}) {
        std::vector<woby::Coordinate> points(19, {-0.0, 0.0, 1e16 + 2.0});
        auto expected = points;
        for (auto& point : expected) {
            for (size_t k = 0; k < 3; ++k) { point[k] -= origin[k]; }
        }
        woby::rebaseCoordinates(points, origin);
        for (size_t i = 0; i < points.size(); ++i) {
            for (size_t k = 0; k < 3; ++k) { checkBits(points[i][k], expected[i][k]); }
        }
    }
}

TEST_CASE("AVX-512 dispatch requires CPU features and all OS vector state")
{
    constexpr uint32_t leaf1 = (1u << 26) | (1u << 27) | (1u << 28);
    constexpr uint32_t leaf7 = (1u << 5) | (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31);
    CHECK(woby::avx512RebaseFeatures(leaf1, leaf7, 0xe6));
    for (unsigned bit = 0; bit < 32; ++bit) {
        const auto mask = 1u << bit;
        if (leaf1 & mask) { CHECK_FALSE(woby::avx512RebaseFeatures(leaf1 & ~mask, leaf7, 0xe6)); }
        if (leaf7 & mask) { CHECK_FALSE(woby::avx512RebaseFeatures(leaf1, leaf7 & ~mask, 0xe6)); }
        if (uint64_t{0xe6} & mask) { CHECK_FALSE(woby::avx512RebaseFeatures(leaf1, leaf7, 0xe6 & ~uint64_t{mask})); }
    }
    CHECK_FALSE(woby::avx512RebaseFeatures(leaf1, leaf7, 0x6));
}

TEST_CASE("SIMD original bounds support every position storage and primitive range")
{
    woby::Mesh mesh;
    auto& geometry = woby::editMeshGeometry(mesh);
    geometry.vertices.resize(7);
    std::vector<woby::Coordinate> points{{-2, 4, 1}, {3, -8, 5}, {1, 2, -9}, {7, 0, 2},
        {-10, 3, 4}, {99, 99, 99}, {0, 0, 0}};
    for (size_t i = 0; i < points.size(); ++i) { geometry.vertices[i].position = woby::renderPosition(points[i]); }
    mesh.origin = {8000000, -9000000, 7000000};
    SUBCASE("render floats") {}
    SUBCASE("direct doubles") { geometry.precisePositions = points; }
    SUBCASE("identity source") { geometry.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(points); }
    SUBCASE("mapped source with seams") {
        geometry.sourcePositions = std::make_shared<const std::vector<woby::Coordinate>>(points);
        geometry.positionIndexing = woby::PositionIndexing::mapped;
        geometry.positionIndices = {4, 3, 2, 1, 0, 6, 4};
    }
    geometry.indices = {5, 4, 3, 1, 0, 2, 3, 5, 6};
    geometry.lineIndices = {6, 3, 2, 0};
    geometry.pointIndices = {4, 2, 1};
    woby::MeshNode triangle, line, point;
    triangle.indexOffset = 2; triangle.indexCount = 5;
    line.lineIndexOffset = 1; line.lineIndexCount = 3;
    point.pointIndexOffset = 1; point.pointIndexCount = 2;
    for (const auto* node : std::array<const woby::MeshNode*, 4>{nullptr, &triangle, &line, &point}) {
        const auto indices = node ? woby::meshNodeIndices(mesh, *node) : std::span<const uint32_t>{};
        const size_t count = node ? indices.size() : geometry.vertices.size();
        std::array<woby::Coordinate, 2> expected{};
        for (size_t i = 0; i < count; ++i) {
            const auto p = woby::originalPosition(woby::meshPosition(mesh, node ? indices[i] : i), mesh.origin);
            if (!i) { expected = {p, p}; }
            else { for (size_t k = 0; k < 3; ++k) {
                expected[0][k] = std::min(expected[0][k], p[k]);
                expected[1][k] = std::max(expected[1][k], p[k]);
            } }
        }
        CHECK(woby::originalMeshBounds(mesh, node) == expected);
    }
    woby::MeshNode empty;
    CHECK(woby::originalMeshBounds(mesh, &empty) == std::array<woby::Coordinate, 2>{});
    geometry.indices[2] = 100;
    CHECK_THROWS_AS((void)woby::originalMeshBounds(mesh, &triangle), std::out_of_range);
}

TEST_CASE("SIMD bounds preserve the mesh radius and progress cancellation")
{
    std::vector<woby::Vertex> vertices(16385);
    for (size_t i = 0; i < vertices.size(); ++i) {
        vertices[i].position = {float(i % 101) - 50, float(i % 71) - 35, float(i % 43) - 21};
    }
    std::vector<size_t> completed;
    const auto bounds = woby::calculateBounds(vertices, [&](const woby::ModelLoadProgress& progress) {
        CHECK(progress.stage == woby::ModelLoadStage::bounds);
        CHECK(progress.total == vertices.size() * 2);
        completed.push_back(progress.completed);
    });
    CHECK(completed == std::vector<size_t>{0, 16384, 32768, 32770});
    float squared = 0;
    for (const auto& vertex : vertices) {
        const float x = vertex.position[0] - bounds.center[0], y = vertex.position[1] - bounds.center[1],
            z = vertex.position[2] - bounds.center[2];
        squared = std::max(squared, x*x + y*y + z*z);
    }
    checkBits(bounds.radius, std::max(std::sqrt(squared), .001f));
    CHECK_THROWS_AS((void)woby::calculateBounds(vertices, [](const woby::ModelLoadProgress& progress) {
        if (progress.completed == 16384) { throw std::runtime_error("canceled"); }
    }), std::runtime_error);
}

TEST_CASE("coordinate origin still rejects non-finite values on every axis")
{
    CHECK(woby::coordinateOrigin({}) == woby::Coordinate{});
    for (size_t axis = 0; axis < 3; ++axis) {
        for (size_t index : {size_t{0}, size_t{16}}) {
            for (double invalid : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                std::numeric_limits<double>::quiet_NaN()}) {
                std::vector<woby::Coordinate> points(17, {8000000, -9000000, 7000000});
                points[index][axis] = invalid;
                CHECK_THROWS_AS((void)woby::coordinateOrigin(points), std::invalid_argument);
            }
        }
    }
}

TEST_CASE("SIMD group bounds retain the box radius instead of the mesh point radius")
{
    woby::Mesh mesh;
    auto& geometry = woby::editMeshGeometry(mesh);
    for (const std::array<float, 3> p : {std::array<float, 3>{-1, 0, 0}, {1, 0, 0},
        {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}}) {
        woby::Vertex vertex; vertex.position = p; geometry.vertices.push_back(vertex);
    }
    geometry.indices = {0, 1, 2, 3, 4, 5};
    woby::MeshNode node; node.indexCount = 6; geometry.nodes.push_back(node);
    woby::finalizeMesh(mesh, false);
    const auto groups = woby::createUiGroupStates(mesh, 0);
    REQUIRE(groups.size() == 1);
    CHECK(mesh.bounds.radius == 1.0f);
    CHECK(groups[0].localBounds.min == mesh.bounds.min);
    CHECK(groups[0].localBounds.max == mesh.bounds.max);
    CHECK(groups[0].localBounds.radius == std::sqrt(3.0f));
}

TEST_CASE("AVX2 double hashing retains zero normalization for scene caches")
{
    uint64_t positive = 123, negative = positive, subnormal = positive;
    woby::hashDouble(positive, 0.0);
    woby::hashDouble(negative, -0.0);
    woby::hashDouble(subnormal, std::numeric_limits<double>::denorm_min());
    CHECK(positive == negative);
    CHECK(positive != subnormal);
}
