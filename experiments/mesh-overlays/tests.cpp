#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "renderer.h"
#include <algorithm>
#include <limits>

namespace o=woby::overlay;
namespace {
std::array<float,16> identity() { std::array<float,16> result{}; bx::mtxIdentity(result.data()); return result; }
size_t difference(const o::Capture& a,const o::Capture& b) {
    size_t count=0;
    for (size_t p=0;p<a.rgba.size();p+=4)
        if (!std::equal(a.rgba.begin()+static_cast<ptrdiff_t>(p),a.rgba.begin()+static_cast<ptrdiff_t>(p+3),b.rgba.begin()+static_cast<ptrdiff_t>(p))) ++count;
    return count;
}
size_t identified(const o::Capture& capture) { return static_cast<size_t>(std::count_if(capture.ids.begin(),capture.ids.end(),[](auto id){return id!=0;})); }
o::Capture capture(o::Renderer& renderer,o::Display display) {
    o::Capture result; const auto measurement=o::render(renderer,display,identity(),&result);
    CHECK(measurement.totalMs>0); CHECK(measurement.draws>0); return result;
}
}

TEST_CASE("Overlay experiment rejects invalid workload settings") {
    CHECK_NOTHROW(o::validate({},{}));
    CHECK_THROWS(o::validate({.width=0},{})); CHECK_THROWS(o::validate({.samples=2},{}));
    CHECK_THROWS(o::validate({},{.pointSize=0})); CHECK_THROWS(o::validate({},{.pointSize=41}));
    CHECK_THROWS(o::validate({},{.opacity=std::numeric_limits<float>::quiet_NaN()}));
    CHECK_THROWS(o::validate({},{.edgeHalfWidth=-1}));
    CHECK_THROWS(o::fittedProjection({},0,720));
}
TEST_CASE("Overlay GPU surfaces-first rendering is independent of opaque group order") {
    for (const uint32_t samples:{1u,4u}) {
        o::Renderer r; o::initialize(r,{128,96,samples,true}); o::upload(r,o::fixtureMesh());
        for (const auto method:{o::Method::ordered,o::Method::barycentric,o::Method::pulled}) {
            INFO(o::methodName(method), " samples=", samples);
            if (method==o::Method::barycentric && !gpu::get_device_caps(r.device).fragment_barycentric) continue;
            const o::Display display{.method=method,.pointSize=8};
            const auto a=capture(r,display);
            std::reverse(r.scene.groups.begin(),r.scene.groups.end());
            const auto b=capture(r,display);
            CHECK(difference(a,b)==0); CHECK(a.ids==b.ids);
            std::reverse(r.scene.groups.begin(),r.scene.groups.end());
        }
    }
}
TEST_CASE("Overlay GPU control shaders retain complete marker circles and IDs") {
    for (const uint32_t samples:{1u,4u}) {
        o::Renderer r; o::initialize(r,{128,96,samples,true}); o::upload(r,o::fixtureMesh());
        size_t previous=0;
        for (const float size:{1.0f,4.0f,8.0f,40.0f}) {
            o::Display d{.solid=false,.edges=false,.pointSize=size};
            const auto ordered=capture(r,d);
            d.method=o::Method::legacy;
            const auto legacy=capture(r,d);
            CHECK(difference(ordered,legacy)==0); CHECK(ordered.ids==legacy.ids);
            const auto area=identified(ordered); CHECK(area>previous); previous=area;
            CHECK(std::all_of(ordered.ids.begin(),ordered.ids.end(),[](auto id){return id<=7;}));
            d.method=o::Method::pulled;
            const auto pulled=capture(r,d); CHECK(difference(pulled,legacy)==0); CHECK(pulled.ids==legacy.ids);
        }
    }
}
TEST_CASE("Overlay GPU X-ray exposes occluded edges while hidden-line mode keeps the background") {
    o::Renderer r; o::initialize(r,{128,96,4,true}); o::upload(r,o::fixtureMesh());
    r.scene.groups[1].edges=false;
    o::Display d{.points=false};
    const auto visible=capture(r,d);
    d.xray=true; const auto xray=capture(r,d);
    CHECK(difference(visible,xray)>0);
    // The rear triangle's top edge passes behind the front rectangle.
    d.xray=false; d.solid=false;
    const auto lines=capture(r,d);
    CHECK(difference(lines,visible)>1000);
    CHECK(identified(lines)==0);
}
TEST_CASE("Overlay GPU native and pulled barycentrics agree on edges and preserve solid control") {
    for (const uint32_t samples:{1u,4u}) {
        o::Renderer r; o::initialize(r,{128,96,samples,true}); o::upload(r,o::fixtureMesh());
        for (const bool solid:{false,true}) {
            const o::Display d{.method=o::Method::pulled,.solid=solid,.points=false};
            const auto pulled=capture(r,d);
            if (gpu::get_device_caps(r.device).fragment_barycentric) {
                auto nativeDisplay=d; nativeDisplay.method=o::Method::barycentric;
                const auto native=capture(r,nativeDisplay);
                CHECK(difference(pulled,native)<40); CHECK(native.ids==pulled.ids);
            }
        }
        const auto legacy=capture(r,{.method=o::Method::legacy,.edges=false,.points=false});
        const auto solid=capture(r,{.method=o::Method::pulled,.edges=false,.points=false});
        CHECK(difference(legacy,solid)==0);
        auto transparent=capture(r,{.method=o::Method::pulled,.opacity=.4f});
        CHECK(difference(transparent,solid)>100); CHECK(identified(transparent)>0);
    }
}

TEST_CASE("Overlay GPU surfaces occlude marker IDs and transformed near-plane clipping keeps complete circles") {
    auto mesh=o::fixtureMesh();
    woby::Vertex hidden; hidden.position={0,0,.3f}; hidden.normal={0,0,1};
    mesh.vertices.push_back(hidden); mesh.pointIndices={7};
    woby::MeshNode node; node.name="hidden marker"; node.pointIndexCount=1; mesh.nodes.push_back(node);
    for (const uint32_t samples:{1u,4u}) {
        o::Renderer r; o::initialize(r,{128,96,samples,true}); o::upload(r,mesh);
        const auto visible=capture(r,{.edges=false,.pointSize=8});
        CHECK(std::count(visible.ids.begin(),visible.ids.end(),8u)==0);
        const auto alone=capture(r,{.solid=false,.edges=false,.pointSize=8});
        CHECK(std::count(alone.ids.begin(),alone.ids.end(),8u)>20);
        // Rear triangle moves beyond the [0,1] clip interval. The front markers
        // remain visible, including the parts of their circles outside faces.
        r.scene.groups[0].model[14]=1.0f;
        r.scene.groups[2].points=false;
        const auto clipped=capture(r,{.solid=false,.edges=false,.pointSize=40});
        CHECK(std::none_of(clipped.ids.begin(),clipped.ids.end(),[](auto id){return id>=1 && id<=3;}));
        CHECK(identified(clipped)>2000);
        const auto control=capture(r,{.method=o::Method::legacy,.solid=false,.edges=false,.pointSize=40});
        CHECK(difference(control,clipped)==0); CHECK(control.ids==clipped.ids);
    }
}
