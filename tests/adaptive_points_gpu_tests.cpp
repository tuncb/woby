#include "adaptive_points.h"
#include "graphics_helpers.h"
#include "marker_pick.h"
#include <doctest/doctest.h>
#include <bit>
#include <cmath>
#include <cstring>

namespace {
namespace g=woby::graphics;
struct PointGpuFixture {
    bool initialized=false;
    std::vector<woby::LoadedModelRuntime> models;
    woby::Mesh source;
    woby::AdaptivePointRuntime runtime;
    woby::SceneDrawPlan plan;
    woby::ScenePickView view;
    g::ProgramHandle point{},mesh{};
    g::UniformHandle color{},params{},base{},uv{};
    g::TextureHandle output{},depth{},ids{};
    g::FrameBufferHandle framebuffer{};
    woby::MarkerDrawContext markers;
    uint32_t samples=1;
    ~PointGpuFixture() {
        if (!initialized) return;
        woby::destroyAdaptivePoints(runtime); woby::destroyModelRuntimes(models);
        for (auto h:{point,mesh}) if (g::isValid(h)) g::destroy(h);
        for (auto h:{color,params,base,uv}) if (g::isValid(h)) g::destroy(h);
        for (auto h:{output,depth,ids}) if (g::isValid(h)) g::destroy(h);
        if (g::isValid(framebuffer)) g::destroy(framebuffer);
        g::shutdown();
    }
};
bool initialize(PointGpuFixture& f,uint32_t samples,bool requireAtomics=true,g::PointBackend backend=g::PointBackend::automatic) {
    f.initialized=g::init({}); REQUIRE(f.initialized);
    f.runtime.preference=backend;
    const auto selected=g::selectPointBackend(g::getCaps()->supported,backend);
    if (requireAtomics && (selected==g::PointBackend::quads || (backend!=g::PointBackend::automatic && selected!=backend))) {
        MESSAGE("Requested optional point compute backend is unavailable on this device.");
        return false;
    }
    f.samples=samples;
    const std::filesystem::path assets=WOBY_TEST_ASSET_DIRECTORY;
    f.point=woby::loadProgram(assets,"vs_point_sprite.bin","fs_marker_point.bin");
    f.mesh=woby::loadProgram(assets,"vs_color.bin","fs_marker_line.bin");
    f.color=g::createUniform("u_color",g::UniformType::Vec4);
    f.params=g::createUniform("u_pointParams",g::UniformType::Vec4,2);
    f.base=g::createUniform("u_markerBase",g::UniformType::Vec4); f.markers.baseUniform=f.base;
    f.uv=g::createUniform("u_uvGrid",g::UniformType::Vec4);
    f.view.width=37; f.view.height=29;
    bx::mtxIdentity(f.view.view.data()); bx::mtxIdentity(f.view.renderProjection.data());
    const auto flags=samples==4?WOBY_GPU_TEXTURE_RT_MSAA_X4:WOBY_GPU_TEXTURE_RT;
    f.output=g::createTexture2D(43,37,false,1,g::TextureFormat::RGBA8,flags|WOBY_GPU_TEXTURE_READ_BACK);
    f.depth=g::createTexture2D(43,37,false,1,g::TextureFormat::D24S8,flags);
    f.ids=g::createTexture2D(43,37,false,1,g::TextureFormat::RGBA8,flags|WOBY_GPU_TEXTURE_READ_BACK);
    const std::array targets{f.output,f.ids,f.depth}; f.framebuffer=g::createFrameBuffer(3,targets.data());
    g::setViewFrameBuffer(0,f.framebuffer); g::setViewRect(0,3,5,37,29);
    g::setViewClear(0,WOBY_GPU_CLEAR_COLOR|WOBY_GPU_CLEAR_DEPTH,0x000000ff,1);
    g::setViewTransform(0,f.view.view.data(),f.view.renderProjection.data(),true);
    auto& source=f.source;
    for (uint32_t i=0;i<257;++i) {
        woby::Vertex v; v.position={float(int(i*17%109)-54)/43,float(int(i*23%79)-39)/34,float(i%13)/12};
        source.vertices.push_back(v); source.pointIndices.push_back(i);
    }
    source.vertices[256].position=source.vertices[255].position; // Stable equal-depth tie.
    source.nodes={{"first"},{"second"}}; source.nodes[0].pointIndexCount=131;
    source.nodes[1].pointIndexOffset=131; source.nodes[1].pointIndexCount=126;
    auto prepared=woby::prepareSceneMesh(source,woby::gpuMeshPoints,{},false); REQUIRE(prepared);
    prepared->pointCloud=std::make_shared<const woby::points::Cloud>(woby::points::buildCloud(source,32,8));
    prepared->pointVertexIndices.clear(); prepared->compactOnly=true;
    auto upload=woby::beginGpuMeshUpload(std::move(*prepared));
    while (!woby::stepGpuMeshUpload(upload,source,woby::meshVertexLayout(),64)) g::frame();
    f.models.push_back({std::move(upload.mesh)});
    CHECK_FALSE(g::isValid(f.models[0].gpuMesh.vertexBuffer)); CHECK_FALSE(g::isValid(f.models[0].gpuMesh.pointIdBuffer));
    for (size_t i=0;i<2;++i) {
        woby::SceneDrawItem item; item.groupIndex=i; item.points=true; item.pointSize=8;
        item.color=i?std::array<float,4>{0,.6f,.2f,1}:std::array<float,4>{.6f,0,.2f,1};
        bx::mtxIdentity(item.model.data()); item.model[0]=.81f; item.model[5]=1.07f; item.model[12]=i?.1f:-.03f;
        f.plan.items.push_back(item);
    }
    return true;
}
void draw(PointGpuFixture& f,double now,bool adaptive=false,bool query=false,std::array<float,2> position={},bool markerIds=true) {
    f.markers.list={};
    g::setViewRect(0,3,5,static_cast<uint16_t>(f.view.width),static_cast<uint16_t>(f.view.height));
    g::setViewTransform(0,f.view.view.data(),f.view.renderProjection.data(),true); g::touch(0);
    woby::prepareAdaptivePoints(f.runtime,WOBY_TEST_ASSET_DIRECTORY,f.view,adaptive,now,query,position);
    woby::submitSceneFiles(0,f.plan,f.models,f.mesh,f.uv,f.mesh,f.point,f.color,f.params,{},f.view.width,f.view.height,
        markerIds?&f.markers:nullptr,false,&f.runtime);
}
std::vector<uint64_t> winners(PointGpuFixture& f) {
    std::vector<uint64_t> values(size_t(f.view.width)*f.view.height*4);
    const auto ready=g::readBuffer(f.runtime.winners,values.data()); while (g::frame()<ready) {}
    values.resize(size_t(f.view.width)*f.view.height*f.samples); return values;
}
std::vector<uint32_t> reference(const PointGpuFixture& f) {
    const auto& cloud=*f.models[0].gpuMesh.pointCloud;
    std::vector<uint32_t> ids(size_t(f.view.width)*f.view.height*f.samples);
    std::vector<float> depths(ids.size());
    const std::array<std::array<float,2>,4> locations={{{.375f,.125f},{.875f,.375f},{.125f,.625f},{.625f,.875f}}};
    uint32_t first=1;
    for (const auto& item:f.plan.items) {
        const auto& group=cloud.groups[item.groupIndex];
        for (const auto& p:cloud.points) {
            if (p.id<group.firstId || p.id>=group.endId) continue;
            const auto world=woby::transformMarkerPosition(item.model,p.position);
            const auto clip=woby::transformMarkerPosition(f.view.view,world);
            if (clip[2]<0 || clip[2]>1) continue;
            const float x=(clip[0]*.5f+.5f)*float(f.view.width),y=(.5f-clip[1]*.5f)*float(f.view.height);
            for (uint32_t row=0;row<f.view.height;++row) for (uint32_t col=0;col<f.view.width;++col) for (uint32_t sample=0;sample<f.samples;++sample) {
                const auto at=f.samples==1?std::array<float,2>{.5f,.5f}:locations[sample];
                const float dx=float(col)+at[0]-x,dy=float(row)+at[1]-y;
                const auto index=(size_t(row)*f.view.width+col)*f.samples+sample; const auto id=first+p.id-group.firstId;
                if (dx*dx+dy*dy>item.pointSize*item.pointSize*.25f) continue;
                if (clip[2]>depths[index] || (clip[2]==depths[index] && id>ids[index])) { ids[index]=id; depths[index]=clip[2]; }
            }
        }
        first+=group.endId-group.firstId;
    }
    return ids;
}
void checkIds(const std::vector<uint64_t>& actual,const std::vector<uint32_t>& expected) {
    REQUIRE(actual.size()==expected.size());
    size_t differences=0;
    for (size_t i=0;i<actual.size();++i) if (uint32_t(actual[i])!=expected[i]) ++differences;
    CHECK(differences==0);
}
void checkColors(PointGpuFixture& f,const std::vector<uint32_t>& ids,const std::array<float,3>& background={}) {
    std::vector<uint8_t> pixels(43*37*4);
    const auto ready=g::readTexture(f.output,pixels.data()); while (g::frame()<ready) {}
    size_t differences=0;
    for (uint32_t y=0;y<f.view.height;++y) for (uint32_t x=0;x<f.view.width;++x) for (size_t channel=0;channel<3;++channel) {
        float sum=0;
        for (uint32_t sample=0;sample<f.samples;++sample) {
            const auto id=ids[(size_t(y)*f.view.width+x)*f.samples+sample];
            if (!id) { sum+=std::round(background[channel]*255); continue; }
            const auto* draw=woby::findMarkerDraw(f.markers.list.draws,id); REQUIRE(draw);
            const auto& item=f.plan.items[static_cast<size_t>(draw-f.markers.list.draws.data())];
            sum+=std::round(std::min(1.0f,item.color[channel]*1.5f)*255);
        }
        const auto actual=pixels[((size_t(y)+5)*43+x+3)*4+channel];
        if (std::abs(float(actual)-sum/float(f.samples))>1.1f) ++differences;
    }
    CHECK(differences==0);
}
}
TEST_CASE("Integrated opaque point GPU footprints IDs refinement and invalidation match full source") {
    for (const auto backend:{g::PointBackend::atomic32,g::PointBackend::atomic64}) for (const uint32_t samples:{1u,4u}) {
        INFO("backend=" << int(backend) << ", samples=" << samples);
        PointGpuFixture f; if (!initialize(f,samples,true,backend)) continue;
        f.runtime.budget=73;
        for (const float size:{1.0f,4.0f,8.0f,40.0f}) {
            for (auto& item:f.plan.items) item.pointSize=size;
            draw(f,1); const auto expected=reference(f); checkIds(winners(f),expected); checkColors(f,expected);
            draw(f,2); CHECK(f.runtime.submitted==0); checkIds(winners(f),reference(f));
        }
        f.plan.items[0].pointSize=4; f.plan.items[1].pointSize=8;
        f.view.view[12]=-.23f; draw(f,3); checkIds(winners(f),reference(f));
        f.plan.items[1].model[13]=.12f; draw(f,4); checkIds(winners(f),reference(f));
        f.view.width=31; f.view.height=23; draw(f,5); checkIds(winners(f),reference(f));
        f.plan.items.erase(f.plan.items.begin()); draw(f,6); checkIds(winners(f),reference(f));
        CHECK(f.runtime.budget==73); // Full-detail GPU timings must not change the navigation budget.
        CHECK_FALSE(g::getStats()->pointRasterBudgeted);
        f.runtime.budget=73; draw(f,7,true); (void)winners(f);
        for (uint32_t i=0;i<4 && f.runtime.refined<f.runtime.total;++i) { draw(f,8+i,true); (void)winners(f); }
        CHECK(f.runtime.refined==f.runtime.total); checkIds(winners(f),reference(f));
        const auto epoch=f.runtime.epoch;
        for (const float alpha:{.35f,0.0f,1.0f}) {
            f.plan.items[0].color[3]=alpha; draw(f,20+alpha,true);
            REQUIRE(f.runtime.enabled); REQUIRE(f.runtime.active);
            CHECK(f.runtime.epoch==epoch); CHECK(f.runtime.submitted==0);
            checkIds(winners(f),reference(f)); checkColors(f,reference(f));
        }
    }
}
TEST_CASE("Integrated reduced point GPU picking queries all original circle footprints") {
    for (const auto backend:{g::PointBackend::atomic32,g::PointBackend::atomic64}) {
    PointGpuFixture f; if (!initialize(f,4,true,backend)) continue;
    f.runtime.budget=1; draw(f,0,true,true,{19.2f,13.4f});
    const auto actual=winners(f); const auto expected=reference(f);
    // The query radius encompasses this small viewport; full IDs must already
    // match even though stationary refinement has not started.
    REQUIRE(f.runtime.refined==0);
    for (uint32_t y=5;y<23;++y) for (uint32_t x=11;x<27;++x) for (uint32_t sample=0;sample<4;++sample) {
        const auto i=(size_t(y)*f.view.width+x)*4+sample; CHECK(uint32_t(actual[i])==expected[i]);
    }
    }
}
TEST_CASE("Transparent surfaces retain adaptive point rendering cached visibility and picking") {
    for (const auto backend:{g::PointBackend::atomic32,g::PointBackend::atomic64})
        for (const uint32_t samples:{1u,4u}) for (const bool adaptive:{false,true}) {
        INFO("backend=" << int(backend) << ", samples=" << samples << ", adaptive=" << adaptive);
        PointGpuFixture f; if (!initialize(f,samples,true,backend)) continue;
        f.view.view[10]=.5f;
        const auto expected=reference(f);
        draw(f,0,adaptive); (void)winners(f);
        if (f.runtime.pending.valid()) f.runtime.pending.wait();
        draw(f,1,adaptive); checkIds(winners(f),expected);
        REQUIRE(f.runtime.refined==f.runtime.total);
        const auto epoch=f.runtime.epoch;
        const auto captureIds=[&] {
            std::vector<uint8_t> pixels(43*37*4);
            const auto ready=g::readTexture(f.ids,pixels.data()); while (g::frame()<ready) {}
            return pixels;
        };
        const auto pointIds=captureIds();
        REQUIRE(std::any_of(pointIds.begin(),pointIds.end(),[](uint8_t value) { return value!=0; }));

        woby::Mesh surface;
        for (const auto& position:std::array<std::array<float,3>,4>{{{-1,-1,1.8f},{1,-1,1.8f},{1,1,1.8f},{-1,1,1.8f}}}) {
            woby::Vertex vertex; vertex.position=position; surface.vertices.push_back(vertex);
        }
        surface.indices={0,1,2,0,2,3}; surface.nodes={{"surface",0,6}};
        f.models.push_back({woby::createGpuMesh(surface,woby::meshVertexLayout(),woby::gpuMeshPoints)});
        woby::SceneDrawItem item; item.fileIndex=1; item.solid=true; item.color={0,0,1,.35f};
        bx::mtxIdentity(item.model.data()); f.plan.items.push_back(item);
        double now=2;
        for (const float alpha:{.35f,.6f,1.0f,.2f}) {
            f.plan.items.back().color[3]=alpha;
            draw(f,now++,adaptive);
            REQUIRE(f.runtime.enabled); REQUIRE(f.runtime.active);
            CHECK(f.runtime.epoch==epoch); CHECK(f.runtime.submitted==0);
            checkIds(winners(f),expected);
            if (alpha<.999f) {
                // Preserve markers-last color and IDs even behind the surface.
                checkColors(f,expected,{0,0,alpha}); CHECK(captureIds()==pointIds);
            } else {
                checkColors(f,std::vector<uint32_t>(expected.size()),{0,0,1});
                const auto ids=captureIds();
                CHECK(std::all_of(ids.begin(),ids.end(),[](uint8_t value) { return value==0; }));
            }
        }
        // Enabling vertices on a small translucent mesh keeps the cloud optimized.
        f.plan.items.back().points=true;
        draw(f,now++,adaptive); REQUIRE(f.runtime.enabled); REQUIRE(f.runtime.active);
        CHECK(f.runtime.epoch==epoch); CHECK(f.runtime.submitted==0);
        g::frame();
        f.plan.items.back().points=false;
        draw(f,now,adaptive); REQUIRE(f.runtime.active); CHECK(f.runtime.submitted==0);
        checkIds(winners(f),expected); CHECK(captureIds()==pointIds);
    }
}
TEST_CASE("Integrated cached points obey current opaque surfaces and color-only viewport offsets") {
    for (const auto backend:{g::PointBackend::atomic32,g::PointBackend::atomic64}) {
    PointGpuFixture f; if (!initialize(f,4,true,backend)) continue;
    g::destroy(f.framebuffer);
    const std::array targets{f.output,f.depth}; f.framebuffer=g::createFrameBuffer(2,targets.data());
    g::setViewFrameBuffer(0,f.framebuffer);
    f.view.view[10]=.5f;
    draw(f,0,false,false,{},false); checkIds(winners(f),reference(f));
    const auto surface=woby::loadProgram(WOBY_TEST_ASSET_DIRECTORY,"vs_color.bin","fs_color.bin");
    g::TransientVertexBuffer vertices;
    g::allocTransientVertexBuffer(&vertices,3,woby::helperLineVertexLayout());
    const std::array<std::array<float,3>,3> triangle={{{-1,-1,1},{1,-1,1},{0,1,1}}};
    std::memcpy(vertices.data,triangle.data(),sizeof(triangle));
    std::array<float,16> identity; bx::mtxIdentity(identity.data());
    // The view halves Z; choose a surface at .9 after that transform.
    identity[14]=.8f;
    const std::array<float,4> blue{0,0,1,1};
    g::setTransform(identity.data()); g::setUniform(f.color,blue.data()); g::setVertexBuffer(0,&vertices);
    g::setState(WOBY_GPU_STATE_WRITE_RGB|WOBY_GPU_STATE_WRITE_A|WOBY_GPU_STATE_WRITE_Z|WOBY_GPU_STATE_DEPTH_TEST_LESS);
    g::submit(0,surface);
    draw(f,1,false,false,{},false); CHECK(f.runtime.submitted==0);
    std::vector<uint8_t> pixels(43*37*4);
    const auto ready=g::readTexture(f.output,pixels.data()); while (g::frame()<ready) {}
    const auto center=((size_t(14)+5)*43+18+3)*4;
    CHECK(pixels[center]==0); CHECK(pixels[center+1]==0); CHECK(pixels[center+2]==255);
    draw(f,2,false,false,{},false); CHECK(f.runtime.submitted==0);
    const auto restored=g::readTexture(f.output,pixels.data()); while (g::frame()<restored) {}
    CHECK(pixels[center]+pixels[center+1]>0);
    g::destroy(surface);
    }
}
TEST_CASE("Compact point fallback preserves full-source colors and valid original picking IDs") {
    for (const uint32_t samples:{1u,4u}) for (const float alpha:{1.0f,.35f,0.0f}) {
        PointGpuFixture f; if (!initialize(f,samples,false)) return;
        f.runtime.unavailable=true;
        const auto capture=[&] {
            draw(f,0); CHECK_FALSE(f.runtime.active);
            std::vector<uint8_t> pixels(43*37*8);
            (void)g::readTexture(f.output,pixels.data());
            const auto ready=g::readTexture(f.ids,pixels.data()+43*37*4); while (g::frame()<ready) {}
            return pixels;
        };
        const auto opaque=capture();
        for (auto& item:f.plan.items) item.color[3]=alpha;
        const auto compact=capture();
        CHECK(compact==opaque);
        // Hardware ties follow spatial storage order.
        // The ID must still identify a real source footprint; opaque
        // winners must have the nearest depth. They need not choose the same
        // original ID as source-order drawing when several answers overlap.
        if (samples==1) {
            const auto expected=reference(f);
            size_t picked=0;
            for (uint32_t y=0;y<f.view.height;++y) for (uint32_t x=0;x<f.view.width;++x) {
                const auto offset=43*37*4+((size_t(y)+5)*43+x+3)*4;
                const auto id=uint32_t(compact[offset])|(uint32_t(compact[offset+1])<<8)
                    |(uint32_t(compact[offset+2])<<16)|(uint32_t(compact[offset+3])<<24);
                if (!id) continue;
                ++picked; REQUIRE(id<=f.source.vertices.size());
                const auto& item=f.plan.items[id<=131?0:1];
                const auto p=woby::transformMarkerPosition(item.model,f.source.vertices[id-1].position);
                const float dx=(p[0]*.5f+.5f)*float(f.view.width)-(float(x)+.5f);
                const float dy=(.5f-p[1]*.5f)*float(f.view.height)-(float(y)+.5f);
                CHECK(dx*dx+dy*dy<=item.pointSize*item.pointSize*.25f+.0001f);
                const auto nearest=expected[size_t(y)*f.view.width+x]; REQUIRE(nearest);
                CHECK(p[2]==f.source.vertices[nearest-1].position[2]);
            }
            CHECK(picked>0);
        }
        woby::destroyGpuMesh(f.models[0].gpuMesh);
        f.models[0].gpuMesh=woby::createGpuMesh(f.source,woby::meshVertexLayout(),woby::gpuMeshPoints);
        const auto legacy=capture();
        CHECK(std::equal(compact.begin(),compact.begin()+43*37*4,legacy.begin()));
    }
}
TEST_CASE("Portable point batches preserve depth ID pairs across arbitrary refinement order") {
    for (const uint32_t samples:{1u,4u}) {
        PointGpuFixture f; if (!initialize(f,samples,true,g::PointBackend::atomic32)) continue;
        draw(f,0); const auto expected=winners(f); checkIds(expected,reference(f));
        std::vector<g::OpaquePointGroup> groups;
        for (const auto& key:f.runtime.keys) groups.push_back(key.group);
        const auto& mesh=f.models[0].gpuMesh;
        std::vector<g::OpaquePointTask> tasks;
        for (uint32_t i=0;i<mesh.pointCloud->points.size();++i) {
            const auto id=mesh.pointCloud->points[i].id;
            const uint32_t group=id<mesh.pointCloud->groups[0].endId?0u:1u;
            tasks.push_back({mesh.pointChunks[i/woby::pointChunkSize],i%woby::pointChunkSize,1,group,false});
        }
        const g::PointPrograms programs{f.runtime.clear,f.runtime.raster,f.runtime.resolveIds,
            f.runtime.batchClear,f.runtime.ids,f.runtime.merge};
        for (int order=0;order<3;++order) {
            if (order==1) std::reverse(tasks.begin(),tasks.end());
            if (order==2) std::rotate(tasks.begin(),tasks.begin()+73,tasks.end());
            for (size_t first=0;first<tasks.size();first+=17) {
                const auto count=std::min<size_t>(17,tasks.size()-first);
                g::submitOpaquePoints(0,g::PointBackend::atomic32,f.runtime.winners,f.runtime.batch,programs,
                    groups,std::span(tasks).subspan(first,count),{},first==0,true);
                g::frame();
            }
            CHECK(winners(f)==expected); // Includes depth bits, ties, and depth zero.
        }
        if (g::getCaps()->supported&WOBY_GPU_CAPS_OPAQUE_POINTS) {
            f.runtime.preference=g::PointBackend::atomic64; draw(f,1);
            CHECK(winners(f)==expected);
        }
        // Backend changes must reset visibility, including switching back from
        // a hardware path that has no persistent winner buffer.
        f.runtime.preference=g::PointBackend::quads; draw(f,2); g::frame();
        CHECK_FALSE(g::isValid(f.runtime.winners)); CHECK_FALSE(g::isValid(f.runtime.batch));
        f.runtime.preference=g::PointBackend::atomic32; draw(f,3);
        CHECK(winners(f)==expected);
    }
}

TEST_CASE("Adaptive hardware points redraw navigation cuts and retain full source cursor picking") {
    for (const uint32_t samples:{1u,4u}) {
        PointGpuFixture f; if (!initialize(f,samples,false,g::PointBackend::quads)) continue;
        const auto capture=[&] {
            std::vector<uint8_t> pixels(43*37*8);
            (void)g::readTexture(f.output,pixels.data());
            const auto ready=g::readTexture(f.ids,pixels.data()+43*37*4);
            while (g::frame()<ready) {}
            return pixels;
        };
        draw(f,0); const auto full=capture();
        CHECK(f.runtime.active); CHECK(f.runtime.refined==257);
        f.runtime.unavailable=true; draw(f,0); CHECK(capture()==full);
        // Leave room for the hierarchy's coverage/extrema floor in both groups.
        f.runtime.unavailable=false; f.runtime.budget=64;
        f.runtime.lastTiming=g::getStats()->pointRasterFrame;
        draw(f,1,true);
        CHECK(f.runtime.active); CHECK(f.runtime.submitted>0); CHECK(f.runtime.submitted<=64);
        CHECK_FALSE(g::isValid(f.runtime.winners)); CHECK_FALSE(g::isValid(f.runtime.batch));
        (void)capture();
        for (const double now:{1.01,1.02}) {
            f.runtime.lastTiming=g::getStats()->pointRasterFrame;
            draw(f,now,true,true,{19.2f,13.4f});
            CHECK(f.runtime.refined==0); CHECK(f.runtime.submitted>0);
            const auto queried=capture();
            // Original points are redrawn in the query region every frame,
            // even with an unchanged camera and pointer.
            for (uint32_t y=5;y<23;++y) for (uint32_t x=11;x<27;++x) for (size_t channel=0;channel<3;++channel) {
                const auto index=((size_t(y)+5)*43+x+3)*4+channel;
                CHECK(queried[index]==full[index]);
            }
            if (samples==1) {
                const auto expected=reference(f);
                for (uint32_t y=5;y<23;++y) for (uint32_t x=11;x<27;++x) {
                    const auto nearest=expected[size_t(y)*f.view.width+x];
                    const auto index=43*37*4+((size_t(y)+5)*43+x+3)*4;
                    const uint32_t id=uint32_t(queried[index])|(uint32_t(queried[index+1])<<8)
                        |(uint32_t(queried[index+2])<<16)|(uint32_t(queried[index+3])<<24);
                    if (!nearest) { CHECK(id==0); continue; }
                    REQUIRE(id>0); REQUIRE(id<=f.source.vertices.size());
                    CHECK(f.source.vertices[id-1].position[2]==f.source.vertices[nearest-1].position[2]);
                    const auto& item=f.plan.items[id<=131?0:1];
                    const auto p=woby::transformMarkerPosition(item.model,f.source.vertices[id-1].position);
                    const float dx=(p[0]*.5f+.5f)*float(f.view.width)-(float(x)+.5f);
                    const float dy=(.5f-p[1]*.5f)*float(f.view.height)-(float(y)+.5f);
                    CHECK(dx*dx+dy*dy<=item.pointSize*item.pointSize*.25f+.0001f);
                }
            }
        }
        draw(f,2,true); CHECK(f.runtime.refined==f.runtime.total); CHECK(capture()==full);
        draw(f,3,true); CHECK(f.runtime.submitted==257); CHECK(capture()==full);
    }
}
