#include "analysis_display.h"
#include "graphics_helpers.h"
#include "math_utils.h"
#include "marker_pick.h"
#include <doctest/doctest.h>
#include <algorithm>

namespace {
namespace g = woby::graphics;
struct DisplayFixture {
    bool initialized = false;
    g::ProgramHandle comparison{}, color{};
    g::TextureHandle output{}, depth{}, ids{};
    g::FrameBufferHandle target{};
    std::vector<g::VertexBufferHandle> vertices;
    std::vector<g::IndexBufferHandle> indices;
    ~DisplayFixture() {
        if (!initialized) { return; }
        for (auto h : vertices) { g::destroy(h); }
        for (auto h : indices) { g::destroy(h); }
        for (auto h : {comparison, color}) { if (g::isValid(h)) { g::destroy(h); } }
        if (g::isValid(target)) { g::destroy(target); }
        for (auto h : {output, depth, ids}) { if (g::isValid(h)) { g::destroy(h); } }
        g::shutdown();
    }
};
void initialize(DisplayFixture& f, bool msaa, bool markers) {
    f.initialized = g::init({}); REQUIRE(f.initialized);
    const std::filesystem::path assets = WOBY_TEST_ASSET_DIRECTORY;
    f.comparison = woby::loadProgram(assets, "vs_comparison.bin", markers ? "fs_marker_comparison.bin" : "fs_comparison.bin");
    f.color = woby::loadProgram(assets, "vs_color.bin", markers ? "fs_marker_line.bin" : "fs_color.bin");
    const auto flags = msaa ? WOBY_GPU_TEXTURE_RT_MSAA_X4 : WOBY_GPU_TEXTURE_RT;
    f.output = g::createTexture2D(128, 96, false, 1, g::TextureFormat::RGBA8, flags | WOBY_GPU_TEXTURE_READ_BACK);
    f.depth = g::createTexture2D(128, 96, false, 1, g::TextureFormat::D24S8, flags);
    if (markers) { f.ids = g::createTexture2D(128, 96, false, 1, g::TextureFormat::RGBA8,
        flags | WOBY_GPU_TEXTURE_READ_BACK | (msaa ? WOBY_GPU_TEXTURE_MSAA_SAMPLE : uint64_t{0})); }
    const std::array attachments{f.output, markers ? f.ids : f.depth, f.depth};
    f.target = g::createFrameBuffer(markers ? 3 : 2, attachments.data());
    g::setViewFrameBuffer(0, f.target); g::setViewRect(0, 0, 0, 128, 96);
    g::setViewClear(0, WOBY_GPU_CLEAR_COLOR | WOBY_GPU_CLEAR_DEPTH, 0x20242aff, 1);
    std::array<float,16> identity{}; woby::identityMatrix(identity.data());
    g::setViewTransform(0, identity.data(), identity.data());
}
template<class T> g::VertexBufferHandle buffer(DisplayFixture& f, const std::vector<T>& values) {
    const auto h = g::createVertexBuffer(g::copy(values.data(), static_cast<uint32_t>(values.size()*sizeof(T))), {sizeof(T)});
    REQUIRE(g::isValid(h)); f.vertices.push_back(h); return h;
}
g::IndexBufferHandle indices(DisplayFixture& f, const std::vector<uint32_t>& values) {
    const auto h = g::createIndexBuffer(g::copy(values.data(), static_cast<uint32_t>(values.size()*sizeof(uint32_t))), WOBY_GPU_BUFFER_INDEX32);
    REQUIRE(g::isValid(h)); f.indices.push_back(h); return h;
}
std::vector<uint8_t> capture(DisplayFixture& f, g::VertexBufferHandle source, g::IndexBufferHandle triangles,
    g::VertexBufferHandle faces, g::FaceDisplayKind kind, uint32_t first, uint32_t count,
    std::array<float,4> parameters, bool wire = false, bool release = false) {
    g::touch(0);
    g::setComparison(parameters);
    const std::array<float,4> color{.3f,.8f,.5f,.45f}; g::setColor(color);
    g::setVertexBuffer(0, source);
    if (wire) {
        if (g::isValid(faces)) { g::setTriangleWireframe(triangles); }
        else { g::setIndexBuffer(triangles); }
    } else if (g::isValid(faces)) { g::setFaceDisplay(faces, triangles, kind, first, count); }
    else { g::setVertexCount(count); }
    auto flags = WOBY_GPU_STATE_WRITE_RGB | WOBY_GPU_STATE_WRITE_A | WOBY_GPU_STATE_WRITE_Z
        | WOBY_GPU_STATE_DEPTH_TEST_LEQUAL | WOBY_GPU_STATE_MSAA;
    if (wire) { flags |= WOBY_GPU_STATE_PT_LINES | WOBY_GPU_STATE_BLEND_ALPHA; }
    woby::setMarkerRenderState(flags, g::isValid(f.ids));
    g::submit(0, wire ? f.color : f.comparison);
    // Submitted operations must retain all three buffers until execution.
    if (release) {
        for (auto h : f.vertices) { g::destroy(h); } f.vertices.clear();
        for (auto h : f.indices) { g::destroy(h); } f.indices.clear();
    }
    std::vector<uint8_t> pixels(128*96*4*(g::isValid(f.ids) ? 2 : 1));
    auto ready = g::readTexture(f.output, pixels.data());
    if (g::isValid(f.ids)) { ready = std::max(ready, g::readTexture(f.ids, pixels.data()+128*96*4)); }
    while (g::frame() < ready) {}
    return pixels;
}
}
TEST_CASE("GPU face displays match expanded quality UV distance and comparison wireframes") {
    for (const bool msaa : {false, true}) for (const bool markers : {false, true}) {
        CAPTURE(msaa); CAPTURE(markers);
        DisplayFixture f; initialize(f, msaa, markers);
        woby::Mesh mesh;
        auto& geometry = woby::editMeshGeometry(mesh);
        geometry.vertices = {{{-1.2f,-.7f,.7f},{0,0,1}}, {{.9f,-.7f,.7f},{0,1,0}}, {{0,.85f,.7f},{1,0,0}},
            {{-.4f,-.4f,.3f},{0,0,1}}, {{.7f,-.4f,.3f},{0,1,0}}, {{.1f,.7f,.3f},{1,0,0}}};
        constexpr uint32_t first = 21846; // First index is above 16 bits.
        geometry.indices.assign(first*3, 0);
        geometry.indices.insert(geometry.indices.end(), {0,1,2,3,4,5});
        const auto source = buffer(f, geometry.vertices);
        const auto triangleBuffer = indices(f, geometry.indices);
        std::vector<woby::SurfaceQualityFace> quality(first+2);
        quality[first] = {{.1f,.3f,-1,-2},{0,0,1},0}; quality[first+1] = {{.9f,.7f,.5f,.2f},{0,1,0},0};
        std::vector<woby::UvQualityFace> uv(first+2);
        std::vector<woby::DistanceFace> distance(first+2);
        std::vector<woby::Vertex> expandedQuality, expandedUv, expandedDistance;
        std::vector<uint32_t> edges;
        for (size_t face = first; face < first+2; ++face) {
            uv[face].value = {face == first ? -.2f : .8f, face == first ? 0.f : 1.f};
            for (size_t k = 0; k < 3; ++k) {
                const auto a = geometry.indices[face*3+k], b = geometry.indices[face*3+(k+1)%3];
                auto vertex = geometry.vertices[a];
                vertex.normal = quality[face].normal;
                vertex.texcoord = k < 2 ? std::array<float,2>{quality[face].values[k*2],quality[face].values[k*2+1]} : std::array<float,2>{};
                expandedQuality.push_back(vertex);
                vertex = geometry.vertices[a]; uv[face].normals[k] = vertex.normal;
                vertex.texcoord = uv[face].value; expandedUv.push_back(vertex);
                edges.insert(edges.end(), {a,b});
                for (size_t axis = 0; axis < 3; ++axis) {
                    distance[face].midpoints[k][axis] = (geometry.vertices[a].position[axis]+geometry.vertices[b].position[axis])*.5f;
                }
            }
            distance[face].distances = {.1f,.4f,.7f,1}; distance[face].normals.fill({0,0,1});
            for (size_t corner = 0; corner < 12; ++corner) { expandedDistance.push_back(woby::distanceDisplayVertex(mesh,distance,face*12+corner)); }
        }
        const auto q = buffer(f,quality), u = buffer(f,uv), d = buffer(f,distance);
        const auto eq = buffer(f,expandedQuality), eu = buffer(f,expandedUv), ed = buffer(f,expandedDistance);
        for (uint32_t metric = 0; metric < 4; ++metric) {
            const std::array<float,4> parameters{static_cast<float>(metric),0,2,0};
            const auto reference = capture(f,eq,{}, {},g::FaceDisplayKind::surfaceQuality,0,6,parameters);
            CHECK(reference == capture(f,source,triangleBuffer,q,g::FaceDisplayKind::surfaceQuality,first,2,parameters));
            CHECK(reference != capture(f,source,triangleBuffer,q,g::FaceDisplayKind::surfaceQuality,first,0,parameters));
        }
        CHECK(capture(f,eu,{}, {},g::FaceDisplayKind::uvQuality,0,6,{0,1,6,0}) ==
            capture(f,source,triangleBuffer,u,g::FaceDisplayKind::uvQuality,first,2,{0,1,6,0}));
        CHECK(capture(f,ed,{}, {},g::FaceDisplayKind::distance,0,24,{.2f,1,1,0}) ==
            capture(f,source,triangleBuffer,d,g::FaceDisplayKind::distance,first,2,{.2f,1,1,0}));
        const auto edgeBuffer = indices(f,edges);
        // Leading triangles are collapsed, so the complete wire draw has the same visible edges.
        CHECK(capture(f,source,edgeBuffer,{},g::FaceDisplayKind::surfaceQuality,0,0,{},true) ==
            capture(f,source,triangleBuffer,q,g::FaceDisplayKind::surfaceQuality,0,0,{},true));
        CHECK_THROWS(g::setFaceDisplay(q,triangleBuffer,g::FaceDisplayKind::uvQuality));
        CHECK_THROWS(g::setFaceDisplay(q,triangleBuffer,g::FaceDisplayKind::surfaceQuality,first+3));
        CHECK_THROWS(g::setFaceDisplay(q,edgeBuffer,g::FaceDisplayKind::surfaceQuality));
        const auto reference = capture(f,ed,{}, {},g::FaceDisplayKind::distance,0,24,{.2f,1,1,0});
        CHECK(reference == capture(f,source,triangleBuffer,d,g::FaceDisplayKind::distance,first,2,{.2f,1,1,0},false,true));
    }
}
