#include "point_index.h"
#include <doctest/doctest.h>
#include <random>

namespace {
using namespace hover_experiment;
Matrix identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
struct Fixture { std::vector<woby::Vertex> points; std::vector<uint32_t> ids; std::vector<Range> ranges; std::vector<Group> groups; };
Fixture randomFixture() {
    Fixture f; std::mt19937 rng(1742); std::uniform_real_distribution<float> d(-2,2);
    for (uint32_t i=0;i<8192;++i) { woby::Vertex v; v.position={d(rng),d(rng),d(rng)}; f.points.push_back(v); f.ids.push_back(i); }
    f.ranges={{0,4096},{4096,4096}}; f.groups={{identity(),identity(),3,true},{identity(),identity(),8,true}};
    f.groups[1].model[0]=.7f; f.groups[1].model[5]=1.4f; f.groups[1].model[12]=.31f;
    return f;
}
}
TEST_CASE("experimental point hierarchy agrees with linear picking during zoom and movement") {
    auto f=randomFixture(); auto index=buildIndex(f.points,f.ids,f.ranges);
    Query q{identity(),identity(),0,0,800,600,true};
    std::mt19937 rng(907); std::uniform_real_distribution<float> xy(0,1);
    for (int i=0;i<500;++i) {
        q.x=xy(rng)*800; q.y=xy(rng)*600;
        q.view[12]=xy(rng)-.5f; q.projection[0]=.1f+xy(rng)*3; q.projection[5]=q.projection[0];
        if (i%2) { q.projection[11]=.15f; } else { q.projection[11]=0; }
        f.groups[1].active=i%3!=0;
        for (auto& g:f.groups) { g.mvp=multiply(q.projection,multiply(q.view,g.model)); }
        Counters counts; const auto actual=queryIndex(index,f.points,f.ids,f.groups,q,counts);
        const auto expected=queryLinear(f.points,f.ids,f.ranges,f.groups,q);
        CHECK(actual.rank==expected.rank); CHECK(actual.depth==expected.depth); CHECK(actual.distance==expected.distance);
    }
}
TEST_CASE("experimental point hierarchy preserves ties frustum boundaries and pixel radius") {
    Fixture f; f.points.resize(1024); f.ids.resize(1024); std::iota(f.ids.begin(),f.ids.end(),0u);
    for (auto& p:f.points) { p.position={0,0,.5f}; }
    f.ranges={{0,1024}}; f.groups={{identity(),identity(),3,true}};
    auto index=buildIndex(f.points,f.ids,f.ranges);
    Query q{identity(),identity(),53,50,100,100,false}; Counters count;
    CHECK(queryIndex(index,f.points,f.ids,f.groups,q,count).rank==0);
    q.x=53.01f; CHECK(queryIndex(index,f.points,f.ids,f.groups,q,count).rank==invalid);
    q.x=50; q.view[14]=-2; f.groups[0].mvp=q.view;
    CHECK(queryIndex(index,f.points,f.ids,f.groups,q,count).rank==invalid);
    q.view=identity(); q.view[12]=1; f.groups[0].mvp=q.view; q.x=100;
    CHECK(queryIndex(index,f.points,f.ids,f.groups,q,count).rank==0);
}
TEST_CASE("experimental point hierarchy validates input and handles empty ranges") {
    auto f=randomFixture();
    f.ids[4]=UINT32_MAX; CHECK_THROWS(buildIndex(f.points,f.ids,f.ranges));
    f.ids[4]=4; f.points[4].position[0]=std::numeric_limits<float>::infinity(); CHECK_THROWS(buildIndex(f.points,f.ids,f.ranges));
    f.points[4].position[0]=0; f.ranges[0].count=UINT32_MAX; CHECK_THROWS(buildIndex(f.points,f.ids,f.ranges));
    const std::vector<Range> emptyRanges={{0,0}}; const auto empty=buildIndex({}, {}, emptyRanges);
    const std::vector<Group> groups={{identity(),identity(),3,true}};
    Query q{identity(),identity(),0,0,100,100,false}; Counters count;
    CHECK(queryIndex(empty,{}, {},groups,q,count).rank==invalid);
}
TEST_CASE("experimental point hierarchy remains conservative at large coordinate magnitudes") {
    auto f=randomFixture();
    for (auto& p:f.points) { p.position[0]=p.position[0]*10000+1000000; p.position[1]*=10000; p.position[2]*=10000; }
    const auto index=buildIndex(f.points,f.ids,f.ranges);
    Query q{identity(),identity(),400,300,800,600,true}; q.view[12]=-1000000;
    q.projection[0]=.00005f; q.projection[5]=.00005f; q.projection[10]=.00005f;
    for (auto& g:f.groups) { g.model=identity(); g.mvp=multiply(q.projection,q.view); }
    for (int i=0;i<50;++i) {
        q.x=static_cast<float>(i)*16; Counters count;
        CHECK(queryIndex(index,f.points,f.ids,f.groups,q,count).rank==queryLinear(f.points,f.ids,f.ranges,f.groups,q).rank);
    }
}
