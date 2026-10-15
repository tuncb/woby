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
    editMeshGeometry(mesh).vertices.resize(8);
    editMeshGeometry(mesh).indices = {6, 1, 6, 1, 3, 6, 3, 1, 7};
    editMeshGeometry(mesh).nodes = {{"first", 0, 6}, {"shared", 6, 3}, {"overlap", 0, 3}};
    auto worker = std::async(std::launch::async, [source = mesh] {
        return woby::prepareSceneMesh(source, woby::gpuMeshEdges | woby::gpuMeshPoints);
    });
    editMeshGeometry(mesh).indices.clear(); // The result does not borrow this source or the worker snapshot.
    const auto prepared = worker.get();
    REQUIRE(prepared);
    CHECK(prepared->pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 3, 1, 7, 6, 1});
    CHECK(prepared->edgeIndices == std::vector<uint32_t>{6, 1, 1, 6, 6, 6, 1, 3, 3, 6, 6, 1, 3, 1, 1, 7, 7, 3});
    REQUIRE(prepared->nodeRanges.size() == 3);
    CHECK(prepared->nodeRanges[2].pointIndexOffset == 6);
    CHECK(prepared->nodeRanges[2].pointIndexCount == 2);

    editMeshGeometry(mesh).indices.assign(300000, 0);
    editMeshGeometry(mesh).nodes = {{"large", 0, 300000}};
    // Cancel in initial validation, range preparation, and optional edge generation.
    for (const size_t stop : {1u, 4u, 9u, 14u}) {
        size_t polls = 0;
        CHECK_FALSE(woby::prepareSceneMesh(mesh, woby::gpuMeshEdges, [&] { return ++polls == stop; }));
        CHECK(polls == stop);
    }
    CHECK(woby::prepareSceneMesh(mesh).has_value());
}

TEST_CASE("Mesh uploads preserve every primitive buffer and complete without rendered frames")
{
    RendererFixture fixture;
    woby::graphics::Init init; init.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(init); REQUIRE(fixture.initialized);
    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices.resize(8);
    editMeshGeometry(mesh).indices = {6, 1, 3, 3, 1, 7};
    editMeshGeometry(mesh).lineIndices = {0, 2}; editMeshGeometry(mesh).pointIndices = {5, 4, 5};
    editMeshGeometry(mesh).nodes = {{"triangles", 0, 6}, {"lines"}, {"points"}};
    editMeshGeometry(mesh).nodes[1].lineIndexCount = 2;
    editMeshGeometry(mesh).nodes[2].pointIndexCount = 3;
    auto prepared = woby::prepareSceneMesh(mesh, woby::gpuMeshEdges | woby::gpuMeshPoints); REQUIRE(prepared);
    auto upload = woby::beginGpuMeshUpload(std::move(*prepared), mesh, woby::meshVertexLayout());
    mesh = {}; // The request owns the source snapshot.
    woby::waitGpuMeshUpload(upload);
    CHECK(woby::graphics::submittedFrameToken() == 0);
    CHECK(upload.uploadedBytes == 256 + 24 + 8 + 48 + 32);
    CHECK(upload.uploadedBytes == upload.totalBytes);
    fixture.mesh = std::move(upload.mesh);
    CHECK(woby::graphics::isValid(fixture.mesh.vertexBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.triangleIndexBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.importedLineBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.lineIndexBuffer));
    CHECK(woby::graphics::isValid(fixture.mesh.pointIdBuffer));
    CHECK_FALSE(woby::graphics::isValid(fixture.mesh.vertexColors));
    CHECK_FALSE(woby::graphics::isValid(fixture.mesh.pointColors));
    CHECK(fixture.mesh.pointVertexIndices == std::vector<uint32_t>{6, 1, 3, 7, 0, 2, 5, 4});
}

TEST_CASE("Optional vertex colors validate their count and survive point reordering and upload") {
    RendererFixture fixture;
    woby::graphics::Init init; init.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(init); REQUIRE(fixture.initialized);
    woby::Mesh source;
    auto& geometry = editMeshGeometry(source);
    geometry.vertices = {{{0,0,0}}, {{1,0,0}}, {{0,1,0}}};
    geometry.pointIndices = {2,0,1}; geometry.nodes = {{"points"}}; geometry.nodes[0].pointIndexCount = 3;
    geometry.vertexColors = {0xff0000ff};
    CHECK_THROWS_AS((void)woby::prepareSceneMesh(source), std::runtime_error);
    geometry.vertexColors = {0xff0000ff, 0, 0xffff0000};
    auto prepared = woby::prepareSceneMesh(source, woby::gpuMeshPoints, {}, false); REQUIRE(prepared);
    const auto expectedBytes = prepared->uploadBytes;
    auto upload = woby::beginGpuMeshUpload(*prepared, source, woby::meshVertexLayout());
    woby::waitGpuMeshUpload(upload); CHECK(upload.totalBytes == expectedBytes);
    fixture.mesh = std::move(upload.mesh); CHECK(woby::graphics::isValid(fixture.mesh.vertexColors));
    auto cloud = woby::points::buildCloud(source, prepared->pointVertexIndices, prepared->nodeRanges, {}, 2, 1); REQUIRE(cloud);
    CHECK(cloud->colors == std::vector<uint32_t>{0xffff0000, 0xff0000ff, 0});
    for (const auto& point : cloud->points) { CHECK(cloud->colors[point.id - 1] == geometry.vertexColors[cloud->sourceVertices[point.id - 1]]); }
    for (const auto& point : cloud->proxies) { CHECK(cloud->colors[point.id - 1] == geometry.vertexColors[cloud->sourceVertices[point.id - 1]]); }
    prepared->pointCloud = std::make_shared<const woby::points::Cloud>(std::move(*cloud));
    prepared->compactOnly = true;
    auto points = woby::beginGpuPointUpload(std::move(*prepared), source, woby::meshVertexLayout(), false);
    woby::waitGpuMeshUpload(points); woby::commitGpuPointUpload(fixture.mesh, points);
    CHECK_FALSE(woby::graphics::isValid(fixture.mesh.vertexColors));
    CHECK(woby::graphics::isValid(fixture.mesh.pointColors));
    woby::destroyGpuMesh(fixture.mesh);
    CHECK_FALSE(woby::graphics::isValid(fixture.mesh.pointColors));
}

TEST_CASE("Optional mesh features prepare asynchronously and reject a changed geometry snapshot") {
    RendererFixture fixture;
    woby::graphics::Init options; options.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(options); REQUIRE(fixture.initialized);
    woby::Mesh source;
    editMeshGeometry(source).vertices.resize(3);
    editMeshGeometry(source).indices = {0, 1, 2};
    editMeshGeometry(source).nodes = {{"triangle", 0, 3}};
    publishMeshGeometry(source);
    woby::LoadedModelRuntime runtime;
    runtime.gpuMesh = woby::createGpuMesh(source, woby::meshVertexLayout());
    std::promise<void> entered, release;
    auto ready = release.get_future().share();
    auto blocker = woby::graphics::beginBufferUpload([&entered, ready](const std::atomic<bool>&) {
        entered.set_value(); ready.wait(); return std::vector<woby::graphics::BufferUploadSource>{};
    });
    entered.get_future().wait();
    const uint8_t features = woby::gpuMeshEdges | woby::gpuMeshPoints;
    woby::updateGpuMeshFeatures(runtime, source, features);
    auto old = runtime.featureUpload;
    CHECK(old != nullptr);
    CHECK_FALSE(woby::graphics::isValid(runtime.gpuMesh.lineIndexBuffer));
    editMeshGeometry(source).indices = {2, 1, 0}; publishMeshGeometry(source);
    woby::updateGpuMeshFeatures(runtime, source, features);
    CHECK(runtime.featureUpload != old);
    release.set_value();
    woby::graphics::waitBufferUpload(blocker);
    woby::graphics::waitBufferUpload(old);
    CHECK_THROWS_WITH(woby::graphics::adoptBufferUpload(old), "GPU upload was canceled.");
    REQUIRE(runtime.featureUpload);
    woby::graphics::waitBufferUpload(runtime.featureUpload);
    woby::updateGpuMeshFeatures(runtime, source, features);
    CHECK_FALSE(runtime.featureUpload);
    CHECK(woby::graphics::isValid(runtime.gpuMesh.lineIndexBuffer));
    CHECK(woby::graphics::isValid(runtime.gpuMesh.pointIdBuffer));
    CHECK(woby::graphics::submittedFrameToken() == 0);
    fixture.mesh = std::move(runtime.gpuMesh);
}

TEST_CASE("A line-only model can asynchronously request edges and points together") {
    RendererFixture fixture;
    woby::graphics::Init options; options.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(options); REQUIRE(fixture.initialized);
    woby::Mesh source; editMeshGeometry(source).vertices.resize(3);
    editMeshGeometry(source).lineIndices = {0, 1, 1, 2};
    editMeshGeometry(source).nodes = {{"lines"}};
    editMeshGeometry(source).nodes[0].lineIndexCount = 4;
    woby::LoadedModelRuntime runtime;
    runtime.gpuMesh = woby::createGpuMesh(source, woby::meshVertexLayout());
    const uint8_t features = woby::gpuMeshEdges | woby::gpuMeshPoints;
    woby::updateGpuMeshFeatures(runtime, source, features);
    REQUIRE(runtime.featureUpload);
    woby::graphics::waitBufferUpload(runtime.featureUpload);
    woby::updateGpuMeshFeatures(runtime, source, features);
    CHECK_FALSE(woby::graphics::isValid(runtime.gpuMesh.lineIndexBuffer));
    CHECK(woby::graphics::isValid(runtime.gpuMesh.pointIdBuffer));
    fixture.mesh = std::move(runtime.gpuMesh);
}

TEST_CASE("Canceled mesh requests cannot publish and do not prevent a fresh upload")
{
    RendererFixture fixture;
    woby::graphics::Init init; init.type = woby::graphics::RendererType::Noop;
    fixture.initialized = woby::graphics::init(init); REQUIRE(fixture.initialized);
    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices.resize(3); editMeshGeometry(mesh).indices = {0, 1, 2};
    editMeshGeometry(mesh).nodes = {{"triangle", 0, 3}};
    auto upload = woby::beginGpuMeshUpload(*woby::prepareSceneMesh(mesh), mesh, woby::meshVertexLayout());
    const auto ticket = upload.ticket;
    woby::abortGpuMeshUpload(upload);
    woby::graphics::waitBufferUpload(ticket);
    CHECK_THROWS_WITH(woby::graphics::adoptBufferUpload(ticket), "GPU upload was canceled.");
    fixture.mesh = woby::createGpuMesh(mesh, woby::meshVertexLayout());
    CHECK(woby::graphics::isValid(fixture.mesh.triangleIndexBuffer));
    const std::array<uint32_t, 8> data{};
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
    editMeshGeometry(mesh).vertices.resize(8);
    editMeshGeometry(mesh).indices = {6, 1, 6, 1, 3, 6, 3, 1, 7, 7, 1, 3};
    editMeshGeometry(mesh).nodes = {{"first", 0, 6}, {"empty", 6, 0}, {"shared", 6, 6}, {"overlap", 0, 3}};

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
        for (size_t i = 0; i < meshGeometry(mesh).nodes.size(); ++i) {
            const auto& range = fixture.mesh.nodeRanges[i];
            CHECK(range.pointIndexOffset == offsets[i]);
            CHECK(range.pointIndexCount == counts[i]);
            CHECK(range.triangleIndexOffset == meshGeometry(mesh).nodes[i].indexOffset);
            CHECK(range.triangleIndexCount == meshGeometry(mesh).nodes[i].indexCount);
            CHECK(range.lineIndexOffset == meshGeometry(mesh).nodes[i].indexOffset * 2u);
            CHECK(range.lineIndexCount == meshGeometry(mesh).nodes[i].indexCount * 2u);
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

TEST_CASE("Point parameters preserve large offsets and pixel sizing")
{
    for (const uint32_t offset : {0u, 65535u, 65536u, 16777217u, 38414391u,
                                 std::numeric_limits<uint32_t>::max()}) {
        const auto params = woby::pointSpriteParameters(40.0f, 1280, 720, offset);
        CHECK(params.size == 40.0f);
        CHECK(params.width == 1280u);
        CHECK(params.height == 720u);
        CHECK(params.offset == offset);
    }
    const auto minimized = woby::pointSpriteParameters(1.0f, 0, 0, 0);
    CHECK(minimized.width == 1u);
    CHECK(minimized.height == 1u);
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
    CHECK(woby::requestedGpuMeshFeatures(file) == woby::gpuMeshPoints);
    file.groupSettings[0].visible = false;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
    file.groupSettings[1].opacity = 0;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
    file.groupSettings[0].visible = true;
    CHECK(woby::requestedGpuMeshFeatures(file) == woby::gpuMeshPoints);
    file.fileSettings.visible = false;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
    file.fileSettings.visible = true;
    file.fileSettings.opacity = 0;
    CHECK(woby::requestedGpuMeshFeatures(file) == woby::gpuMeshPoints);
    file.groupSettings[0].opacity = 0;
    CHECK(woby::requestedGpuMeshFeatures(file) == woby::gpuMeshPoints);
    file.groupSettings[0].showVertices = false;
    CHECK(woby::requestedGpuMeshFeatures(file) == 0);
}

TEST_CASE("GPU uploads reject invalid indices and ranges before allocating")
{
    woby::Mesh mesh;
    editMeshGeometry(mesh).vertices.resize(3);
    editMeshGeometry(mesh).indices = {0, 1, 99};
    editMeshGeometry(mesh).nodes = {{"part", 0, 3}};
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid vertex index.");
    editMeshGeometry(mesh).indices[2] = 2;
    editMeshGeometry(mesh).nodes[0].indexCount = 6;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid primitive range.");
    editMeshGeometry(mesh).indices.clear();
    editMeshGeometry(mesh).nodes[0].indexCount = 0;
    editMeshGeometry(mesh).nodes[0].lineIndexCount = 2;
    editMeshGeometry(mesh).lineIndices = {0, 99};
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid line vertex index.");
    editMeshGeometry(mesh).lineIndices[1] = 1;
    editMeshGeometry(mesh).nodes[0].lineIndexOffset = 1;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid primitive range.");
    editMeshGeometry(mesh).nodes[0].lineIndexOffset = 0;
    editMeshGeometry(mesh).nodes[0].lineIndexCount = 1;
    CHECK_THROWS_WITH((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()),
        "Scene contains an invalid primitive range.");
    editMeshGeometry(mesh).lineIndices.push_back(2);
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
    woby::Mesh mesh; editMeshGeometry(mesh).vertices.resize(3);
    editMeshGeometry(mesh).pointIndices = {2,0,2,1};
    woby::MeshNode first; first.name = "first"; first.pointIndexCount = 3;
    woby::MeshNode second; second.name = "second"; second.pointIndexOffset = 3; second.pointIndexCount = 1;
    editMeshGeometry(mesh).nodes = {first, second};
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
    SUBCASE("invalid vertex") { editMeshGeometry(mesh).pointIndices[0] = 3; }
    SUBCASE("invalid range") { editMeshGeometry(mesh).nodes[1].pointIndexCount = 2; }
    SUBCASE("mixed primitives in one node") {
        editMeshGeometry(mesh).lineIndices = {0,1}; editMeshGeometry(mesh).nodes[0].lineIndexCount = 2;
    }
    CHECK_THROWS_AS((void)woby::createGpuMesh(mesh, woby::meshVertexLayout()), std::runtime_error);
}
