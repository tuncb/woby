#include "scene_renderer.h"
#include "graphics_helpers.h"
#include "marker_pick.h"

#include <doctest/doctest.h>
#include <cstring>
#include <algorithm>
#include <cmath>

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

namespace {
namespace g = woby::graphics;
struct TransparencyFixture {
    bool initialized = false;
    std::vector<woby::LoadedModelRuntime> models;
    woby::TransparentSurfacePrograms transparency;
    woby::TriangleEdgePrograms edges, pulled;
    g::ProgramHandle mesh{}, point{};
    g::UniformHandle color{}, uv{}, params{}, base{};
    g::TextureHandle output{}, depth{}, ids{};
    g::FrameBufferHandle target{};
    woby::SceneDrawPlan plan;
    ~TransparencyFixture() {
        if (!initialized) { return; }
        woby::destroyModelRuntimes(models);
        woby::destroyTransparentSurfacePrograms(transparency);
        woby::destroyTriangleEdgePrograms(edges);
        woby::destroyTriangleEdgePrograms(pulled);
        for (auto h : {mesh, point}) { if (g::isValid(h)) { g::destroy(h); } }
        for (auto h : {color, uv, params, base}) { if (g::isValid(h)) { g::destroy(h); } }
        if (g::isValid(target)) { g::destroy(target); }
        for (auto h : {output, depth, ids}) { if (g::isValid(h)) { g::destroy(h); } }
        g::shutdown();
    }
};
void initializeTransparency(TransparencyFixture& f, bool msaa)
{
    f.initialized = g::init({}); REQUIRE(f.initialized);
    f.transparency = woby::createTransparentSurfacePrograms(WOBY_TEST_ASSET_DIRECTORY);
    f.edges = woby::createTriangleEdgePrograms(WOBY_TEST_ASSET_DIRECTORY);
    f.pulled = woby::createTriangleEdgePrograms(WOBY_TEST_ASSET_DIRECTORY, true);
    f.mesh = woby::loadProgram(WOBY_TEST_ASSET_DIRECTORY, "vs_mesh.bin", "fs_marker_mesh.bin");
    f.point = woby::loadProgram(WOBY_TEST_ASSET_DIRECTORY, "vs_point_sprite.bin", "fs_marker_point.bin");
    f.color = g::createUniform("u_color", g::UniformType::Vec4);
    f.uv = g::createUniform("u_uvGrid", g::UniformType::Vec4);
    f.params = g::createUniform("u_pointParams", g::UniformType::Vec4, 2);
    f.base = g::createUniform("u_markerBase", g::UniformType::Vec4);
    const auto flags = msaa ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
    f.output = g::createTexture2D(96, 80, false, 1, g::TextureFormat::RGBA8, flags | WOBY_GPU_TEXTURE_READ_BACK);
    f.ids = g::createTexture2D(96, 80, false, 1, g::TextureFormat::RGBA8, flags | WOBY_GPU_TEXTURE_READ_BACK);
    f.depth = g::createTexture2D(96, 80, false, 1, g::TextureFormat::D24S8, flags);
    const std::array targets{f.output, f.ids, f.depth};
    f.target = g::createFrameBuffer(3, targets.data());
    g::setViewFrameBuffer(0, f.target);
    g::setViewRect(0, 9, 7, 80, 64); // Nonzero viewport origin, as in the real UI.
    g::setViewClear(0, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 0x20242aff);
    woby::Mesh source;
    for (const auto& p : std::array<std::array<float,3>,7>{{
        {-.95f,-.9f,.15f},{.95f,-.9f,.85f},{0,.95f,.5f},
        {-.95f,-.9f,.85f},{.95f,-.9f,.15f},{0,.95f,.5f},{0,0,.95f}}}) {
        woby::Vertex v; v.position = p; v.normal = {0,0,1};
        if (source.vertices.size() >= 3 && source.vertices.size() < 6) { v.normal = {0,0,-1}; }
        v.texcoord = {p[0]+.5f,p[1]+.5f}; source.vertices.push_back(v);
    }
    source.indices = {0,1,2,3,4,5}; source.pointIndices = {6};
    source.nodes = {{"red",0,3},{"blue",3,3},{"point"}};
    source.nodes[2].pointIndexCount = 1;
    f.models.push_back({woby::createGpuMesh(source, woby::meshVertexLayout(), woby::gpuMeshPoints)});
    for (size_t i = 0; i < 2; ++i) {
        woby::SceneDrawItem item;
        bx::mtxIdentity(item.model.data()); item.groupIndex = i; item.solid = true;
        item.color = i == 0 ? std::array<float,4>{1,0,0,.4f} : std::array<float,4>{0,0,1,.55f};
        f.plan.items.push_back(item);
    }
}
struct TransparencyImage { std::vector<uint8_t> color, ids; uint32_t draws = 0; };
TransparencyImage captureTransparency(TransparencyFixture& f, const woby::SceneDrawPlan& plan,
    bool reversed = false, bool approximate = true, bool perspective = false, bool pulled = false)
{
    std::array<float,16> projection{}; bx::mtxIdentity(projection.data());
    if (perspective) {
        projection[10] = 1.2f/1.1f; projection[14] = -.12f/1.1f;
        projection[11] = 1; projection[15] = 0;
    }
    if (reversed) {
        projection[10] = projection[11]-projection[10];
        projection[14] = projection[15]-projection[14];
    }
    g::setViewTransform(0, nullptr, projection.data(), reversed);
    g::touch(0);
    woby::MarkerDrawContext markers; markers.baseUniform = f.base;
    woby::submitSceneFiles(0, plan, f.models, f.mesh, f.uv, f.mesh, f.point, f.color, f.params,
        pulled ? f.pulled : f.edges, 80, 64, &markers, false, nullptr, approximate ? &f.transparency : nullptr);
    TransparencyImage result{std::vector<uint8_t>(96*80*4),std::vector<uint8_t>(96*80*4)};
    g::readTexture(f.output, result.color.data());
    const auto ready = g::readTexture(f.ids, result.ids.data());
    auto frame = g::frame();
    result.draws = g::getStats()->numDraw;
    while (frame < ready) { frame = g::frame(); }
    return result;
}
int maximumColorDifference(const TransparencyImage& a, const TransparencyImage& b)
{
    int largest = 0;
    for (size_t i = 0; i < a.color.size(); ++i) {
        largest = std::max(largest, std::abs(int(a.color[i])-int(b.color[i])));
    }
    return largest;
}
}

TEST_CASE("Weighted transparent surfaces ignore group and intersecting triangle submission order")
{
    for (const bool msaa : {false,true}) {
        CAPTURE(msaa);
        TransparencyFixture f; initializeTransparency(f, msaa);
        const auto image = captureTransparency(f, f.plan);
        auto reversed = f.plan; std::reverse(reversed.items.begin(), reversed.items.end());
        CHECK(maximumColorDifference(image, captureTransparency(f, reversed)) <= 1);
        CHECK(maximumColorDifference(captureTransparency(f, f.plan, false, false),
            captureTransparency(f, reversed, false, false)) > 10);
        CHECK(maximumColorDifference(image, captureTransparency(f, f.plan, true)) <= 1);
        CHECK(maximumColorDifference(captureTransparency(f, f.plan, false, true, true),
            captureTransparency(f, f.plan, true, true, true)) <= 1);
        const auto left = (48*96+35)*4, right = (48*96+63)*4;
        CHECK(image.color[left] > image.color[left+2]);
        CHECK(image.color[right+2] > image.color[right]);
        // The same self-overlap inside a single group must also be independent
        // of index order, rather than merely sorting group centers.
        auto oneGroup = f.plan; oneGroup.items.resize(1);
        f.models[0].gpuMesh.nodeRanges[0].triangleIndexCount = 6;
        const auto original = captureTransparency(f, oneGroup);
        const auto unsorted = captureTransparency(f, oneGroup, false, false);
        const auto old = f.models[0].gpuMesh.triangleIndexBuffer;
        const std::array<uint32_t,6> indices{3,4,5,0,1,2};
        f.models[0].gpuMesh.triangleIndexBuffer = g::createIndexBuffer(g::copy(indices.data(), sizeof(indices)), WOBY_GPU_BUFFER_INDEX32);
        g::destroy(old);
        CHECK(maximumColorDifference(original, captureTransparency(f, oneGroup)) <= 1);
        CHECK(maximumColorDifference(unsorted, captureTransparency(f, oneGroup, false, false)) > 10);
        // No accumulation data may survive a frame with no transparent draws.
        auto hidden = oneGroup; hidden.items[0].color[3] = 0;
        const auto blank = captureTransparency(f, hidden);
        CHECK(blank.color[(39*96+49)*4] == 32);
        CHECK(blank.color[(39*96+49)*4+1] == 36);
        CHECK(blank.color[(39*96+49)*4+2] == 42);
        CHECK(maximumColorDifference(original, captureTransparency(f, oneGroup)) <= 1);
    }
}

TEST_CASE("Weighted transparency preserves opaque occlusion prominent markers and UV shading")
{
    for (const bool msaa : {false,true}) {
        CAPTURE(msaa);
        TransparencyFixture f; initializeTransparency(f, msaa);
        auto single = f.plan; single.items.resize(1); single.items[0].color[3] = .6f;
        for (const auto alpha : {.02f,.6f,.98f}) {
            single.items[0].color[3] = alpha;
            const auto reference = captureTransparency(f, single, false, false);
            CHECK(maximumColorDifference(reference, captureTransparency(f, single)) <= 1);
        }
        single.items[0].color[3] = .6f;
        single.items[0].uvGrid = {8,8,1,0};
        CHECK(maximumColorDifference(captureTransparency(f, single, false, false), captureTransparency(f, single)) <= 1);
        auto covered = f.plan;
        covered.items[0].color[3] = 1;
        covered.items[0].model[10] = 0; covered.items[0].model[14] = .05f;
        const auto opaque = captureTransparency(f, covered);
        covered.items.resize(1);
        CHECK(opaque.color == captureTransparency(f, covered).color);
        woby::SceneDrawItem marker;
        bx::mtxIdentity(marker.model.data()); marker.groupIndex = 2; marker.points = true;
        marker.pointSize = 12; marker.color = {0,1,0,0}; // Surface opacity never affects this marker.
        auto marked = f.plan; marked.items.push_back(marker);
        const auto withSurfaces = captureTransparency(f, marked);
        marked.items.erase(marked.items.begin(),marked.items.begin()+2);
        const auto pointOnly = captureTransparency(f, marked);
        const size_t center = (39*96+49)*4;
        for (size_t c = 0; c < 4; ++c) {
            CHECK(withSurfaces.color[center+c] == pointOnly.color[center+c]);
            CHECK(withSurfaces.ids[center+c] == pointOnly.ids[center+c]);
        }
        CHECK(withSurfaces.ids[center] != 0);
        covered.items.push_back(marker);
        CHECK(captureTransparency(f, covered).ids[center] == 0); // Opaque geometry still occludes the marker.
    }
}

TEST_CASE("Combined transparent edges retain surface opacity with native and fallback barycentrics")
{
    for (const bool msaa : {false,true}) {
        CAPTURE(msaa);
        TransparencyFixture f; initializeTransparency(f,msaa);
        CHECK(f.edges.nativeBarycentrics == ((g::getCaps()->supported & WOBY_GPU_CAPS_FRAGMENT_BARYCENTRIC) != 0));
        CHECK_FALSE(f.pulled.nativeBarycentrics);
        REQUIRE(g::isValid(f.edges.transparentSurface));
        REQUIRE(g::isValid(f.pulled.transparentSurface));
        auto single = f.plan; single.items.resize(1);
        for (const float alpha : {.02f,.35f,.7f,.98f}) {
            CAPTURE(alpha);
            single.items[0].color[3] = alpha;
            single.items[0].edges = false;
            const auto fill = captureTransparency(f,single);
            single.items[0].edges = true;
            const auto edged = captureTransparency(f,single);
            CHECK(edged.draws == 2); // One geometry draw and the existing transparency resolve.
            CHECK(edged.draws == fill.draws);
            CHECK(maximumColorDifference(fill,edged) > 0);
            CHECK(maximumColorDifference(edged,captureTransparency(f,single,false,true,false,true)) <= 3);
            CHECK(maximumColorDifference(edged,captureTransparency(f,single,false,false)) <= 1);
            // Edge coverage changes RGB only: revealage and final alpha must
            // match a surface with no edges, including antialiased boundaries.
            int alphaDifference = 0;
            for (size_t i = 3; i < edged.color.size(); i += 4) {
                alphaDifference = std::max(alphaDifference,std::abs(int(edged.color[i])-int(fill.color[i])));
            }
            CHECK(alphaDifference <= 1);
        }
    }
}

TEST_CASE("Combined transparent edges preserve intersecting order UVs and large index offsets")
{
    for (const bool msaa : {false,true}) {
        CAPTURE(msaa);
        TransparencyFixture f; initializeTransparency(f,msaa);
        auto& mesh = f.models[0].gpuMesh;
        constexpr uint32_t offset = 65538;
        std::vector<uint32_t> indices(offset,0);
        indices.insert(indices.end(),{0,1,2,3,4,5});
        auto replaceIndices = [&] {
            const auto old = mesh.triangleIndexBuffer;
            mesh.triangleIndexBuffer = g::createIndexBuffer(g::copy(indices.data(),
                static_cast<uint32_t>(indices.size()*sizeof(uint32_t))),WOBY_GPU_BUFFER_INDEX32);
            g::destroy(old);
        };
        replaceIndices();
        for (auto& range : mesh.nodeRanges) { range.triangleIndexOffset += offset; }
        CHECK_FALSE(g::isValid(mesh.lineIndexBuffer));
        auto edged = f.plan; for (auto& item : edged.items) { item.edges = true; }
        const auto image = captureTransparency(f,edged);
        CHECK(image.draws == 3); // Two geometry draws share one resolve.
        std::reverse(edged.items.begin(),edged.items.end());
        CHECK(maximumColorDifference(image,captureTransparency(f,edged)) <= 1);
        CHECK(maximumColorDifference(image,captureTransparency(f,edged,false,true,false,true)) <= 3);
        CHECK(maximumColorDifference(image,captureTransparency(f,edged,true)) <= 1);
        CHECK(maximumColorDifference(captureTransparency(f,edged,false,true,true),
            captureTransparency(f,edged,true,true,true)) <= 1);
        for (const bool pulled : {false,true}) {
            CAPTURE(pulled);
            auto oneGroup = f.plan; oneGroup.items.resize(1); oneGroup.items[0].edges = true;
            mesh.nodeRanges[0].triangleIndexCount = 6;
            const auto original = captureTransparency(f,oneGroup,false,true,false,pulled);
            std::swap_ranges(indices.end()-6,indices.end()-3,indices.end()-3);
            replaceIndices();
            CHECK(maximumColorDifference(original,captureTransparency(f,oneGroup,false,true,false,pulled)) <= 1);
            mesh.nodeRanges[0].triangleIndexCount = 3;
        }
        edged = f.plan; for (auto& item : edged.items) { item.edges = true; item.uvGrid = {8,8,1,0}; }
        const auto uv = captureTransparency(f,edged);
        CHECK(maximumColorDifference(image,uv) > 10);
        CHECK(maximumColorDifference(uv,captureTransparency(f,edged,false,true,false,true)) <= 3);
        edged.triangleEdgeXray = true;
        const auto xray = captureTransparency(f,edged);
        CHECK(xray.draws == 5); // X-ray retains independent hardware lines.
        CHECK(xray.color == captureTransparency(f,edged,false,true,false,true).color);
        CHECK(maximumColorDifference(uv,xray) > 10);
    }
}

TEST_CASE("Combined transparent edges preserve opaque occlusion zero opacity and point IDs")
{
    for (const bool msaa : {false,true}) {
        CAPTURE(msaa);
        TransparencyFixture f; initializeTransparency(f,msaa);
        woby::SceneDrawItem marker; bx::mtxIdentity(marker.model.data());
        marker.groupIndex = 2; marker.points = true; marker.pointSize = 12; marker.color = {0,1,0,0};
        auto pointOnly = f.plan; pointOnly.items = {marker};
        const auto referencePoint = captureTransparency(f,pointOnly);
        for (const bool pulled : {false,true}) {
            CAPTURE(pulled);
            auto edged = f.plan; for (auto& item : edged.items) { item.edges = true; }
            edged.items.push_back(marker);
            const auto marked = captureTransparency(f,edged,false,true,false,pulled);
            const size_t center = (39*96+49)*4;
            for (size_t c = 0; c < 4; ++c) {
                CHECK(marked.color[center+c] == referencePoint.color[center+c]);
                CHECK(marked.ids[center+c] == referencePoint.ids[center+c]);
            }
            CHECK(marked.ids[center] != 0);
            for (auto& item : edged.items) if (item.solid) item.color[3] = 0;
            CHECK(captureTransparency(f,edged,false,true,false,pulled).color == referencePoint.color);
            auto covered = f.plan; covered.items[0].color[3] = 1;
            covered.items[0].model[10] = 0; covered.items[0].model[14] = .05f;
            covered.items[1].edges = true; covered.items.push_back(marker);
            const auto opaque = captureTransparency(f,covered,false,true,false,pulled);
            covered.items.resize(1);
            CHECK(opaque.color == captureTransparency(f,covered,false,true,false,pulled).color);
            CHECK(opaque.ids[center] == 0);
        }
    }
}
