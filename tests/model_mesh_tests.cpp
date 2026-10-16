#include "model_mesh.h"
#include "parallel_work.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
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

void serialSmoothNormals(std::vector<woby::Vertex>& vertices, const std::vector<uint32_t>& indices)
{
    for (auto& item : vertices) { item.normal = {0, 0, 0}; }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto normal = woby::calculateFaceNormal(vertices[indices[i]].position,
            vertices[indices[i + 1]].position, vertices[indices[i + 2]].position);
        for (size_t corner = 0; corner < 3; ++corner) {
            for (size_t axis = 0; axis < 3; ++axis) { vertices[indices[i + corner]].normal[axis] += normal[axis]; }
        }
    }
    for (auto& item : vertices) {
        auto& n = item.normal;
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (length <= 0.000001f || !std::isfinite(length)) { n = {0, 1, 0}; }
        else { n[0] /= length; n[1] /= length; n[2] /= length; }
    }
}

bool sameNormalBits(const std::vector<woby::Vertex>& a, const std::vector<woby::Vertex>& b)
{
    if (a.size() != b.size()) { return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::bit_cast<std::array<uint32_t, 3>>(a[i].normal)
            != std::bit_cast<std::array<uint32_t, 3>>(b[i].normal)) { return false; }
    }
    return true;
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

TEST_CASE("batched smooth normals retain serial bits across shared vertices and batch tails")
{
    std::vector<woby::Vertex> vertices(131101);
    uint32_t random = 34567;
    const auto next = [&] { random = random * 1664525u + 1013904223u; return random; };
    for (auto& item : vertices) {
        for (float& axis : item.position) { axis = static_cast<float>(static_cast<int>(next() % 10001) - 5000) / 31.0f; }
        item.normal = {7, 8, 9}; item.texcoord = {0.25f, 0.75f};
    }
    vertices[1].position = {-0.0f, 0, 0};
    vertices[2].position = {std::numeric_limits<float>::infinity(), 0, 0};
    vertices[3].position = {std::numeric_limits<float>::quiet_NaN(), 0, 0};
    vertices[4].position = {1e30f, 1e30f, 1e30f};
    vertices[5].position = {1e-30f, 1e-30f, 1e-30f};
    std::vector<uint32_t> indices;
    for (size_t face = 0; face < 1048591; ++face) {
        // A high-valence hub exposes changes to summation order. Repeated corners,
        // invalid face lengths, reversed faces and disconnected vertices coexist.
        const auto a = next() % 131099u, b = next() % 131099u;
        indices.insert(indices.end(), {face % 7 ? 0u : a, a, face % 11 ? b : a});
    }
    indices.insert(indices.end(), {1, 2, 3, 4, 5, 1, 0, 1}); // Ignore the incomplete last face.
    auto expected = vertices;
    serialSmoothNormals(expected, indices);
    const auto caller = std::this_thread::get_id();
    size_t previous = 0, calls = 0;
    const size_t total = vertices.size() * 2 + indices.size();
    woby::generateSmoothNormals(vertices, indices, [&](const auto& event) {
        CHECK(std::this_thread::get_id() == caller);
        CHECK(event.stage == woby::ModelLoadStage::normals);
        CHECK(event.total == total);
        CHECK(event.completed >= previous);
        CHECK(event.completed <= total);
        previous = event.completed; ++calls;
    });
    CHECK(previous == total);
    CHECK(calls > 3);
    CHECK(sameNormalBits(vertices, expected));
    CHECK(woby::analysisWorkerCount.load() == 0);
    bool unchanged = true;
    for (size_t i = 0; i < vertices.size(); ++i) {
        unchanged &= std::bit_cast<std::array<uint32_t, 3>>(vertices[i].position)
            == std::bit_cast<std::array<uint32_t, 3>>(expected[i].position);
        unchanged &= vertices[i].texcoord == expected[i].texcoord;
    }
    CHECK(unchanged);
}

TEST_CASE("smooth normals join helpers on callback failure and recover")
{
    std::vector<woby::Vertex> vertices(262147, vertex(1, 2, 3));
    std::vector<uint32_t> indices(786441);
    for (size_t i = 0; i < indices.size(); ++i) { indices[i] = static_cast<uint32_t>(i % vertices.size()); }
    size_t cancelAt = 1;
    SUBCASE("clear") { cancelAt = 1; }
    SUBCASE("face calculation") { cancelAt = vertices.size() + 1; }
    SUBCASE("final normalization") { cancelAt = vertices.size() + indices.size() + 1; }
    CHECK_THROWS_WITH(woby::generateSmoothNormals(vertices, indices, [&](const auto& event) {
        if (event.completed >= cancelAt) { throw std::runtime_error("stop normals"); }
    }), "stop normals");
    CHECK(woby::analysisWorkerCount.load() == 0);
    woby::generateSmoothNormals(vertices, indices);
    auto expected = vertices;
    serialSmoothNormals(expected, indices);
    CHECK(sameNormalBits(vertices, expected));
}

TEST_CASE("smooth normal batch and worker thresholds retain serial results")
{
    for (size_t count : {16383u, 16384u, 16385u, 131071u, 131072u, 131073u}) {
        CAPTURE(count);
        std::vector<woby::Vertex> vertices = {vertex(0, 0, 0), vertex(1, 0, 0), vertex(0, 1, 0),
            vertex(0, 0, -1), vertex(0.123f, 0.456f, 0.789f), vertex(4, 5, 6)};
        std::vector<uint32_t> indices;
        for (size_t i = 0; i < count; ++i) {
            indices.insert(indices.end(), {0, static_cast<uint32_t>(i % 4 + 1), static_cast<uint32_t>((i + 1) % 4 + 1)});
        }
        auto expected = vertices;
        serialSmoothNormals(expected, indices);
        woby::generateSmoothNormals(vertices, indices);
        CHECK(sameNormalBits(vertices, expected));
        CHECK(woby::analysisWorkerCount.load() == 0);
    }
}

TEST_CASE("smooth normals handle empty faces and equal face weights")
{
    std::vector<woby::Vertex> vertices = {vertex(0, 0, 0), vertex(100, 0, 0), vertex(0, 100, 0),
        vertex(0, 0, 1), vertex(0, 1, 0), vertex(2, 3, 4)};
    std::vector<uint32_t> indices;
    SUBCASE("no triangles") {}
    SUBCASE("different areas have equal weight") { indices = {0, 1, 2, 0, 4, 3}; }
    SUBCASE("opposite faces cancel") { indices = {0, 1, 2, 0, 2, 1}; }
    SUBCASE("incomplete face") { indices = {0, 1}; }
    auto expected = vertices;
    serialSmoothNormals(expected, indices);
    woby::generateSmoothNormals(vertices, indices);
    CHECK(sameNormalBits(vertices, expected));
    vertices.clear();
    woby::generateSmoothNormals(vertices, {});
    CHECK(vertices.empty());
}

TEST_CASE("mesh finalization retains duplicate vertices and triangle indices")
{
    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices = {
        vertex(0.0f, 0.0f, 0.0f),
        vertex(1.0f, 0.0f, 0.0f),
        vertex(0.0f, 1.0f, 0.0f),
        vertex(0.0f, 0.0f, 0.0f),
    };
    editMeshGeometry(mesh).indices = {0u, 1u, 2u, 3u, 1u, 2u};
    const auto positions = meshGeometry(mesh).vertices;
    bool generateNormals = false;
    SUBCASE("supplied normals") {
        for (auto& item : editMeshGeometry(mesh).vertices) { item.normal = {0, 0, 1}; }
    }
    SUBCASE("generated normals") { generateNormals = true; }

    woby::finalizeMesh(mesh, generateNormals);

    REQUIRE(meshGeometry(mesh).vertices.size() == 4u);
    CHECK(meshGeometry(mesh).indices == std::vector<uint32_t>{0u, 1u, 2u, 3u, 1u, 2u});
    for (size_t i = 0; i < meshGeometry(mesh).vertices.size(); ++i) {
        CHECK(meshGeometry(mesh).vertices[i].position == positions[i].position);
        CHECK(meshGeometry(mesh).vertices[i].normal == std::array<float, 3>{0, 0, 1});
    }
}

TEST_CASE("finalizing a mesh rejects empty input and fills derived data")
{
    woby::Mesh emptyMesh;
    CHECK_THROWS_WITH_AS(
        woby::finalizeMesh(emptyMesh, true),
        "Mesh did not contain renderable triangles, lines or points.",
        std::runtime_error);

    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices = {
        vertex(0.0f, 0.0f, 0.0f),
        vertex(1.0f, 0.0f, 0.0f),
        vertex(0.0f, 1.0f, 0.0f),
    };
    editMeshGeometry(mesh).indices = {0u, 1u, 2u};

    woby::finalizeMesh(mesh, true);

    CHECK(mesh.bounds.max[0] == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max[1] == doctest::Approx(1.0f));
    REQUIRE(meshGeometry(mesh).vertices.size() == 3u);
    for (const auto& item : meshGeometry(mesh).vertices) {
        CHECK(woby::validNormal(item.normal));
    }
}

TEST_CASE("mesh finalization retains sparse vertex IDs and unreferenced vertices")
{
    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices.resize(40);
    editMeshGeometry(mesh).vertices[30] = vertex(0, 1, 0);
    editMeshGeometry(mesh).vertices[25] = vertex(1, 0, 0);
    editMeshGeometry(mesh).vertices[15] = vertex(0, 0, 0);
    editMeshGeometry(mesh).vertices[39] = meshGeometry(mesh).vertices[30];
    editMeshGeometry(mesh).vertices[0] = vertex(-2, -3, -4);
    editMeshGeometry(mesh).indices = {30, 25, 15, 39, 15, 25};
    const auto source = meshGeometry(mesh).vertices;
    woby::captureSourceMesh(mesh, woby::SourceProvenance::importerVertices);
    const auto provenance = mesh.sourceData;

    woby::finalizeMesh(mesh, false);

    REQUIRE(meshGeometry(mesh).vertices.size() == source.size());
    CHECK(meshGeometry(mesh).indices == std::vector<uint32_t>{30, 25, 15, 39, 15, 25});
    CHECK(mesh.sourceData == provenance);
    CHECK(mesh.sourceData->indices == meshGeometry(mesh).indices);
    CHECK(mesh.bounds.min == std::array<float, 3>{-2, -3, -4});
    for (size_t i = 0; i < meshGeometry(mesh).vertices.size(); ++i) {
        CHECK(meshGeometry(mesh).vertices[i].position == source[i].position);
        CHECK(meshGeometry(mesh).vertices[i].normal == source[i].normal);
        CHECK(meshGeometry(mesh).vertices[i].texcoord == source[i].texcoord);
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
