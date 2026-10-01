#include "marker_pick.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <algorithm>

namespace {

struct MarkerGpuFixture {
    bool initialized = false;
    woby::GpuMarkerPicker picker;
    woby::UiState state;
    std::vector<woby::LoadedModelRuntime> runtimes;
    woby::graphics::UniformHandle colorUniform = WOBY_GPU_INVALID_HANDLE, pointUniform = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::UniformHandle uvUniform = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::TextureHandle output = WOBY_GPU_INVALID_HANDLE, staging = WOBY_GPU_INVALID_HANDLE;
    woby::graphics::FrameBufferHandle outputFramebuffer = WOBY_GPU_INVALID_HANDLE;
    std::vector<uint8_t> pixels = std::vector<uint8_t>(128 * 128 * 4);
    uint32_t captureReady = 0;
    bool capturePending = false;

    ~MarkerGpuFixture()
    {
        if (!initialized) { return; }
        // Both result and image readbacks own CPU destinations in this fixture.
        uint32_t frame = woby::graphics::frame();
        while (capturePending && !woby::markerFrameReached(frame, captureReady)) { frame = woby::graphics::frame(); }
        woby::destroyGpuMarkerPicker(picker);
        woby::destroyModelRuntimes(runtimes);
        if (woby::graphics::isValid(outputFramebuffer)) { woby::graphics::destroy(outputFramebuffer); }
        if (woby::graphics::isValid(output)) { woby::graphics::destroy(output); }
        if (woby::graphics::isValid(staging)) { woby::graphics::destroy(staging); }
        if (woby::graphics::isValid(colorUniform)) { woby::graphics::destroy(colorUniform); }
        if (woby::graphics::isValid(pointUniform)) { woby::graphics::destroy(pointUniform); }
        if (woby::graphics::isValid(uvUniform)) { woby::graphics::destroy(uvUniform); }
        woby::graphics::shutdown();
    }
};

woby::UiFileState markerFile(float z, uint64_t id)
{
    woby::UiFileState file;
    file.objectId = id;
    file.mesh.vertices.resize(3);
    file.mesh.vertices[0].position = {0, 0, z};
    file.mesh.vertices[1].position = {-1, -1, z};
    file.mesh.vertices[2].position = {1, -1, z};
    file.mesh.indices = {0, 1, 2};
    file.mesh.nodes = {{"triangle", 0, 3}};
    file.groupSettings.resize(1);
    auto& group = file.groupSettings[0];
    group.showSolidMesh = false; group.showVertices = true;
    group.color = {.1f, .4f, .6f, 1};
    return file;
}

} // namespace

TEST_CASE("GPU marker picking highlights before readback and handles visibility resize and cancellation")
{
    MarkerGpuFixture fixture;
    woby::graphics::Init init;
#if defined(_WIN32)
    init.type = woby::graphics::RendererType::Vulkan;
#else
    init.type = woby::graphics::RendererType::Vulkan;
#endif
    init.resolution.width = init.resolution.height = 0;
    fixture.initialized = woby::graphics::init(init);
    REQUIRE(fixture.initialized);
    REQUIRE(woby::supportsGpuMarkerPicking(*woby::graphics::getCaps()));
    auto& state = fixture.state;
    state.files = {markerFile(0, 10), markerFile(1, 20)};
    state.masterVertexPointSize = 12;
    woby::setSceneUpAxis(state, woby::SceneUpAxis::y);
    woby::recalculateSceneBounds(state);
    woby::lookAtUiCamera(state, {0, 0, 4}, {0, 0, 0});
    for (const auto& file : state.files) {
        fixture.runtimes.push_back({woby::createGpuMesh(file.mesh, woby::meshVertexLayout(), woby::gpuMeshPoints), woby::gpuMeshPoints});
    }
    fixture.colorUniform = woby::graphics::createUniform("u_color", woby::graphics::UniformType::Vec4);
    fixture.pointUniform = woby::graphics::createUniform("u_pointParams", woby::graphics::UniformType::Vec4, 2);
    fixture.uvUniform = woby::graphics::createUniform("u_uvGrid", woby::graphics::UniformType::Vec4);
    fixture.output = woby::graphics::createTexture2D(128, 128, false, 1, woby::graphics::TextureFormat::BGRA8, WOBY_GPU_TEXTURE_RT);
    fixture.staging = woby::graphics::createTexture2D(128, 128, false, 1, woby::graphics::TextureFormat::BGRA8,
        WOBY_GPU_TEXTURE_READ_BACK | WOBY_GPU_TEXTURE_BLIT_DST);
    fixture.outputFramebuffer = woby::graphics::createFrameBuffer(1, &fixture.output, false);
    REQUIRE(woby::graphics::isValid(fixture.outputFramebuffer));
    REQUIRE(woby::graphics::isValid(fixture.staging));
    const std::filesystem::path assets = WOBY_TEST_ASSET_DIRECTORY;
    auto& picker = fixture.picker;
    woby::SceneViewport viewport{0, 128, 128, 0};
    int samples = 4;
    auto render = [&](bool capture = false) {
        const auto view = woby::scenePickView(state.camera, state.upAxis, state.sceneBounds,
            viewport.width, viewport.height, woby::graphics::getCaps()->homogeneousDepth, 1.0f);
        REQUIRE(woby::beginGpuMarkerPicking(picker, assets, state, viewport,
            {static_cast<float>(viewport.width)/2, static_cast<float>(viewport.height)/2}, true, samples));
        // Exercise exact byte encoding above float's 24-bit integer precision.
        picker.context.list.nextId = 16777217;
        woby::graphics::setViewTransform(1, view.view.data(), view.renderProjection.data(), true);
        woby::graphics::setViewMode(1, woby::graphics::ViewMode::Sequential);
        woby::graphics::touch(1);
        woby::submitSceneFiles(1, state.files, state.sceneNodes, fixture.runtimes, state.masterVertexPointSize,
            picker.mesh, fixture.uvUniform, picker.line, picker.point, fixture.colorUniform, fixture.pointUniform,
            viewport.width, viewport.height, &picker.context);
        woby::submitGpuMarkerPicking(picker, viewport);
        // Test output replaces the window, using the production composite/highlight.
        woby::graphics::setViewFrameBuffer(3, fixture.outputFramebuffer);
        woby::graphics::setViewFrameBuffer(4, fixture.outputFramebuffer);
        if (capture) {
            woby::graphics::blit(7, fixture.staging, 0, 0, fixture.output);
            fixture.captureReady = woby::graphics::readTexture(fixture.staging, fixture.pixels.data());
            fixture.capturePending = true;
        }
        woby::pollGpuMarkerPicking(picker, woby::graphics::frame(), state, fixture.runtimes);
    };
    render(true);
    CHECK_FALSE(picker.coordinates); // Highlight is already in the captured GPU frame.
    for (int i = 0; i < 12; ++i) { render(); }
    REQUIRE(picker.coordinates);
    CHECK(picker.coordinates->localPosition == std::array<float, 3>{0, 0, 1});
    CHECK(woby::markerFrameReached(picker.frame, fixture.captureReady));
    size_t highlightedPixels = 0;
    for (size_t i = 0; i < fixture.pixels.size(); i += 4) {
        if (fixture.pixels[i] < 55 && fixture.pixels[i+1] > 135 && fixture.pixels[i+1] < 205
            && fixture.pixels[i+2] > 230) { ++highlightedPixels; }
    }
    CHECK(highlightedPixels >= 40);
    CHECK(picker.context.list.draws[0].firstId != picker.context.list.draws[1].firstId);

    viewport = {9, 96, 80, 7}; samples = 1;
    for (int i = 0; i < 12; ++i) { render(); }
    REQUIRE(picker.coordinates);
    CHECK(picker.coordinates->localPosition[2] == 1);
    state.files[1].fileSettings.opacity = .4f; ++state.sceneEditRevision;
    for (int i = 0; i < 12; ++i) { render(); }
    REQUIRE(picker.coordinates);
    CHECK(picker.coordinates->localPosition[2] == 1);
    state.files[1].fileSettings.visible = false; ++state.sceneEditRevision;
    for (int i = 0; i < 12; ++i) { render(); }
    REQUIRE(picker.coordinates);
    CHECK(picker.coordinates->localPosition[2] == 0);
    state.files[0].fileSettings.opacity = 0; ++state.sceneEditRevision;
    for (int i = 0; i < 12; ++i) { render(); }
    CHECK_FALSE(picker.coordinates);

    // A surface rendered over the rear marker clears the ID, including at alpha.
    state.files[0].fileSettings.opacity = 1;
    auto& front = state.files[1];
    front.fileSettings.visible = true; front.fileSettings.opacity = 1;
    front.groupSettings[0].showSolidMesh = true;
    front.mesh.vertices[0].position = {-1, -1, 1};
    front.mesh.vertices[1].position = {1, -1, 1};
    front.mesh.vertices[2].position = {0, 1, 1};
    woby::destroyGpuMesh(fixture.runtimes[1].gpuMesh);
    fixture.runtimes[1].gpuMesh = woby::createGpuMesh(front.mesh, woby::meshVertexLayout(), woby::gpuMeshPoints);
    ++state.sceneEditRevision;
    for (int i = 0; i < 12; ++i) { render(); }
    CHECK_FALSE(picker.coordinates);
    front.fileSettings.opacity = .4f; ++state.sceneEditRevision;
    for (int i = 0; i < 12; ++i) { render(); }
    CHECK_FALSE(picker.coordinates);

    // Keep marker lookup consistent with the adaptive projection on a large
    // scene whose geometry lies beyond the old target-based far plane.
    front.fileSettings.visible = false;
    auto& rear = state.files[0];
    for (auto& vertex : rear.mesh.vertices) {
        vertex.position[0] *= 100000; vertex.position[1] *= 100000;
    }
    rear.mesh.bounds = woby::calculateBounds(rear.mesh.vertices);
    woby::destroyGpuMesh(fixture.runtimes[0].gpuMesh);
    fixture.runtimes[0].gpuMesh = woby::createGpuMesh(rear.mesh, woby::meshVertexLayout(), woby::gpuMeshPoints);
    woby::recalculateSceneBounds(state);
    woby::lookAtUiCamera(state, {0, 0, 1000000}, {0, 0, 999999});
    ++state.sceneEditRevision;
    CHECK(woby::cameraDepthRange(state.camera, state.sceneBounds, state.upAxis).nearPlane > 1000);
    for (int i = 0; i < 12; ++i) { render(); }
    REQUIRE(picker.coordinates);
    CHECK(picker.coordinates->localPosition == std::array<float, 3>{0, 0, 0});

    ++state.sceneEditRevision;
    render(); // Cancel a hit while its ID is still in flight.
    CHECK_FALSE(woby::beginGpuMarkerPicking(picker, assets, state, viewport, {48, 40}, false, samples));
    ++state.sceneGeneration;
    state.files.clear(); woby::destroyModelRuntimes(fixture.runtimes);
    for (int i = 0; i < 12; ++i) { woby::pollGpuMarkerPicking(picker, woby::graphics::frame(), state, fixture.runtimes); }
    CHECK_FALSE(picker.coordinates);
    CHECK(std::none_of(picker.requests.begin(), picker.requests.end(), [](const auto& request) { return request.pending; }));
}
