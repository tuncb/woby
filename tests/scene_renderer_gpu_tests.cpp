#include "scene_renderer.h"
#include "graphics_helpers.h"

#include <doctest/doctest.h>
#include <cstring>

namespace {

struct RendererFixture {
    bool initialized = false;
    woby::GpuMesh mesh;

    ~RendererFixture()
    {
        if (initialized) {
            woby::destroyGpuMesh(mesh);
            woby::graphics::shutdown();
        }
    }
};

} // namespace

TEST_CASE("Staged native GPU uploads preserve complete geometry across frames and source destruction")
{
    namespace g = woby::graphics;
    RendererFixture fixture;
    fixture.initialized = g::init({});
    REQUIRE(fixture.initialized);
    woby::Mesh source;
    source.vertices.resize(7);
    for (size_t i = 0; i < source.vertices.size(); ++i) {
        source.vertices[i].position = {static_cast<float>(i), -2.0f, 3.0f};
    }
    source.indices = {2, 0, 1, 1, 3, 2};
    source.lineIndices = {4, 6};
    source.pointIndices = {5, 5, 6};
    source.nodes = {{"triangles", 0, 6}, {"lines"}, {"points"}};
    source.nodes[1].lineIndexCount = 2;
    source.nodes[2].pointIndexCount = 3;
    const auto expected = source;
    auto upload = woby::beginGpuMeshUpload(*woby::prepareSceneMesh(source, woby::gpuMeshEdges | woby::gpuMeshPoints));
    while (!woby::stepGpuMeshUpload(upload, source, woby::meshVertexLayout(), 64)) { g::frame(); }
    fixture.mesh = std::move(upload.mesh);
    source = {}; // Including the last chunk, staging must own bytes before frame().
    upload = {};
    std::vector<woby::Vertex> vertices(expected.vertices.size());
    std::vector<uint32_t> triangles(expected.indices.size()), lines(expected.lineIndices.size());
    std::vector<uint32_t> edges(expected.indices.size() * 2), points(fixture.mesh.pointVertexIndices.size());
    g::readBuffer(fixture.mesh.vertexBuffer, vertices.data());
    g::readBuffer(fixture.mesh.triangleIndexBuffer, triangles.data());
    g::readBuffer(fixture.mesh.importedLineBuffer, lines.data());
    g::readBuffer(fixture.mesh.lineIndexBuffer, edges.data());
    const auto ready = g::readBuffer(fixture.mesh.pointIdBuffer, points.data());
    while (g::frame() < ready) {}
    CHECK(std::memcmp(vertices.data(), expected.vertices.data(), vertices.size() * sizeof(woby::Vertex)) == 0);
    CHECK(triangles == expected.indices);
    CHECK(lines == expected.lineIndices);
    CHECK(edges == std::vector<uint32_t>{2, 0, 0, 1, 1, 2, 1, 3, 3, 2, 2, 1});
    CHECK(points == fixture.mesh.pointVertexIndices);

    upload = woby::beginGpuMeshUpload(*woby::prepareSceneMesh(expected));
    CHECK_FALSE(woby::stepGpuMeshUpload(upload, expected, woby::meshVertexLayout(), 32));
    woby::abortGpuMeshUpload(upload); // Destroy before queued GPU copy has been submitted.
    for (int i = 0; i < 4; ++i) { g::frame(); }
    CHECK_NOTHROW(g::setVertexBuffer(0, fixture.mesh.vertexBuffer)); // Existing scene still usable.
}
