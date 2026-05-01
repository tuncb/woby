#include "model_mesh.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

woby::Vertex vertex(float x, float y, float z)
{
    woby::Vertex result;
    result.position = {x, y, z};
    return result;
}

void calculateEmptyBounds()
{
    (void)woby::calculateBounds({});
}

} // namespace

TEST_CASE("face normals are deterministic for regular and degenerate triangles")
{
    const auto normal = woby::calculateFaceNormal(
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f});

    CHECK(normal[0] == doctest::Approx(0.0f));
    CHECK(normal[1] == doctest::Approx(0.0f));
    CHECK(normal[2] == doctest::Approx(1.0f));

    const auto degenerate = woby::calculateFaceNormal(
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {2.0f, 2.0f, 2.0f});

    CHECK(degenerate[0] == doctest::Approx(0.0f));
    CHECK(degenerate[1] == doctest::Approx(1.0f));
    CHECK(degenerate[2] == doctest::Approx(0.0f));
}

TEST_CASE("mesh bounds include all finite vertex positions")
{
    const std::vector<woby::Vertex> vertices = {
        vertex(-1.0f, 2.0f, 0.0f),
        vertex(3.0f, -2.0f, 4.0f),
        vertex(1.0f, 0.0f, -2.0f),
    };

    const woby::Bounds bounds = woby::calculateBounds(vertices);

    CHECK(bounds.min[0] == doctest::Approx(-1.0f));
    CHECK(bounds.min[1] == doctest::Approx(-2.0f));
    CHECK(bounds.min[2] == doctest::Approx(-2.0f));
    CHECK(bounds.max[0] == doctest::Approx(3.0f));
    CHECK(bounds.max[1] == doctest::Approx(2.0f));
    CHECK(bounds.max[2] == doctest::Approx(4.0f));
    CHECK(bounds.center[0] == doctest::Approx(1.0f));
    CHECK(bounds.center[1] == doctest::Approx(0.0f));
    CHECK(bounds.center[2] == doctest::Approx(1.0f));
    CHECK(bounds.radius == doctest::Approx(std::sqrt(17.0f)));

    CHECK_THROWS_WITH_AS(
        calculateEmptyBounds(),
        "Cannot calculate bounds for an empty mesh.",
        std::runtime_error);
}

TEST_CASE("smooth normal generation fills missing vertex normals")
{
    std::vector<woby::Vertex> vertices = {
        vertex(0.0f, 0.0f, 0.0f),
        vertex(1.0f, 0.0f, 0.0f),
        vertex(1.0f, 1.0f, 0.0f),
        vertex(0.0f, 1.0f, 0.0f),
    };
    const std::vector<uint32_t> indices = {0u, 1u, 2u, 0u, 2u, 3u};

    woby::generateSmoothNormals(vertices, indices);

    for (const auto& item : vertices) {
        CHECK(item.normal[0] == doctest::Approx(0.0f));
        CHECK(item.normal[1] == doctest::Approx(0.0f));
        CHECK(item.normal[2] == doctest::Approx(1.0f));
    }
}

TEST_CASE("mesh compaction removes duplicate vertices and remaps indices")
{
    std::vector<woby::Vertex> vertices = {
        vertex(0.0f, 0.0f, 0.0f),
        vertex(1.0f, 0.0f, 0.0f),
        vertex(0.0f, 1.0f, 0.0f),
        vertex(0.0f, 0.0f, 0.0f),
    };
    std::vector<uint32_t> indices = {0u, 1u, 2u, 3u, 1u, 2u};

    woby::compactMesh(vertices, indices);

    CHECK(vertices.size() == 3u);
    REQUIRE(indices.size() == 6u);
    for (const uint32_t index : indices) {
        CHECK(index < vertices.size());
    }
}

TEST_CASE("finalizing a mesh rejects empty input and fills derived data")
{
    woby::Mesh emptyMesh;
    CHECK_THROWS_WITH_AS(
        woby::finalizeMesh(emptyMesh, true),
        "Mesh did not contain renderable triangles.",
        std::runtime_error);

    woby::Mesh mesh;
    mesh.vertices = {
        vertex(0.0f, 0.0f, 0.0f),
        vertex(1.0f, 0.0f, 0.0f),
        vertex(0.0f, 1.0f, 0.0f),
    };
    mesh.indices = {0u, 1u, 2u};

    woby::finalizeMesh(mesh, true);

    CHECK(mesh.bounds.max[0] == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max[1] == doctest::Approx(1.0f));
    REQUIRE(mesh.vertices.size() == 3u);
    for (const auto& item : mesh.vertices) {
        CHECK(woby::validNormal(item.normal));
    }
}
