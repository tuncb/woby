#include "scene_renderer.h"
#include "graphics_helpers.h"
#include "marker_pick.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <algorithm>

namespace {
namespace g = woby::graphics;
struct EdgeGpuFixture {
    bool initialized = false;
    woby::UiState state;
    std::vector<woby::LoadedModelRuntime> runtimes;
    woby::TriangleEdgePrograms native, pulled;
    g::ProgramHandle mesh{}, color{}, point{}, markerMesh{}, markerColor{}, markerPoint{};
    g::UniformHandle colorUniform{}, pointUniform{}, uvUniform{}, baseUniform{};
    g::TextureHandle output{}, depth{}, ids{};
    g::IndexBufferHandle referenceEdges{};
    g::FrameBufferHandle target{};
    bool markerIds = false;
    ~EdgeGpuFixture() {
        if (!initialized) { return; }
        woby::destroyModelRuntimes(runtimes);
        woby::destroyTriangleEdgePrograms(native); woby::destroyTriangleEdgePrograms(pulled);
        for (auto handle : {mesh, color, point, markerMesh, markerColor, markerPoint}) { if (g::isValid(handle)) { g::destroy(handle); } }
        for (auto handle : {colorUniform, pointUniform, uvUniform, baseUniform}) { if (g::isValid(handle)) { g::destroy(handle); } }
        if (g::isValid(target)) { g::destroy(target); }
        for (auto handle : {output, depth, ids}) { if (g::isValid(handle)) { g::destroy(handle); } }
        if (g::isValid(referenceEdges)) { g::destroy(referenceEdges); }
        g::shutdown();
    }
};
void initialize(EdgeGpuFixture& fixture, bool multisample, bool markerIds)
{
    fixture.initialized = g::init({}); REQUIRE(fixture.initialized);
    fixture.markerIds = markerIds;
    const std::filesystem::path assets = WOBY_TEST_ASSET_DIRECTORY;
    fixture.native = woby::createTriangleEdgePrograms(assets);
    fixture.pulled = woby::createTriangleEdgePrograms(assets, true);
    fixture.mesh = woby::loadProgram(assets, "vs_mesh.bin", "fs_mesh.bin");
    fixture.color = woby::loadProgram(assets, "vs_color.bin", "fs_color.bin");
    fixture.point = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_point_sprite.bin");
    fixture.markerMesh = woby::loadProgram(assets, "vs_mesh.bin", "fs_marker_mesh.bin");
    fixture.markerColor = woby::loadProgram(assets, "vs_color.bin", "fs_marker_line.bin");
    fixture.markerPoint = woby::loadProgram(assets, "vs_point_sprite.bin", "fs_marker_point.bin");
    fixture.colorUniform = g::createUniform("u_color", g::UniformType::Vec4);
    fixture.pointUniform = g::createUniform("u_pointParams", g::UniformType::Vec4, 2);
    fixture.uvUniform = g::createUniform("u_uvGrid", g::UniformType::Vec4);
    fixture.baseUniform = g::createUniform("u_markerBase", g::UniformType::Vec4);
    const uint64_t flags = multisample ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
    fixture.output = g::createTexture2D(128, 96, false, 1, g::TextureFormat::RGBA8, flags | WOBY_GPU_TEXTURE_READ_BACK);
    fixture.depth = g::createTexture2D(128, 96, false, 1, g::TextureFormat::D24S8, flags);
    if (markerIds) {
        fixture.ids = g::createTexture2D(128, 96, false, 1, g::TextureFormat::RGBA8,
            flags | (multisample ? WOBY_GPU_TEXTURE_MSAA_SAMPLE : uint64_t{0}));
    }
    const std::array attachments{fixture.output, markerIds ? fixture.ids : fixture.depth, fixture.depth};
    fixture.target = g::createFrameBuffer(markerIds ? 3 : 2, attachments.data());
    g::setViewFrameBuffer(0, fixture.target); g::setViewRect(0, 0, 0, 128, 96);
    g::setViewClear(0, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 0x20242aff, 1);
    std::array<float,16> identity{}; bx::mtxIdentity(identity.data());
    g::setViewTransform(0, identity.data(), identity.data());
    woby::Mesh mesh;
    for (const auto& position : std::array<std::array<float,3>,7>{{
        {-.8f,0,.7f},{.8f,0,.7f},{0,.8f,.7f},
        {-.4f,-.4f,.3f},{.4f,-.4f,.3f},{.4f,.4f,.3f},{-.4f,.4f,.3f}}}) {
        woby::Vertex vertex; vertex.position = position; vertex.normal = {0,0,1};
        vertex.texcoord = {position[0]+.5f,position[1]+.5f}; mesh.vertices.push_back(vertex);
    }
    // Exercise index offsets above 16 bits, with no separate runtime edge buffer.
    mesh.indices.assign(65538, 0); mesh.indices.insert(mesh.indices.end(), {0,1,2,3,4,5,3,5,6});
    mesh.nodes = {{"unused",0,65538},{"rear",65538,3},{"front",65541,6}};
    mesh.nodes[2].hasTexcoords = true; mesh.bounds = woby::calculateBounds(mesh.vertices);
    auto file = woby::createUiFileState({}, mesh, 0);
    file.groupSettings[0].visible = false;
    for (auto& group : file.groupSettings) { group.showTriangles = true; group.showVertices = false; }
    file.groupSettings[1].color = {.7f,.2f,.1f,1}; file.groupSettings[2].color = {.2f,.65f,.45f,1};
    fixture.state.files.push_back(std::move(file));
    woby::appendDefaultSceneNodesForFiles(fixture.state, 0); woby::assignSceneObjectIds(fixture.state);
    fixture.runtimes.push_back({woby::createGpuMesh(mesh, woby::meshVertexLayout(), woby::gpuMeshPoints)});
    CHECK_FALSE(g::isValid(fixture.runtimes[0].gpuMesh.lineIndexBuffer));
    const auto reference = woby::prepareSceneMesh(mesh, woby::gpuMeshEdges);
    REQUIRE(reference);
    fixture.referenceEdges = g::createIndexBuffer(g::copy(reference->edgeIndices.data(),
        static_cast<uint32_t>(reference->edgeIndices.size()*sizeof(uint32_t))), WOBY_GPU_BUFFER_INDEX32);
}
std::vector<uint8_t> capture(EdgeGpuFixture& fixture, woby::SceneDrawPlan plan, bool pulled = false, bool referenceLines = false)
{
    woby::MarkerDrawContext markers; markers.baseUniform = fixture.baseUniform;
    const auto reference = plan;
    if (referenceLines) { for (auto& item : plan.items) { item.edges = false; } }
    g::touch(0);
    woby::submitSceneFiles(0, plan, fixture.runtimes,
        fixture.markerIds ? fixture.markerMesh : fixture.mesh, fixture.uvUniform,
        fixture.markerIds ? fixture.markerColor : fixture.color,
        fixture.markerIds ? fixture.markerPoint : fixture.point, fixture.colorUniform, fixture.pointUniform,
        pulled ? fixture.pulled : fixture.native, 128, 96, fixture.markerIds ? &markers : nullptr);
    if (referenceLines) {
        for (const auto& item : reference.items) {
            if (!item.edges) { continue; }
            const auto& mesh = fixture.runtimes[item.fileIndex].gpuMesh;
            const auto& range = mesh.nodeRanges[item.groupIndex];
            auto color = item.color;
            for (size_t channel = 0; channel < 3; ++channel) { color[channel] = std::min(1.0f, color[channel]*1.25f); }
            g::setTransform(item.model.data()); g::setUniform(fixture.colorUniform, color.data());
            g::setVertexBuffer(0, mesh.vertexBuffer);
            g::setIndexBuffer(fixture.referenceEdges, range.lineIndexOffset, range.lineIndexCount);
            auto flags = WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_PT_LINES | WOBY_GPU_STATE_MSAA;
            if (!reference.triangleEdgeXray) { flags |= WOBY_GPU_STATE_DEPTH_TEST_LEQUAL; }
            if (color[3] < .999f) { flags |= WOBY_GPU_STATE_BLEND_ALPHA; }
            woby::setMarkerRenderState(flags, fixture.markerIds);
            g::submit(0, fixture.markerIds ? fixture.markerColor : fixture.color);
        }
    }
    std::vector<uint8_t> pixels(128*96*4);
    const auto ready = g::readTexture(fixture.output, pixels.data());
    while (g::frame() < ready) {}
    return pixels;
}
size_t differences(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
{
    size_t count = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        if (!std::equal(a.begin()+static_cast<ptrdiff_t>(i), a.begin()+static_cast<ptrdiff_t>(i+3), b.begin()+static_cast<ptrdiff_t>(i))) { ++count; }
    }
    return count;
}
}

TEST_CASE("GPU shader edges preserve occlusion picking targets UV color and portable fallback")
{
    for (const bool msaa : {false, true}) for (const bool ids : {false, true}) {
        CAPTURE(msaa); CAPTURE(ids);
        EdgeGpuFixture fixture; initialize(fixture, msaa, ids);
        auto plan = woby::buildSceneDrawPlan(fixture.state);
        const auto visible = capture(fixture, plan);
        CHECK(differences(visible, capture(fixture, plan, true)) < 20);
        std::reverse(plan.items.begin(), plan.items.end());
        CHECK(visible == capture(fixture, plan));
        plan.triangleEdgeXray = true;
        const auto xray = capture(fixture, plan);
        CHECK(differences(visible, xray) > 20);
        CHECK(xray == capture(fixture, plan, false, true)); // Exact hardware-line coverage.
        plan.triangleEdgeXray = false;
        for (auto& item : plan.items) { item.color[3] = .4f; }
        CHECK(capture(fixture, plan) == capture(fixture, plan, false, true)); // Separate alpha blending.
        auto& front = fixture.state.files[0].groupSettings[2]; front.uvGrid.enabled = true;
        plan = woby::buildSceneDrawPlan(fixture.state);
        CHECK(differences(visible, capture(fixture, plan)) > 100);
        CHECK(differences(capture(fixture, plan), capture(fixture, plan, true)) < 20);
    }
}

TEST_CASE("GPU hidden-line occluders precede surfaces and vertex circles keep their full size")
{
    EdgeGpuFixture fixture; initialize(fixture, true, true);
    auto plan = woby::buildSceneDrawPlan(fixture.state);
    plan.items[0].edges = false; plan.items[1].solid = false;
    const auto hidden = capture(fixture, plan);
    std::reverse(plan.items.begin(), plan.items.end());
    CHECK(hidden == capture(fixture, plan));
    const size_t interior = (38*128+57)*4;
    CHECK(hidden[interior] == 32); CHECK(hidden[interior+1] == 36); CHECK(hidden[interior+2] == 42);
    for (const float size : {1.0f, 4.0f, 8.0f, 40.0f}) {
        for (auto& item : plan.items) { item.solid = item.edges = false; item.points = true; item.pointSize = size; }
        const auto baseline = capture(fixture, plan);
        CHECK(baseline == capture(fixture, plan, true));
        plan.triangleEdgeXray = true;
        CHECK(baseline == capture(fixture, plan));
        plan.triangleEdgeXray = false;
    }
}
