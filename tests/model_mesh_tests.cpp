#include "model_mesh.h"
#include "parallel_work.h"

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

TEST_CASE("compaction preserves corners and first use order with sparse source vertices")
{
    std::vector<woby::Vertex> vertices(40);
    vertices[30] = vertex(0, 1, 0);
    vertices[25] = vertex(1, 0, 0);
    vertices[15] = vertex(0, 0, 0);
    vertices[39] = vertices[30];
    const auto source = vertices;
    std::vector<uint32_t> indices = {30, 25, 15, 39, 15, 25};
    const auto original = indices;
    woby::compactMesh(vertices, indices);
    REQUIRE(vertices.size() == 3);
    CHECK(indices == std::vector<uint32_t>{0, 1, 2, 0, 2, 1});
    for (size_t i = 0; i < indices.size(); ++i) {
        CHECK(vertices[indices[i]].position == source[original[i]].position);
        CHECK(vertices[indices[i]].normal == source[original[i]].normal);
    }
}

TEST_CASE("parallel analysis batches cover tails exactly once and recover from failure")
{
    std::vector<std::atomic<unsigned>> visits(16391);
    const auto run = [&] {
        woby::parallelAnalysisBatches(visits.size(), 31, {}, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) { visits[i].fetch_add(1, std::memory_order_relaxed); }
        });
    };
    run();
    for (const auto& value : visits) { CHECK(value.load() == 1); }
    CHECK_THROWS_WITH(woby::parallelAnalysisBatches(visits.size(), 128, {}, [](size_t begin, size_t) {
        if (begin == 0) { throw std::runtime_error("worker failure"); }
    }), "worker failure");
    CHECK(woby::analysisWorkerCount.load() == 0);
    run();
    for (const auto& value : visits) { CHECK(value.load() == 2); }
    std::stop_source stop;
    CHECK_THROWS_WITH(woby::parallelAnalysisBatches(visits.size(), 128, stop.get_token(), [&](size_t, size_t) {
        stop.request_stop();
    }), "Analysis canceled.");
    CHECK(woby::analysisWorkerCount.load() == 0);
}
