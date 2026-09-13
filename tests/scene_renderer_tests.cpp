#include "scene_renderer.h"

#include <doctest/doctest.h>

namespace {

struct RendererFixture {
    bool initialized = false;
    woby::GpuMesh mesh;

    ~RendererFixture()
    {
        if (initialized) {
            woby::destroyGpuMesh(mesh);
            bgfx::shutdown();
        }
    }
};

} // namespace

TEST_CASE("GPU point ranges preserve first occurrence order independently for each group")
{
    RendererFixture fixture;
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 1;
    init.resolution.height = 1;
    fixture.initialized = bgfx::init(init);
    REQUIRE(fixture.initialized);

    woby::Mesh mesh;
    mesh.vertices.resize(8);
    mesh.indices = {6, 1, 6, 1, 3, 6, 3, 1, 7, 7, 1, 3};
    mesh.nodes = {{"first", 0, 6}, {"empty", 6, 0}, {"shared", 6, 6}, {"overlap", 0, 3}};

    for (int iteration = 0; iteration < 2; ++iteration) {
        fixture.mesh = woby::createGpuMesh(mesh, woby::meshVertexLayout(), woby::pointSpriteVertexLayout());
        CHECK(bgfx::isValid(fixture.mesh.vertexBuffer));
        CHECK(bgfx::isValid(fixture.mesh.triangleIndexBuffer));
        CHECK_FALSE(bgfx::isValid(fixture.mesh.lineIndexBuffer));
        CHECK_FALSE(bgfx::isValid(fixture.mesh.pointSpriteVertexBuffer));
        CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 3, 1, 7, 6, 1});
        REQUIRE(fixture.mesh.nodeRanges.size() == 4);
        CHECK(fixture.mesh.nodeRanges[0].pointIndexCount == 3);
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh, woby::pointSpriteVertexLayout(), woby::gpuMeshPoints);
        CHECK(bgfx::isValid(fixture.mesh.pointSpriteVertexBuffer));
        CHECK(bgfx::isValid(fixture.mesh.pointSpriteIndexBuffer));
        CHECK_FALSE(bgfx::isValid(fixture.mesh.lineIndexBuffer));
        CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 3, 1, 7, 6, 1});
        REQUIRE(fixture.mesh.nodeRanges.size() == 4u);
        const std::array<uint32_t, 4> offsets = {0, 3, 3, 6};
        const std::array<uint32_t, 4> counts = {3, 0, 3, 2};
        for (size_t i = 0; i < mesh.nodes.size(); ++i) {
            const auto& range = fixture.mesh.nodeRanges[i];
            CHECK(range.pointIndexOffset == offsets[i]);
            CHECK(range.pointIndexCount == counts[i]);
            CHECK(range.pointSpriteIndexOffset == offsets[i] * 6u);
            CHECK(range.pointSpriteIndexCount == counts[i] * 6u);
            CHECK(range.triangleIndexOffset == mesh.nodes[i].indexOffset);
            CHECK(range.triangleIndexCount == mesh.nodes[i].indexCount);
            CHECK(range.lineIndexOffset == mesh.nodes[i].indexOffset * 2u);
            CHECK(range.lineIndexCount == mesh.nodes[i].indexCount * 2u);
        }
        const auto points = fixture.mesh.pointSpriteVertexBuffer.idx;
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh, woby::pointSpriteVertexLayout(),
            woby::gpuMeshEdges | woby::gpuMeshPoints);
        REQUIRE(bgfx::isValid(fixture.mesh.lineIndexBuffer));
        const auto lines = fixture.mesh.lineIndexBuffer.idx;
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh, woby::pointSpriteVertexLayout(), 0);
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh, woby::pointSpriteVertexLayout(),
            woby::gpuMeshEdges | woby::gpuMeshPoints);
        CHECK(fixture.mesh.pointSpriteVertexBuffer.idx == points);
        CHECK(fixture.mesh.lineIndexBuffer.idx == lines);
        woby::destroyGpuMesh(fixture.mesh);
    }
}

TEST_CASE("GPU display demand follows visible file and part settings")
{
    woby::UiFileState file;
    file.groupSettings.resize(2);
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
    file.groupSettings[0].showVertices = true;
    file.groupSettings[1].showTriangles = true;
    CHECK(woby::requestedGpuMeshFeatures(file) == (woby::gpuMeshEdges | woby::gpuMeshPoints));
    file.groupSettings[0].visible = false;
    CHECK(woby::requestedGpuMeshFeatures(file) == woby::gpuMeshEdges);
    file.groupSettings[1].opacity = 0;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
    file.groupSettings[0].visible = true;
    CHECK(woby::requestedGpuMeshFeatures(file) == woby::gpuMeshPoints);
    file.fileSettings.visible = false;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
    file.fileSettings.visible = true;
    file.fileSettings.opacity = 0;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
}

TEST_CASE("GPU uploads reject invalid indices and ranges before allocating")
{
    woby::Mesh mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 99};
    mesh.nodes = {{"part", 0, 3}};
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout(), woby::pointSpriteVertexLayout()),
        "Scene contains an invalid vertex index.");
    mesh.indices[2] = 2;
    mesh.nodes[0].indexCount = 6;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout(), woby::pointSpriteVertexLayout()),
        "Scene contains an invalid triangle range.");
}
