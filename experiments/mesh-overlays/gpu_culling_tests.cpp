#include <doctest/doctest.h>
#include "renderer.h"
#include "visibility.h"
#include <algorithm>
#include <numeric>

namespace o=woby::overlay;
namespace {
std::array<float,16> identity() { std::array<float,16> result{}; bx::mtxIdentity(result.data()); return result; }
void addPoints(woby::Mesh& mesh,const std::vector<std::array<float,3>>& positions) {
    woby::MeshNode node; node.name="culling markers";
    node.pointIndexOffset=static_cast<uint32_t>(mesh.pointIndices.size());
    node.pointIndexCount=static_cast<uint32_t>(positions.size());
    for (const auto& p:positions) {
        woby::Vertex vertex; vertex.position=p; vertex.normal={0,0,1};
        mesh.pointIndices.push_back(static_cast<uint32_t>(mesh.vertices.size())); mesh.vertices.push_back(vertex);
    }
    mesh.nodes.push_back(node);
}
void verify(o::Renderer& r,const woby::Mesh& mesh,o::Display d,const std::array<float,16>& projection) {
    o::Capture baseline; (void)o::render(r,d,projection,&baseline);
    o::Capture depth; depth.depthRequested=true;
    auto surfaces=d; surfaces.points=false; (void)o::render(r,surfaces,projection,&depth);
    const auto audit=o::inspectVisibility(mesh,r.scene,projection,r.options.width,r.options.height,d.pointSize,depth.minimumSampleDepth);
    for (const auto mode:{o::Culling::split,o::Culling::frustum,o::Culling::footprint}) {
        CAPTURE(mode); d.culling=mode;
        o::Capture result; result.cullingSelectionRequested=true;
        const auto timing=o::render(r,d,projection,&result);
        CHECK(result.rgba==baseline.rgba); CHECK(result.sampleIds==baseline.sampleIds);
        CHECK(timing.totalMs>0);
        if (mode==o::Culling::split) continue;
        const auto& selection=mode==o::Culling::frustum?audit.frustum:audit.conservative;
        CHECK(result.cullingSelection==selection.markers);
        REQUIRE(result.cullingCounts.size()==selection.groups.size());
        for (size_t i=0;i<selection.groups.size();++i) CHECK(result.cullingCounts[i]==selection.groups[i].count);
    }
}
}

TEST_CASE("Overlay GPU compaction preserves complete circles, sample IDs, and translucent contributions") {
    auto mesh=o::fixtureMesh();
    addPoints(mesh,{{0,0,.3f},{.405f,0,.3f},{1.02f,0,.7f},{0,0,1.1f},{.405f,0,.3f},
        {-1.02f,0,.7f},{0,1.02f,.7f},{0,-1.02f,.7f},{0,0,-.1f},{0,0,.6f}});
    for (const uint32_t samples:{1u,4u}) for (const bool ids:{false,true}) {
        CAPTURE(samples); CAPTURE(ids);
        o::Renderer r; o::initialize(r,{127,95,samples,ids}); o::upload(r,mesh);
        r.scene.groups[0].points=false; r.scene.groups[1].points=false;
        for (const bool solid:{false,true}) for (const float size:{1.0f,4.0f,8.0f,40.0f}) {
            CAPTURE(solid); CAPTURE(size);
            verify(r,mesh,{.solid=solid,.edges=solid,.pointSize=size},identity());
        }
        verify(r,mesh,{.pointSize=8,.opacity=.4f},identity());
        // Edge-only hidden-line depth is also an opaque occluder; X-ray lines are not.
        verify(r,mesh,{.solid=false,.pointSize=4},identity());
        verify(r,mesh,{.solid=false,.xray=true,.pointSize=4},identity());
    }
}
TEST_CASE("Overlay GPU compaction refreshes transforms, camera, visibility, range order and duplicate groups") {
    auto mesh=o::fixtureMesh(); addPoints(mesh,{{0,0,.3f},{.42f,0,.3f},{1.03f,0,.7f}});
    o::Renderer r; o::initialize(r,{129,97,4,true}); o::upload(r,mesh);
    verify(r,mesh,{.pointSize=4},identity());
    r.scene.groups[2].model[12]=.6f;
    verify(r,mesh,{.pointSize=8},identity());
    auto projection=identity(); projection[12]=-.7f; projection[0]=1.3f;
    verify(r,mesh,{.pointSize=4},projection);
    r.scene.groups[1].points=false;
    verify(r,mesh,{.pointSize=4},projection);
    std::reverse(r.scene.groups.begin(),r.scene.groups.end());
    verify(r,mesh,{.pointSize=4},projection);
    auto duplicate=r.scene.groups.front(); duplicate.model[12]=-.2f; duplicate.color={.8f,.2f,.1f,1};
    r.scene.groups.push_back(duplicate);
    o::Group empty; r.scene.groups.insert(r.scene.groups.begin()+1,empty);
    verify(r,mesh,{.pointSize=4},projection);
    for (auto& group:r.scene.groups) group.points=false;
    verify(r,mesh,{.pointSize=4},projection);
    for (auto& group:r.scene.groups) { group.points=true; group.model[14]=2; }
    verify(r,mesh,{.pointSize=4},projection);
    for (auto& group:r.scene.groups) group.model=identity();
    verify(r,mesh,{.pointSize=4},identity());
}
TEST_CASE("Overlay GPU ordered scan spans block boundaries and multiple prefix levels") {
    auto mesh=o::fixtureMesh();
    for (const uint32_t count:{65539u,257u,1027u}) {
        std::vector<std::array<float,3>> points; points.reserve(count);
        for (uint32_t i=0;i<count;++i) points.push_back({float(i%211)/70-1.5f,float((i/211)%157)/52-1.5f,i%5==0?.3f:.8f});
        addPoints(mesh,points);
    }
    o::Renderer r; o::initialize(r,{65,49,4,true}); o::upload(r,mesh);
    verify(r,mesh,{.pointSize=1},identity());
    REQUIRE(r.culling.levels.size()==2);
    CHECK(r.culling.blockCount>256);
    // Later markers of equal depth have distinct IDs; a reordered scatter changes these samples.
    verify(r,mesh,{.solid=false,.edges=false,.pointSize=4,.opacity=.4f},identity());
}
TEST_CASE("Overlay GPU culling tolerates no submitted markers and a one-pixel viewport") {
    auto mesh=o::fixtureMesh();
    o::Renderer r; o::initialize(r,{1,1,4,true}); o::upload(r,mesh);
    verify(r,mesh,{.pointSize=40},identity());
    r.scene.groups.clear();
    verify(r,mesh,{.pointSize=4},identity());
    o::Renderer empty; o::initialize(empty,{3,2,1,true});
    o::Capture result; (void)o::render(empty,{.culling=o::Culling::footprint},identity(),&result);
    CHECK(result.cullingCounts.empty()); CHECK(result.rgba.size()==24);
}
TEST_CASE("Overlay GPU compaction crosses the third scan level without CPU counts") {
    woby::Mesh mesh;
    addPoints(mesh,std::vector<std::array<float,3>>(65539,{0,0,.5f}));
    o::Renderer r; o::initialize(r,{3,2,1,true}); o::upload(r,mesh);
    const auto full=r.scene.groups.front(); auto single=full; single.range.pointIndexCount=1;
    r.scene.groups.assign(65280,single); r.scene.groups.push_back(full);
    // The last group's 257 blocks straddle global block 65536. Disabled blocks
    // exercise zero contributions as well as carry propagation through three levels.
    for (size_t i=0;i<65280;++i) r.scene.groups[i].points=i%3!=0;
    o::prepareGpuCulling(r,true); o::updateGpuCullInputs(r,identity());
    REQUIRE(r.culling.levels.size()==3); REQUIRE(r.culling.blockCount==65537);
    gpu::reset_command_pool(r.pool); r.arena->reset();
    auto* commands=gpu::begin_commands(r.pool);
    gpu::set_texture_descriptor_heap(commands,r.descriptors);
    o::submitGpuCulling(r,commands,{.culling=o::Culling::frustum},false);
    o::captureGpuCulling(r,commands,true);
    gpu::barrier(commands,gpu::Stage::transfer,gpu::Access::transfer_write,gpu::Stage::host,gpu::Access::host_read);
    gpu::end_commands(commands);
    const std::array list{commands}; gpu::submit(r.device,{.commands=list,.completion={r.timeline,++r.sequence}});
    gpu::wait_timeline({r.timeline,r.sequence});
    o::Capture result; result.cullingSelectionRequested=true; o::readGpuCulling(r,result);
    REQUIRE(result.cullingCounts.size()==65281);
    for (size_t i=0;i<65280;++i) REQUIRE(result.cullingCounts[i]==(i%3!=0?1u:0u));
    CHECK(result.cullingCounts.back()==65539);
    const size_t prefix=65280/3*2;
    REQUIRE(result.cullingSelection.size()==prefix+65539);
    CHECK(std::all_of(result.cullingSelection.begin(),result.cullingSelection.begin()+prefix,[](auto id){return id==0;}));
    for (size_t i=0;i<65539;++i) REQUIRE(result.cullingSelection[prefix+i]==i);
}
