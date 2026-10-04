#include "freeform_trim_fixture.h"
#include "freeform_trim.h"
#include "obj_freeform.h"
#include "obj_mesh.h"
#include "scene_pick.h"
#include "ui_operations.h"
#include "utf8_path.h"
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <map>
#include <numbers>

namespace {
struct TrimFixture {
    std::filesystem::path root;
    TrimFixture() {
        const auto name="woby_trim_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i=0;;++i) {
            root=std::filesystem::absolute(std::filesystem::temp_directory_path())/(name+"_"+std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~TrimFixture() { std::error_code e; std::filesystem::remove_all(root,e); }
    std::filesystem::path write(const std::string& contents) const {
        const auto file=root/woby::pathFromUtf8("切り抜き.obj"); std::ofstream(file)<<contents; return file;
    }
};
const std::string curve="vp .25 .25\nvp .75 .25\nvp .75 .75\nvp .25 .75\ncstype bezier\ndeg 1\n"
    "curv2 -4 -3 -2 -1 -4\nparm u 0 1 2 3 4\nend\n";
const std::string surface="v -1 -1 .4\nv 1 -1 .4\nv -1 1 .4\nv 1 1 .4\n"
    "cstype bezier\ndeg 1 1\nsurf 0 1 0 1 1 2 3 4\nparm u 0 1\nparm v 0 1\n";
woby::Mesh tessellate(woby::FreeformPatch p) {
    woby::Mesh mesh; woby::appendFreeformGeometry(mesh,{std::move(p)}); woby::finalizeMesh(mesh,false); return mesh;
}
double area(const woby::FreeformGrid& grid) {
    double total=0;
    for (size_t i=0;i<grid.triangles.size();i+=3) {
        const auto& a=grid.samples[grid.triangles[i]]; const auto& b=grid.samples[grid.triangles[i+1]]; const auto& c=grid.samples[grid.triangles[i+2]];
        const double cross=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        CHECK(cross>0); total+=cross/2;
    }
    return total;
}
}

TEST_CASE("OBJ trimming imports parameter curves and ordered regions without displaying control geometry") {
    const TrimFixture f;
    const auto file=f.write(curve+surface+"hole 4 0 -1\nend\n");
    const auto parsed=woby::readObjFreeform(file);
    REQUIRE(parsed.patches.size()==1); REQUIRE(parsed.patches[0].trimming);
    const auto& region=parsed.patches[0].trimming->regions[0];
    CHECK(region.outer.segments.empty()); REQUIRE(region.holes.size()==1);
    CHECK(region.holes[0].segments[0].interval==std::array<double,2>{4,0});
    const auto mesh=woby::loadObjMesh(file);
    CHECK(mesh.nodes.size()==1); CHECK(mesh.lineIndices.empty()); CHECK(mesh.pointIndices.empty());
    CHECK(area(mesh.freeform->grids[0])==doctest::Approx(.75));
}

TEST_CASE("Trimmed rational circular hole preserves area and conforming boundaries") {
    const auto mesh=tessellate(trimTestPatch(true));
    const auto& grid=mesh.freeform->grids[0];
    CHECK(area(grid)==doctest::Approx(1-std::numbers::pi/16).epsilon(3e-5));
    CHECK(mesh.vertices.size()==grid.samples.size());
    CHECK(mesh.indices.size()==grid.triangles.size());
    std::map<std::pair<uint32_t,uint32_t>,size_t> incidence;
    for (size_t i=0;i<grid.triangles.size();i+=3) for (size_t k=0;k<3;++k) {
        const auto a=grid.triangles[i+k],b=grid.triangles[i+(k+1)%3]; ++incidence[std::minmax(a,b)];
    }
    for (const auto& [edge,count]:incidence) {
        REQUIRE(count<=2);
        if (count!=1) { continue; }
        for (auto index : {edge.first,edge.second}) {
            const auto& p=grid.samples[index];
            CHECK((p[0]==0 || p[0]==1 || p[1]==0 || p[1]==1 || std::abs(std::hypot(p[0]-.5,p[1]-.5)-.25)<2e-5));
        }
    }
}

TEST_CASE("Trimming supports concavity multiple regions winding and narrow retained features") {
    auto p=trimTestPatch(); auto trim=std::make_shared<woby::FreeformTrimming>(); trim->sourceFile="regions.obj";
    trim->regions.push_back({trimPolygon({{0,0},{.4,0},{.4,.2},{.2,.2},{.2,.4},{0,.4}}),{}});
    trim->regions.push_back({trimPolygon({{.6,.6},{.6,.9},{.9,.9},{.9,.6}}),{trimPolygon({{.7,.7},{.8,.7},{.8,.8},{.7,.8}})}});
    trim->regions.push_back({trimPolygon({{0,.99},{1,.99},{1,.990001},{0,.990001}}),{}});
    p.trimming=trim;
    const auto mesh=tessellate(p);
    CHECK(area(mesh.freeform->grids[0])==doctest::Approx(.200001).epsilon(1e-8));
    const auto again=tessellate(p);
    CHECK(mesh.indices==again.indices); CHECK(mesh.precisePositions==again.precisePositions);
}

TEST_CASE("Trimming rejects invalid loops with source context") {
    auto p=trimTestPatch(); auto trim=std::make_shared<woby::FreeformTrimming>(*p.trimming); p.trimming=trim;
    auto& hole=trim->regions[0].holes[0];
    SUBCASE("open") { hole.segments[0].interval[1]=3; }
    SUBCASE("crossing") { hole=trimPolygon({{.2,.2},{.8,.8},{.8,.2},{.2,.8}}); }
    SUBCASE("outside") { hole=trimPolygon({{-.1,.2},{.8,.2},{.8,.8},{-.1,.8}}); }
    SUBCASE("touching") { hole=trimPolygon({{0,.2},{.8,.2},{.8,.8},{0,.8}}); }
    SUBCASE("overlapping holes") { trim->regions[0].holes.push_back(trimPolygon({{.5,.5},{.9,.5},{.9,.9},{.5,.9}})); }
    SUBCASE("nested holes") { trim->regions[0].holes.push_back(trimPolygon({{.4,.4},{.6,.4},{.6,.6},{.4,.6}})); }
    SUBCASE("overlapping regions") { trim->regions.push_back({trimPolygon({{.1,.1},{.2,.1},{.2,.2},{.1,.2}}),{}}); }
    SUBCASE("interval") { hole.segments[0].interval[1]=9; }
    CHECK_THROWS_WITH_AS((void)tessellate(p),doctest::Contains("test.obj (line 12)"),std::runtime_error);
}

TEST_CASE("OBJ trimming validates references ranges and continued composite loops") {
    const TrimFixture f;
    for (const std::string body : {"hole 0 4 0\n", "hole 0 4 2\n", "hole 0 5 1\n", "hole 0 0 1\n", "hole 0 4\n"}) {
        CAPTURE(body); CHECK_THROWS((void)woby::loadObjMesh(f.write(curve+surface+body+"end\n")));
    }
    const auto mesh=woby::loadObjMesh(f.write(curve+surface+"hole 0 2 1 \\\n 2 4 1\nend\n"));
    CHECK(area(mesh.freeform->grids[0])==doctest::Approx(.75));
}

TEST_CASE("Picking through a trimmed hole selects geometry behind it") {
    woby::UiState state;
    state.files.push_back(woby::createUiFileState("front.obj",tessellate(trimTestPatch()),0));
    auto back=trimTestPatch(); back.trimming.reset(); for (auto& p:back.controls) { p[2]=.8; }
    state.files.push_back(woby::createUiFileState("back.obj",tessellate(back),1));
    woby::appendDefaultSceneNodesForFiles(state,0); woby::assignSceneObjectIds(state);
    woby::ScenePickView view; view.width=view.height=100;
    for (size_t i=0;i<4;++i) { view.view[i*5]=1; view.projection[i*5]=1; }
    const auto parts=woby::scenePickParts(state);
    CHECK(woby::pickSceneObject(parts,view,{50,50})==state.files[1].groupSettings[0].objectId);
    CHECK(woby::pickSceneObject(parts,view,{10,50})==state.files[0].groupSettings[0].objectId);
}

TEST_CASE("Trimmed bounds use retained geometry and sampling remains cancellable") {
    auto p=trimTestPatch(); auto trim=std::make_shared<woby::FreeformTrimming>();
    trim->regions.push_back({trimPolygon({{.2,.2},{.4,.2},{.4,.4},{.2,.4}}),{}}); p.trimming=trim;
    const auto mesh=tessellate(p);
    CHECK(mesh.bounds.min[0]==doctest::Approx(-.6)); CHECK(mesh.bounds.max[0]==doctest::Approx(-.2));
    struct Canceled {};
    woby::Mesh canceled;
    CHECK_THROWS_AS(woby::appendFreeformGeometry(canceled,{p},[](const auto&) { throw Canceled{}; }),Canceled);
}

TEST_CASE("Trimming extracts nonclamped periodic B splines at active domain endpoints") {
    auto p=trimTestPatch(); auto trim=std::make_shared<woby::FreeformTrimming>(*p.trimming); p.trimming=trim;
    auto curve2=std::make_shared<woby::FreeformPatch>(); curve2->degreeU=2; curve2->countU=6;
    curve2->knotsU={0,1,2,3,4,5,6,7,8}; curve2->domainU={2,6};
    curve2->controls={{{.25,.25,0,1}},{{.75,.25,0,1}},{{.75,.75,0,1}},{{.25,.75,0,1}},{{.25,.25,0,1}},{{.75,.25,0,1}}};
    trim->regions[0].holes[0]={{{curve2,{2,6}}},12};
    CHECK(area(tessellate(p).freeform->grids[0])==doctest::Approx(19./24).epsilon(3e-5));
}

TEST_CASE("OBJ trimming preserves multiple outer region and hole ordering") {
    const TrimFixture f;
    const std::string definitions="vp 0 0\nvp .4 0\nvp .4 .4\nvp 0 .4\n"
        "vp .1 .1\nvp .3 .1\nvp .3 .3\nvp .1 .3\n"
        "vp .6 .6\nvp 1 .6\nvp 1 1\nvp .6 1\n"
        "cstype bezier\ndeg 1\ncurv2 1 2 3 4 1\nparm u 0 1 2 3 4\nend\n"
        "curv2 5 6 7 8 5\nparm u 0 1 2 3 4\nend\n"
        "curv2 9 10 11 12 9\nparm u 0 1 2 3 4\nend\n";
    const auto file=f.write(definitions+surface+"trim 0 4 1\nhole 0 4 2\ntrim 4 0 3\nend\n");
    const auto mesh=woby::loadObjMesh(file);
    REQUIRE(mesh.freeform->patches[0].trimming->regions.size()==2);
    CHECK(mesh.freeform->patches[0].trimming->regions[0].holes.size()==1);
    CHECK(mesh.freeform->patches[0].trimming->regions[1].holes.empty());
    CHECK(area(mesh.freeform->grids[0])==doctest::Approx(.28));
}

TEST_CASE("Trimming preserves small features adjacent to grid constraints") {
    auto p=trimTestPatch(); auto trim=std::make_shared<woby::FreeformTrimming>(); p.trimming=trim;
    trim->regions.push_back({trimPolygon({{.25,.2},{.25000000001,.2},{.25000000001,.8},{.25,.8}}),{}});
    const auto mesh=tessellate(p);
    CHECK(area(mesh.freeform->grids[0])==doctest::Approx(6e-12).epsilon(1e-6).scale(1e-12));
}

TEST_CASE("Trimming checks triangulator index capacity and cancels during preparation") {
    auto p=trimTestPatch();
    SUBCASE("32-bit triangulator capacity") {
        woby::FreeformGrid grid;
        for (size_t i=0;i<50000;++i) { grid.u.push_back(double(i)/49999); }
        grid.v=grid.u;
        CHECK_THROWS_WITH(woby::triangulateFreeformTrim(p,grid,{}),doctest::Contains("supported triangulator index range"));
        CHECK(grid.samples.empty());
    }
    SUBCASE("cancellation") {
        struct Canceled {};
        int visits=0; woby::Mesh mesh;
        CHECK_THROWS_AS(woby::appendFreeformGeometry(mesh,{p},[&](const auto&) { if (++visits==12) { throw Canceled{}; } }),Canceled);
        CHECK(visits==12); CHECK(mesh.vertices.empty());
    }
}

TEST_CASE("Trimming omits unused grid cells before checking triangulator capacity") {
    auto p=trimTestPatch();
    auto trim=std::make_shared<woby::FreeformTrimming>(); p.trimming=trim;
    trim->regions.push_back({trimPolygon({{.49999,.49999},{.50001,.49999},{.50001,.50001},{.49999,.50001}}),{}});
    woby::FreeformGrid grid;
    // The full Cartesian product exceeds CDT's capacity, but only a few cells
    // intersect the retained region. No multi-billion-vertex allocation is needed.
    for (size_t i=0;i<50000;++i) { grid.u.push_back(double(i)/49999); }
    grid.v=grid.u;
    woby::triangulateFreeformTrim(p,grid,{});
    CHECK(grid.u.size()==50000); CHECK(grid.v.size()==50000);
    REQUIRE(!grid.samples.empty()); CHECK(grid.samples.size()<20);
    CHECK(area(grid)==doctest::Approx(4e-10).epsilon(1e-6).scale(1e-10));
    for (const auto& uv:grid.samples) {
        CHECK(uv[0]>=.49999); CHECK(uv[0]<=.50001);
        CHECK(uv[1]>=.49999); CHECK(uv[1]<=.50001);
    }
}

TEST_CASE("Trim grid cropping preserves cells in nonunit domains and at grid boundaries") {
    auto p=trimTestPatch();
    p.domainU={10,20}; p.knotsU={10,10,20,20};
    p.domainV={-3,7}; p.knotsV={-3,-3,7,7};
    double u0=12,u1=16,v0=-1,v1=3;
    SUBCASE("aligned bounds") {}
    SUBCASE("bounds within cells") { u0=12.2; u1=15.8; v0=-.8; v1=2.8; }
    SUBCASE("domain edges") { u0=10; v1=7; }
    auto trim=std::make_shared<woby::FreeformTrimming>(); p.trimming=trim;
    trim->regions.push_back({trimPolygon({{u0,v0},{u1,v0},{u1,v1},{u0,v1}}),{}});
    woby::FreeformGrid grid;
    grid.u={10,11,12,13,14,15,16,17,18,19,20};
    grid.v={-3,-2,-1,0,1,2,3,4,5,6,7};
    woby::triangulateFreeformTrim(p,grid,{});
    CHECK(area(grid)==doctest::Approx((u1-u0)*(v1-v0)));
    // Every surviving grid crossing is retained, and no triangle crosses a
    // constrained grid line (including the surface's knot lines).
    for (double v:grid.v) for (double u:grid.u) {
        if (u<u0 || u>u1 || v<v0 || v>v1) { continue; }
        CHECK(std::any_of(grid.samples.begin(),grid.samples.end(),[&](const auto& uv) {
            return std::abs(uv[0]-u)<1e-12 && std::abs(uv[1]-v)<1e-12;
        }));
    }
    for (size_t i=0;i<grid.triangles.size();i+=3) {
        for (size_t k=0;k<2;++k) {
            double low=grid.samples[grid.triangles[i]][k],high=low;
            for (size_t j=1;j<3;++j) {
                const auto value=grid.samples[grid.triangles[i+j]][k];
                low=std::min(low,value); high=std::max(high,value);
            }
            CHECK(low>=(k==0 ? u0 : v0)-1e-12);
            CHECK(high<=(k==0 ? u1 : v1)+1e-12);
            for (double line:k==0 ? grid.u : grid.v) {
                CHECK_FALSE((low<line-1e-12 && high>line+1e-12));
            }
        }
    }
}
