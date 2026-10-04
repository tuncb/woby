#include "point_renderer.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace p=woby::points;
namespace o=woby::overlay;
namespace {
p::Matrix identity() { p::Matrix m; bx::mtxIdentity(m.data()); return m; }
woby::Mesh pointMesh(uint32_t count) {
    woby::Mesh mesh;
    for (uint32_t i=0;i<count;++i) {
        woby::Vertex vertex; vertex.position={float((i*37u)%101)/50-.98f,float((i*59u)%97)/48-.99f,float((i*11u)%83)/100+.1f};
        mesh.vertices.push_back(vertex); mesh.pointIndices.push_back(i);
    }
    mesh.nodes.push_back({.name="points",.pointIndexCount=count});
    if (count) mesh.bounds=woby::calculateBounds(mesh.vertices); return mesh;
}
p::Pick brute(const p::Cloud& cloud,const p::Matrix& projection,uint32_t width,uint32_t height,float size,float x,float y) {
    p::Pick result;
    for (const auto& group:cloud.groups) {
        if (!group.enabled) continue;
        p::Matrix m; bx::mtxMul(m.data(),group.model.data(),projection.data());
        for (const auto& point:cloud.points) {
            if (point.id<group.firstId || point.id>=group.endId) continue;
            std::array<float,4> clip{};
            for (size_t k=0;k<4;++k) clip[k]=m[k]*point.position[0]+m[k+4]*point.position[1]+m[k+8]*point.position[2]+m[k+12];
            if (clip[3]<=0 || clip[2]<0 || clip[2]>clip[3]) continue;
            const float dx=(clip[0]/clip[3]*.5f+.5f)*float(width)-x,dy=(.5f-clip[1]/clip[3]*.5f)*float(height)-y,z=clip[2]/clip[3];
            if (dx*dx+dy*dy<=size*size*.25f && (z>result.depth || (z==result.depth && point.id>result.id))) { result.id=point.id; result.depth=z; }
        }
    }
    return result;
}
}
TEST_CASE("Compact cloud preserves every original ID and position across spatial order and groups") {
    auto mesh=pointMesh(301); mesh.pointIndices.push_back(0); mesh.nodes[0].pointIndexCount=302;
    mesh.pointIndices.push_back(0); mesh.nodes.push_back({.name="instance",.pointIndexOffset=302,.pointIndexCount=1});
    const auto cloud=p::buildCloud(mesh,32,8);
    CHECK(cloud.points.size()==302); CHECK(cloud.roots.size()==2); CHECK(cloud.groups.size()==2);
    std::set<uint32_t> ids;
    for (const auto& point:cloud.points) {
        CHECK(ids.insert(point.id).second); REQUIRE(point.id>=1); REQUIRE(point.id<=cloud.sourceVertices.size());
        CHECK(point.position==mesh.vertices[cloud.sourceVertices[point.id-1]].position);
    }
    for (const auto& point:cloud.proxies) CHECK(point.position==mesh.vertices[cloud.sourceVertices[point.id-1]].position);
    for (const auto& node:cloud.nodes) if (!node.left) CHECK(node.count<=32);
    mesh.vertices[0].position[0]=std::numeric_limits<float>::quiet_NaN(); CHECK_THROWS(p::buildCloud(mesh));
    CHECK_THROWS(p::buildCloud(pointMesh(5),1)); CHECK_THROWS(p::buildCloud(pointMesh(5),4,8));
    CHECK(p::buildCloud({}).points.empty());
}
TEST_CASE("Point LOD is deterministic and bounded; progressive ranges visit every source exactly once") {
    const auto cloud=p::buildCloud(pointMesh(4097),64,16);
    for (const uint32_t budget:{1u,32u,137u,500u,10000u}) {
        const auto a=p::selectDetail(cloud,identity(),127,83,4,budget);
        const auto b=p::selectDetail(cloud,identity(),127,83,4,budget);
        CHECK(a.points<=budget); CHECK(a.points>0); CHECK(a.points==b.points); CHECK(a.ranges.size()==b.ranges.size());
        uint64_t count=0; for (const auto r:a.ranges) count+=r.count; CHECK(count==a.points);
    }
    auto outside=identity(); outside[12]=5; CHECK(p::selectDetail(cloud,outside,64,64,4,500).points==0);
    const auto full=p::allPoints(cloud); p::Refinement cursor; std::set<uint32_t> seen;
    while (cursor.processed<full.points) {
        const auto batch=p::nextRefinement(full,cursor,137); CHECK(batch.points<=137); CHECK(batch.points>0);
        for (const auto r:batch.ranges) for (uint32_t i=r.begin;i<r.begin+r.count;++i) CHECK(seen.insert(cloud.points[i].id).second);
    }
    CHECK(seen.size()==cloud.points.size()); CHECK(p::nextRefinement(full,cursor,137).points==0);
    CHECK(p::adjustedBudget(1000000,16,8,2000000)==500000);
    CHECK(p::adjustedBudget(1000000,1,8,2000000)==1150000);
    CHECK(p::adjustedBudget(4096,100,8,2000000)==4096);
    CHECK_THROWS(p::adjustedBudget(0,1,8,2000000));
}
TEST_CASE("Hierarchical picking uses complete original footprints including fringe and stable equal-depth ties") {
    auto mesh=pointMesh(1400);
    mesh.vertices[0].position={1.01f,0,.8f}; mesh.vertices[1].position=mesh.vertices[0].position;
    auto cloud=p::buildCloud(mesh,32,8);
    for (const float size:{1.0f,4.0f,8.0f,40.0f}) for (uint32_t i=0;i<100;++i) {
        const float x=float((i*43)%97)+.375f,y=float((i*19)%61)+.125f;
        const auto a=p::pick(cloud,identity(),97,61,size,x,y),b=brute(cloud,identity(),97,61,size,x,y);
        CHECK(a.id==b.id); CHECK(a.depth==b.depth);
    }
    cloud.groups[0].model[12]=.1f;
    CHECK(p::pick(cloud,identity(),97,61,8,90.5f,30.5f).id==brute(cloud,identity(),97,61,8,90.5f,30.5f).id);
    cloud.groups[0].enabled=false; CHECK(p::pick(cloud,identity(),97,61,40,50,30).id==0);
    CHECK_THROWS(p::selectDetail(cloud,identity(),0,61,4,500));
}
TEST_CASE("Point GPU full circle visibility matches independent source queries for every sample") {
    auto mesh=pointMesh(257);
    mesh.vertices[0].position={1.015f,0,.91f}; mesh.vertices[1].position=mesh.vertices[0].position;
    mesh.vertices[2].position={0,0,-.1f}; mesh.vertices[3].position={0,0,1.1f};
    const auto cloud=p::buildCloud(mesh,16,4); const auto full=p::allPoints(cloud);
    const std::array<std::array<float,2>,4> offsets{{{.375f,.125f},{.875f,.375f},{.125f,.625f},{.625f,.875f}}};
    for (const uint32_t samples:{1u,4u}) {
        p::PointRenderer renderer; p::initialize(renderer,cloud,mesh,{37,29,samples,true},31);
        for (const float size:{1.0f,4.0f,8.0f,40.0f}) {
            CAPTURE(samples); CAPTURE(size); o::Capture capture; (void)p::render(renderer,cloud,full,identity(),size,false,true,&capture);
            for (uint32_t y=0;y<29;++y) for (uint32_t x=0;x<37;++x) for (uint32_t sample=0;sample<samples;++sample) {
                const auto offset=samples==1?std::array{.5f,.5f}:offsets[sample];
                const auto expected=brute(cloud,identity(),37,29,size,float(x)+offset[0],float(y)+offset[1]);
                REQUIRE(capture.sampleIds[(size_t(y)*37+x)*samples+sample]==expected.id);
            }
        }
        auto shifted=cloud; shifted.groups[0].model[12]=.125f; shifted.groups[0].model[5]=.5f;
        o::Capture transformed; CHECK(p::render(renderer,shifted,full,identity(),4,false,false,&transformed).reset);
        for (uint32_t y=0;y<29;++y) for (uint32_t x=0;x<37;++x) for (uint32_t sample=0;sample<samples;++sample) {
            const auto offset=samples==1?std::array{.5f,.5f}:offsets[sample];
            const auto expected=brute(shifted,identity(),37,29,4,float(x)+offset[0],float(y)+offset[1]);
            REQUIRE(transformed.sampleIds[(size_t(y)*37+x)*samples+sample]==expected.id);
        }
    }
}
TEST_CASE("Point GPU refinement converges exactly and camera style and group changes invalidate accumulation") {
    auto mesh=o::fixtureMesh();
    woby::Vertex rear; rear.position={.415f,0,.3f}; rear.normal={0,0,1}; mesh.vertices.push_back(rear); mesh.pointIndices={7};
    mesh.nodes.push_back({.name="rear fringe",.pointIndexCount=1});
    for (const uint32_t samples:{1u,4u}) {
        auto cloud=p::buildCloud(mesh,4,2); const auto full=p::allPoints(cloud);
        p::PointRenderer renderer; p::initialize(renderer,cloud,mesh,{97,61,samples,true},3);
        for (const bool solid:{false,true}) {
            o::Capture reference,refined; (void)p::render(renderer,cloud,full,identity(),8,solid,true,&reference);
            (void)p::render(renderer,cloud,{},identity(),8,solid,true);
            p::Refinement cursor;
            while (cursor.processed<full.points) (void)p::render(renderer,cloud,p::nextRefinement(full,cursor,2),identity(),8,solid,false);
            (void)p::render(renderer,cloud,{},identity(),8,solid,false,&refined);
            CHECK(refined.sampleIds==reference.sampleIds); CHECK(refined.rgba==reference.rgba);
            // Center behind the rectangle is hidden; the circle's exterior fringe survives.
            if (solid) { CHECK(reference.ids[30*97+67]==0); CHECK(std::count(reference.sampleIds.begin(),reference.sampleIds.end(),8u)>0); }
            auto reversed=full; std::reverse(reversed.ranges.begin(),reversed.ranges.end());
            (void)p::render(renderer,cloud,reversed,identity(),8,solid,true,&refined); CHECK(refined.sampleIds==reference.sampleIds);
        }
        auto moved=identity(); moved[12]=.4f;
        CHECK(p::render(renderer,cloud,{},moved,8,false,false).reset);
        o::Capture empty; CHECK_FALSE(p::render(renderer,cloud,{},moved,8,false,false,&empty).reset);
        CHECK(std::all_of(empty.ids.begin(),empty.ids.end(),[](auto id){return id==0;}));
        CHECK(p::render(renderer,cloud,{},moved,4,false,false).reset);
        cloud.groups[0].enabled=false; CHECK(p::render(renderer,cloud,{},moved,4,false,false).reset);
        cloud.groups[1].model[12]=.3f; CHECK(p::render(renderer,cloud,{},moved,4,false,false).reset);
        cloud.groups[1].color[0]=.9f; CHECK(p::render(renderer,cloud,{},moved,4,false,false).reset);
    }
}
