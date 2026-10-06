#include "adaptive_points.h"
#include "scene_history.h"
#include "ui_operations.h"
#include "automation_registry.h"
#include <doctest/doctest.h>
#include <fstream>
#include <numeric>

namespace {
struct PointDirectory {
    std::filesystem::path path=std::filesystem::absolute(std::filesystem::temp_directory_path())/("woby-points-"+woby::automationRandomHex(8));
    PointDirectory() { std::filesystem::create_directory(path); }
    ~PointDirectory() { std::error_code error; std::filesystem::remove_all(path,error); }
};
woby::Mesh cloudMesh(uint32_t count) {
    woby::Mesh mesh; mesh.vertices.resize(count); mesh.pointIndices.resize(count);
    std::iota(mesh.pointIndices.begin(),mesh.pointIndices.end(),0u);
    for (uint32_t i=0;i<count;++i) mesh.vertices[i].position={float(i%257)*.007f-.9f,float((i/257)%257)*.007f-.9f,.2f+float(i%17)*.03f};
    mesh.nodes={{"points"}}; mesh.nodes[0].pointIndexCount=count; mesh.bounds=woby::calculateBounds(mesh.vertices);
    return mesh;
}
}
TEST_CASE("Adaptive point queue keeps vertices opaque independently of surface opacity") {
    woby::GpuMesh mesh;
    mesh.pointCloud=std::make_shared<const woby::points::Cloud>(woby::points::buildCloud(cloudMesh(16),8,4));
    woby::AdaptivePointRuntime runtime; runtime.enabled=true;
    woby::SceneDrawItem item; item.points=true;
    bx::mtxIdentity(item.model.data());
    for (const float alpha:{0.0f,.35f,.998f,.999f,1.0f}) {
        item.color={.4f,.5f,.6f,alpha}; runtime.draws.clear();
        REQUIRE(woby::queueAdaptivePoints(runtime,mesh,item,1));
        REQUIRE(runtime.draws.size()==1);
        const auto& color=runtime.draws[0].key.group.color;
        CHECK(color[0]==doctest::Approx(.6f)); CHECK(color[1]==doctest::Approx(.75f));
        CHECK(color[2]==doctest::Approx(.9f)); CHECK(color[3]==1);
    }
}
TEST_CASE("Point backend selection retains adaptive rendering without 64 bit atomics") {
    using namespace woby::graphics;
    constexpr auto portable=WOBY_GPU_CAPS_POINT_COMPUTE;
    constexpr auto native=portable|WOBY_GPU_CAPS_OPAQUE_POINTS;
    CHECK(selectPointBackend(0)==PointBackend::quads);
    CHECK(selectPointBackend(portable)==PointBackend::atomic32);
    CHECK(selectPointBackend(native)==PointBackend::atomic64);
    CHECK(selectPointBackend(native,PointBackend::atomic32)==PointBackend::atomic32);
    CHECK(selectPointBackend(native,PointBackend::quads)==PointBackend::quads);
    CHECK(selectPointBackend(portable,PointBackend::atomic64)==PointBackend::atomic32);
    CHECK(selectPointBackend(0,PointBackend::atomic32)==PointBackend::quads);
}
TEST_CASE("Adaptive point controls persist through views scene files and history") {
    PointDirectory directory;
    woby::UiState state; REQUIRE(state.adaptivePoints);
    const auto clean=woby::createSceneDocument(state);
    woby::SceneHistory history; woby::resetSceneHistory(history,state);
    woby::setAdaptivePoints(state,false); REQUIRE(state.isDirty);
    const auto revision=state.sceneEditRevision;
    woby::setAdaptivePoints(state,false); CHECK(state.sceneEditRevision==revision);
    REQUIRE(woby::recordSceneHistory(history,state));
    auto undo=woby::prepareSceneHistoryStep(history,state,clean,false);
    REQUIRE(undo); woby::commitSceneHistoryStep(history,state,std::move(*undo),false); CHECK(state.adaptivePoints);
    auto redo=woby::prepareSceneHistoryStep(history,state,clean,true);
    REQUIRE(redo); woby::commitSceneHistoryStep(history,state,std::move(*redo),true); CHECK_FALSE(state.adaptivePoints);
    const auto view=woby::createView(state);
    woby::setAdaptivePoints(state,true); woby::applyView(state,view); CHECK_FALSE(state.adaptivePoints);
    const auto path=directory.path/"scene.woby";
    woby::writeSceneDocument(path,woby::createSceneDocument(state));
    const auto saved=woby::readSceneDocument(path);
    REQUIRE(saved.views.size()==1); CHECK_FALSE(saved.adaptivePoints); CHECK_FALSE(saved.views[0].scene.adaptivePoints);
    const auto restored=woby::prepareSceneReplacement({}, {}, saved); CHECK_FALSE(restored.adaptivePoints);
    auto changed=saved; changed.adaptivePoints=true; CHECK_FALSE(woby::sceneContentEqual(changed,saved));
    { std::ofstream file(path); file << "version = 23\n[[views]]\nname = \"legacy\"\n"; }
    const auto legacy=woby::readSceneDocument(path); CHECK(legacy.adaptivePoints); CHECK(legacy.views[0].scene.adaptivePoints);
    { std::ofstream file(path); file << "version = 23\nadaptive_points = 42\n"; }
    CHECK_THROWS((void)woby::readSceneDocument(path));
}
TEST_CASE("Large point preparation shares original IDs and supports cancellation during hierarchy construction") {
    auto mesh=cloudMesh(65536);
    auto prepared=woby::prepareSceneMesh(mesh,woby::gpuMeshPoints);
    REQUIRE(prepared); REQUIRE(prepared->pointCloud); CHECK(prepared->pointVertexIndices.empty()); CHECK(prepared->compactOnly);
    const auto& cloud=*prepared->pointCloud; CHECK(cloud.points.size()==mesh.pointIndices.size());
    for (const auto& p:cloud.points) {
        REQUIRE(p.id>0); REQUIRE(p.id<=cloud.sourceVertices.size());
        CHECK(p.position==mesh.vertices[cloud.sourceVertices[p.id-1]].position);
    }
    CHECK(prepared->uploadBytes==(cloud.points.size()+cloud.proxies.size())*sizeof(woby::points::Point));
    uint32_t calls=0;
    const auto canceled=woby::points::buildCloud(mesh,cloud.sourceVertices,prepared->nodeRanges,[&] { return ++calls>8; });
    CHECK_FALSE(canceled); CHECK(calls==9);
}
TEST_CASE("Navigation cuts cover source regions and refinement visits all original points once") {
    const auto mesh=cloudMesh(20000); const auto cloud=woby::points::buildCloud(mesh,256,32);
    const auto navigation=woby::points::navigationDetail(cloud,4096);
    CHECK(navigation.points<=4096); REQUIRE_FALSE(navigation.ranges.empty());
    std::vector<bool> regions(cloud.points.size());
    for (const auto& range:navigation.ranges) {
        const auto n=std::find_if(cloud.nodes.begin(),cloud.nodes.end(),[&](const auto& node) {
            return range.proxy?node.proxyBegin==range.begin:node.begin==range.begin && node.count==range.count;
        });
        REQUIRE(n!=cloud.nodes.end());
        for (uint32_t i=n->begin;i<n->begin+n->count;++i) { CHECK_FALSE(regions[i]); regions[i]=true; }
    }
    CHECK(std::all_of(regions.begin(),regions.end(),[](bool v){return v;}));
    const auto full=woby::points::allPoints(cloud); woby::points::Refinement cursor;
    std::fill(regions.begin(),regions.end(),false);
    while (cursor.processed<full.points) {
        const auto batch=woby::points::nextRefinement(full,cursor,731); REQUIRE(batch.points<=731);
        for (const auto& r:batch.ranges) for (uint32_t i=r.begin;i<r.begin+r.count;++i) { CHECK_FALSE(regions[i]); regions[i]=true; }
    }
    CHECK(std::all_of(regions.begin(),regions.end(),[](bool v){return v;}));
}
TEST_CASE("Full-source point picking remains independent of navigation proxies") {
    auto mesh=cloudMesh(20000); auto cloud=woby::points::buildCloud(mesh,256,16);
    woby::ScenePickView view; view.width=803; view.height=607;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    bx::mtxIdentity(view.renderProjection.data()); view.renderProjection[10]=-1; view.renderProjection[14]=1;
    woby::ScenePickPart part; part.mesh=&mesh; part.objectId=7; part.sourceMesh=true; part.vertices=true;
    part.pointIndexCount=static_cast<uint32_t>(mesh.pointIndices.size()); part.pointSize=4;
    bx::mtxIdentity(part.model.data()); part.model[0]=.83f; part.model[13]=.07f;
    for (uint32_t i=0;i<32;++i) {
        const woby::PickPoint query{float((i*71+19)%803),float((i*43+29)%607)};
        part.pointCloud=nullptr; const auto reference=woby::pickSceneObject({&part,1},view,query);
        part.pointCloud=&cloud; CHECK(woby::pickSceneObject({&part,1},view,query)==reference);
    }
}
TEST_CASE("Navigation allocates each group's budget without charging for other groups") {
    auto mesh=cloudMesh(20000);
    mesh.nodes[0].pointIndexCount=10000;
    mesh.nodes.push_back({"second"}); mesh.nodes[1].pointIndexOffset=10000; mesh.nodes[1].pointIndexCount=10000;
    const auto cloud=woby::points::buildCloud(mesh,256,32);
    for (uint32_t group=0;group<2;++group) {
        const auto cut=woby::points::navigationDetail(cloud,1024,group);
        CHECK(cut.points>512); CHECK(cut.points<=1024);
        uint32_t sourceCount=0;
        for (const auto& range:cut.ranges) {
            CHECK(range.group==group);
            const auto node=std::find_if(cloud.nodes.begin(),cloud.nodes.end(),[&](const auto& n) {
                return range.proxy?n.proxyBegin==range.begin:n.begin==range.begin && n.count==range.count;
            });
            REQUIRE(node!=cloud.nodes.end()); sourceCount+=node->count;
        }
        CHECK(sourceCount==10000);
        const auto complete=woby::points::navigationDetail(cloud,10000,group);
        CHECK(complete.points==10000);
        CHECK(std::none_of(complete.ranges.begin(),complete.ranges.end(),[](const auto& r) { return r.proxy; }));
    }
    CHECK(woby::points::navigationDetail(cloud,1024,2).ranges.empty());
}
TEST_CASE("Prepared navigation cuts retain sparse geometry beside a dense cluster") {
    auto mesh=cloudMesh(20000);
    for (uint32_t i=0;i<19500;++i) mesh.vertices[i].position={0,0,.5f};
    for (uint32_t i=19500;i<20000;++i)
        mesh.vertices[i].position={float(i%25)*.075f-.91f,float((i-19500)/25)*.09f-.91f,.5f};
    const auto cloud=woby::points::buildCloud(mesh,128,8);
    const auto cut=woby::points::navigationDetail(cloud,4096);
    CHECK(cut.points<=4096);
    std::vector<bool> covered(500);
    for (const auto& range:cut.ranges) {
        const auto& points=range.proxy?cloud.proxies:cloud.points;
        for (uint32_t i=range.begin;i<range.begin+range.count;++i)
            if (points[i].id>19500) covered[points[i].id-19501]=true;
    }
    CHECK(std::all_of(covered.begin(),covered.end(),[](bool value) { return value; }));
}
