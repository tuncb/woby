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
        woby::destroyGpuMesh(fixture.mesh);
    }
}
