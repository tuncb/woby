#include "scene_renderer.h"
#include "graphics_helpers.h"

#include <doctest/doctest.h>
#include <limits>
#include <future>

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

TEST_CASE("Scene upload preparation is owned worker data with cancellable validation and range building")
{
    woby::Mesh mesh;
    mesh.vertices.resize(8);
    mesh.indices = {6, 1, 6, 1, 3, 6, 3, 1, 7};
    mesh.nodes = {{"first", 0, 6}, {"shared", 6, 3}, {"overlap", 0, 3}};
    auto worker = std::async(std::launch::async, [source = mesh] {
        return woby::prepareSceneMesh(source, woby::gpuMeshEdges | woby::gpuMeshPoints);
    });
    mesh.indices.clear(); // The result does not borrow this source or the worker snapshot.
    const auto prepared = worker.get();
    REQUIRE(prepared);
    CHECK(prepared->pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 3, 1, 7, 6, 1});
    CHECK(prepared->edgeIndices == std::vector<uint32_t>{6, 1, 1, 6, 6, 6, 1, 3, 3, 6, 6, 1, 3, 1, 1, 7, 7, 3});
    REQUIRE(prepared->nodeRanges.size() == 3);
    CHECK(prepared->nodeRanges[2].pointIndexOffset == 6);
    CHECK(prepared->nodeRanges[2].pointIndexCount == 2);

    mesh.indices.assign(300000, 0);
    mesh.nodes = {{"large", 0, 300000}};
    // Cancel in initial validation, range preparation, and optional edge generation.
    for (const size_t stop : {1u, 4u, 9u, 14u}) {
        size_t polls = 0;
        CHECK_FALSE(woby::prepareSceneMesh(mesh, woby::gpuMeshEdges, [&] { return ++polls == stop; }));
        CHECK(polls == stop);
    }
    CHECK(woby::prepareSceneMesh(mesh).has_value());
}

TEST_CASE("Staged mesh uploads bound bytes and complete every primitive buffer before publication")
{
    RendererFixture fixture;
    woby::graphics::Init init;
    init.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(init);
    REQUIRE(fixture.initialized);
    woby::Mesh mesh;
    mesh.vertices.resize(8);
    mesh.indices = {6, 1, 3, 3, 1, 7};
    mesh.lineIndices = {0, 2};
    mesh.pointIndices = {5, 4, 5};
    mesh.nodes = {{"triangles", 0, 6}, {"lines"}, {"points"}};
    mesh.nodes[1].lineIndexCount = 2;
    mesh.nodes[2].pointIndexCount = 3;
    auto prepared = woby::prepareSceneMesh(mesh, woby::gpuMeshEdges | woby::gpuMeshPoints);
    REQUIRE(prepared);
    auto upload = woby::beginGpuMeshUpload(std::move(*prepared));
    // Moving the source between steps is safe: no pointer is retained by upload.
    auto moved = std::move(mesh);
    bool complete = false;
    size_t steps = 0;
    while (!complete && steps < 100) {
        const auto previous = upload.uploadedBytes;
        complete = woby::stepGpuMeshUpload(upload, moved, woby::meshVertexLayout(), 32);
        CHECK(upload.uploadedBytes - previous <= 32);
        if (!complete) { CHECK(upload.bufferIndex < 5); }
        ++steps;
    }
    REQUIRE(complete);
    CHECK(steps > 1);
    CHECK(upload.uploadedBytes == 256 + 24 + 8 + 48 + 32);
    CHECK(upload.uploadedBytes == upload.totalBytes);
    fixture.mesh = std::move(upload.mesh);
    CHECK(woby::graphics::isValid(fixture.mesh.vertexBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.triangleIndexBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.importedLineBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.lineIndexBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.pointIdBuffer));
    CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 7, 0, 2, 5, 4});
}

TEST_CASE("Canceling a partial mesh upload destroys its buffers and allows a fresh upload")
{
    RendererFixture fixture;
    woby::graphics::Init init;
    init.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(init);
    REQUIRE(fixture.initialized);
    woby::Mesh mesh;
    mesh.vertices.resize(3);
    mesh.indices = {0, 1, 2};
    mesh.nodes = {{"triangle", 0, 3}};
    auto upload = woby::beginGpuMeshUpload(*woby::prepareSceneMesh(mesh));
    CHECK_FALSE(woby::stepGpuMeshUpload(upload, mesh, woby::meshVertexLayout(), 32));
    const auto canceledBuffer = upload.mesh.vertexBuffer;
    woby::abortGpuMeshUpload(upload);
    CHECK_FALSE(woby::graphics::isValid(upload.mesh.vertexBuffer));
    CHECK(upload.mesh.pointVertexIndices.empty());
    const std::array<uint32_t, 8> data{};
    CHECK_THROWS(woby::graphics::uploadBufferRange(canceledBuffer, 0, data.data(), 32));
    fixture.mesh = woby::createGpuMesh(mesh, woby::meshVertexLayout());
    CHECK(woby::graphics::isValid(fixture.mesh.triangleIndexBuffer));
    CHECK_THROWS(woby::graphics::uploadBufferRange(fixture.mesh.vertexBuffer, 92, data.data(), 8));
    CHECK_THROWS(woby::graphics::uploadBufferRange(fixture.mesh.vertexBuffer, 1, data.data(), 4));
    CHECK_THROWS(woby::graphics::uploadBufferRange(fixture.mesh.vertexBuffer, 0, nullptr, 4));
    CHECK_THROWS(woby::graphics::createVertexBufferStorage(31, woby::meshVertexLayout()));
}

TEST_CASE("GPU point ranges preserve first occurrence order independently for each group")
{
    RendererFixture fixture;
    woby::graphics::Init init;
    init.type = woby::graphics::RendererType::Noop;
    init.resolution.width = 1;
    init.resolution.height = 1;
    fixture.initialized = woby::graphics::init(init);
    REQUIRE(fixture.initialized);

    woby::Mesh mesh;
    mesh.vertices.resize(8);
    mesh.indices = {6, 1, 6, 1, 3, 6, 3, 1, 7, 7, 1, 3};
    mesh.nodes = {{"first", 0, 6}, {"empty", 6, 0}, {"shared", 6, 6}, {"overlap", 0, 3}};

    for (int iteration = 0; iteration < 2; ++iteration) {
        fixture.mesh = woby::createGpuMesh(mesh, woby::meshVertexLayout());
        CHECK(woby::graphics::isValid(fixture.mesh.vertexBuffer));
        CHECK(woby::graphics::isValid(fixture.mesh.triangleIndexBuffer));
        CHECK_FALSE(woby::graphics::isValid(fixture.mesh.lineIndexBuffer));
        CHECK_FALSE(woby::graphics::isValid(fixture.mesh.pointIdBuffer));
        CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 3, 1, 7, 6, 1});
        REQUIRE(fixture.mesh.nodeRanges.size() == 4);
        CHECK(fixture.mesh.nodeRanges[0].pointIndexCount == 3);
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh, woby::gpuMeshPoints);
        CHECK(woby::graphics::isValid(fixture.mesh.pointIdBuffer));
        CHECK_FALSE(woby::graphics::isValid(fixture.mesh.lineIndexBuffer));
        CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 3, 1, 7, 6, 1});
        REQUIRE(fixture.mesh.nodeRanges.size() == 4u);
        const std::array<uint32_t, 4> offsets = {0, 3, 3, 6};
        const std::array<uint32_t, 4> counts = {3, 0, 3, 2};
        for (size_t i = 0; i < mesh.nodes.size(); ++i) {
            const auto& range = fixture.mesh.nodeRanges[i];
            CHECK(range.pointIndexOffset == offsets[i]);
            CHECK(range.pointIndexCount == counts[i]);
            CHECK(range.triangleIndexOffset == mesh.nodes[i].indexOffset);
            CHECK(range.triangleIndexCount == mesh.nodes[i].indexCount);
            CHECK(range.lineIndexOffset == mesh.nodes[i].indexOffset * 2u);
            CHECK(range.lineIndexCount == mesh.nodes[i].indexCount * 2u);
        }
        const auto points = fixture.mesh.pointIdBuffer.idx;
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh,
            woby::gpuMeshEdges | woby::gpuMeshPoints);
        REQUIRE(woby::graphics::isValid(fixture.mesh.lineIndexBuffer));
        const auto lines = fixture.mesh.lineIndexBuffer.idx;
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh, 0);
        woby::prepareGpuMeshFeatures(fixture.mesh, mesh,
            woby::gpuMeshEdges | woby::gpuMeshPoints);
        CHECK(fixture.mesh.pointIdBuffer.idx == points);
        CHECK(fixture.mesh.lineIndexBuffer.idx == lines);
        woby::destroyGpuMesh(fixture.mesh);
        CHECK_FALSE(woby::graphics::isValid(fixture.mesh.pointIdBuffer));
        CHECK(fixture.mesh.pointVertexIndices.empty());
        CHECK(fixture.mesh.nodeRanges.empty());
    }
}

TEST_CASE("Point uniforms preserve large offsets and pixel sizing")
{
    for (const uint32_t offset : {0u, 65535u, 65536u, 16777217u, 38414391u,
                                 std::numeric_limits<uint32_t>::max()}) {
        const auto params = woby::pointSpriteParameters(40.0f, 1280, 720, offset);
        CHECK(params[0] == 40.0f);
        CHECK(params[1] == 1280.0f);
        CHECK(params[2] == 720.0f);
        const auto restored = static_cast<uint32_t>(params[4])
            + static_cast<uint32_t>(params[5]) * 65536u;
        CHECK(restored == offset);
    }
    const auto minimized = woby::pointSpriteParameters(1.0f, 0, 0, 0);
    CHECK(minimized[1] == 1.0f);
    CHECK(minimized[2] == 1.0f);
}

TEST_CASE("Renderer requirements reject missing shader features at startup")
{
    woby::graphics::Caps caps{};
    caps.supported = WOBY_GPU_CAPS_COMPUTE | WOBY_GPU_CAPS_VERTEX_ID | WOBY_GPU_CAPS_INSTANCING
        | WOBY_GPU_CAPS_INDEX32 | WOBY_GPU_CAPS_PRIMITIVE_ID;
    caps.limits.maxComputeBindings = 2;
    for (const auto renderer : {woby::graphics::RendererType::Metal, woby::graphics::RendererType::Vulkan}) {
        caps.rendererType = renderer;
        CHECK(woby::unsupportedRendererReason(caps) == nullptr);
        CHECK_NOTHROW(woby::validateRendererCapabilities(caps));
        for (const auto feature : {WOBY_GPU_CAPS_COMPUTE, WOBY_GPU_CAPS_VERTEX_ID,
                                  WOBY_GPU_CAPS_INSTANCING, WOBY_GPU_CAPS_INDEX32}) {
            auto missing = caps;
            missing.supported &= ~feature;
            CHECK(woby::unsupportedRendererReason(missing) != nullptr);
            CHECK_THROWS_WITH_AS(woby::validateRendererCapabilities(missing),
                doctest::Contains("cannot run Woby"), std::runtime_error);
        }
        for (const uint32_t bindings : {0u, 1u}) {
            auto missing = caps;
            missing.limits.maxComputeBindings = bindings;
            CHECK_THROWS_WITH_AS(woby::validateRendererCapabilities(missing),
                doctest::Contains("two shader storage buffer bindings"), std::runtime_error);
        }
    }
    caps.rendererType = woby::graphics::RendererType::Noop;
    CHECK(woby::unsupportedRendererReason(caps) != nullptr);
}

TEST_CASE("Native shader folders match the supported backends")
{
    CHECK(std::string(woby::rendererShaderFolder(woby::graphics::RendererType::Vulkan)) == "spirv");
    CHECK(std::string(woby::rendererShaderFolder(woby::graphics::RendererType::Metal)) == "metal");
    CHECK_THROWS(woby::rendererShaderFolder(woby::graphics::RendererType::Noop));
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
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid vertex index.");
    mesh.indices[2] = 2;
    mesh.nodes[0].indexCount = 6;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid primitive range.");
    mesh.indices.clear();
    mesh.nodes[0].indexCount = 0;
    mesh.nodes[0].lineIndexCount = 2;
    mesh.lineIndices = {0, 99};
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid line vertex index.");
    mesh.lineIndices[1] = 1;
    mesh.nodes[0].lineIndexOffset = 1;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid primitive range.");
    mesh.nodes[0].lineIndexOffset = 0;
    mesh.nodes[0].lineIndexCount = 1;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid primitive range.");
    mesh.lineIndices.push_back(2);
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene needs valid triangles, line segments or points.");
}


TEST_CASE("GPU standalone points use point buffers without triangle or line buffers")
{
    RendererFixture fixture;
    woby::graphics::Init init; init.type = woby::graphics::RendererType::Noop;
    init.resolution.width = init.resolution.height = 1;
    fixture.initialized = woby::graphics::init(init);
    REQUIRE(fixture.initialized);
    woby::Mesh mesh; mesh.vertices.resize(3);
    mesh.pointIndices = {2,0,2,1};
    woby::MeshNode first; first.name = "first"; first.pointIndexCount = 3;
    woby::MeshNode second; second.name = "second"; second.pointIndexOffset = 3; second.pointIndexCount = 1;
    mesh.nodes = {first, second};
    SUBCASE("valid point geometry") {
        fixture.mesh = woby::createGpuMesh(mesh, woby::meshVertexLayout(), woby::gpuMeshPoints);
        CHECK(woby::graphics::isValid(fixture.mesh.pointIdBuffer));
        CHECK_FALSE(woby::graphics::isValid(fixture.mesh.triangleIndexBuffer));
        CHECK_FALSE(woby::graphics::isValid(fixture.mesh.importedLineBuffer));
        CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{2,0,1});
        REQUIRE(fixture.mesh.nodeRanges.size() == 2);
        CHECK(fixture.mesh.nodeRanges[0].pointIndexCount == 2);
        CHECK(fixture.mesh.nodeRanges[1].pointIndexOffset == 2);
        CHECK(fixture.mesh.nodeRanges[1].pointIndexCount == 1);
        return;
    }
    SUBCASE("invalid vertex") { mesh.pointIndices[0] = 3; }
    SUBCASE("invalid range") { mesh.nodes[1].pointIndexCount = 2; }
    SUBCASE("mixed primitives in one node") {
        mesh.lineIndices = {0,1}; mesh.nodes[0].lineIndexCount = 2;
    }
    CHECK_THROWS_AS((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()), std::runtime_error);
}
