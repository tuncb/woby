#include "freeform.h"
#include "freeform_gpu.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include <doctest/doctest.h>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>

namespace {
struct Fixture {
    std::filesystem::path root;
    Fixture() {
        const auto name = "woby_freeform_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i=0;;++i) {
            root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / (name+"_"+std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root,ec); }
    std::filesystem::path write(const std::string& text) const {
        const auto file = root / woby::pathFromUtf8("曲面.obj");
        std::ofstream(file) << text;
        return file;
    }
};
woby::FreeformPatch arc() {
    woby::FreeformPatch p;
    p.degreeU=2; p.countU=3; p.domainU={0,1}; p.knotsU={0,0,0,1,1,1};
    p.controls={{{1,0,0,1}},{{1,1,0,std::sqrt(.5)}},{{0,1,0,1}}};
    return p;
}
const std::string plane = "v 0 0 0\nv 2 0 0\nv 0 3 0\nv 2 3 0\n"
    "cstype bezier\ndeg 1 1\nsurf 0 1 0 1 1 2 3 4\nparm u 0 1\nparm v 0 1\nend\n";
}

TEST_CASE("Freeform rational quadratic evaluates an exact circular arc and derivative") {
    const auto p=arc();
    woby::validateFreeformPatch(p);
    for (double u : {0.,.1,.5,.9,1.}) {
        const auto s=woby::evaluateFreeform(p,u);
        CHECK(s.position[0]*s.position[0]+s.position[1]*s.position[1] == doctest::Approx(1).epsilon(1e-12));
        CHECK(s.position[0]*s.du[0]+s.position[1]*s.du[1] == doctest::Approx(0).epsilon(1e-12));
    }
    CHECK(woby::evaluateFreeform(p,.5).position[0] == doctest::Approx(std::sqrt(.5)));
    CHECK_THROWS_AS((void)woby::evaluateFreeform(p,-.1),std::invalid_argument);
}

TEST_CASE("Freeform evaluation preserves small features at large coordinates") {
    auto p=arc();
    for (auto& c:p.controls) { c[0]+=1e12; c[1]+=1e12; }
    const auto s=woby::evaluateFreeform(p,.5);
    CHECK(s.position[0]-1e12 == doctest::Approx(std::sqrt(.5)).epsilon(.0001));
    CHECK(s.du[0] == doctest::Approx(-1.17157287525381));
}

TEST_CASE("OBJ freeform surfaces tessellate in u-major control order with analytic normals") {
    const Fixture f;
    const auto mesh=woby::loadObjMesh(f.write(plane));
    REQUIRE(mesh.freeform);
    REQUIRE(mesh.vertices.size()==33*33);
    CHECK(std::vector<uint32_t>(mesh.indices.begin(),mesh.indices.begin()+6) == std::vector<uint32_t>{0,1,33,1,34,33});
    CHECK(mesh.pointIndices.empty());
    CHECK(mesh.nodes.size()==1);
    for (const auto& v:mesh.vertices) { CHECK(v.normal == std::array<float,3>{0,0,1}); }
    const auto s=woby::evaluateFreeform(mesh.freeform->patches[0],.25,.5);
    CHECK(s.position == woby::Coordinate{.5,1.5,0});
    REQUIRE(mesh.sourceData);
    CHECK(mesh.sourceData->indices.size()==32*32*6);
}

TEST_CASE("OBJ freeform supports rational curves negative indexes and continuation") {
    const Fixture f;
    const auto mesh=woby::loadObjMesh(f.write("v 1 0 0 1\nv 1 1 0 .7071067811865476\nv 0 1 0 1\n"
        "g arc\ncstype rat bspline\ndeg 2\ncurv 0 1 -3 \\\n -2 -1\nparm u 0 0 0 1 1 1\nend\n"));
    REQUIRE(mesh.freeform);
    CHECK(mesh.vertices.size()==33);
    CHECK(mesh.lineIndices.size()==64);
    CHECK(mesh.indices.empty()); CHECK(mesh.pointIndices.empty());
    CHECK(mesh.nodes[0].name=="arc");
    const auto& middle=mesh.precisePositions[16];
    CHECK(middle[0]+mesh.origin[0] == doctest::Approx(std::sqrt(.5)));
}

TEST_CASE("large native OBJ falls back safely after asynchronous polygon parsing") {
    const Fixture fixture;
    // Exercise the two early parser rejections present in issue #91's models.
    std::string text = "vp 0 0\n" + plane;
    SUBCASE("parameter vertices") {}
    SUBCASE("weighted control points") {
        text = "v 0 0 0 1\nv 2 0 0 1\nv 0 3 0 1\nv 2 3 0 1\n"
            "cstype rat bezier\ndeg 1 1\nsurf 0 1 0 1 1 2 3 4\nparm u 0 1\nparm v 0 1\nend\n";
    }
    const auto comment = "#" + std::string(126, 'x') + "\n";
    while (text.size() < 2 * 1024 * 1024) { text += comment; }
    const auto path = fixture.write(text);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto mesh = woby::loadObjMesh(path);
        REQUIRE(mesh.freeform);
        CHECK(mesh.freeform->patches.size() == 1);
        CHECK(mesh.vertices.size() == 33 * 33);
        CHECK(mesh.indices.size() == 32 * 32 * 6);
    }
}

TEST_CASE("OBJ freeform mixes ordinary primitives without changing authored normals") {
    const Fixture f;
    const auto mesh=woby::loadObjMesh(f.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 -1\n"
        "g face\nf 1//1 2//1 3//1\ng line\nl 1 2\ng dot\np 3\ng curve\n"
        "cstype bezier\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 1\nend\n"));
    REQUIRE(mesh.freeform);
    CHECK(mesh.nodes.size()==4); CHECK(mesh.indices.size()==3);
    CHECK(mesh.lineIndices.size()==66); CHECK(mesh.pointIndices.size()==1);
    CHECK(mesh.vertices[0].normal[2]==-1);
}

TEST_CASE("OBJ freeform interpolates authored UVs and normals") {
    const Fixture f;
    const auto mesh=woby::loadObjMesh(f.write("v 0 0 0\nv 2 0 0\nv 0 3 0\nv 2 3 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nvt 1 1\nvn 0 0 -1\n"
        "cstype bezier\ndeg 1 1\nsurf 0 1 0 1 1/1/1 2/2/1 3/3/1 4/4/1\nparm u 0 1\nparm v 0 1\nend\n"));
    REQUIRE(mesh.freeform);
    CHECK(mesh.nodes[0].hasTexcoords);
    const auto& p=mesh.freeform->patches[0];
    CHECK(woby::evaluateFreeform(p,.25,.5).texcoord==std::array<double,2>{.25,.5});
    CHECK(woby::freeformNormal(p,.25,.5)==std::array<float,3>{0,0,-1});
    CHECK(mesh.vertices[0].texcoord==std::array<float,2>{0,1});
}

TEST_CASE("OBJ piecewise Bezier and partial spline domains include knot boundaries") {
    const Fixture f;
    const auto mesh=woby::loadObjMesh(f.write("v 0 0 0\nv 1 2 0\nv 2 0 0\nv 3 -2 0\nv 4 0 0\n"
        "cstype bezier\ndeg 2\ncurv .25 1.5 1 2 3 4 5\nparm u 0 1 2\nend\n"));
    REQUIRE(mesh.freeform);
    const auto& grid=mesh.freeform->grids[0];
    CHECK(grid.u.front()==.25); CHECK(grid.u.back()==1.5); CHECK(grid.u[32]==1);
    CHECK(woby::evaluateFreeform(mesh.freeform->patches[0],1).position==woby::Coordinate{2,0,0});
}

TEST_CASE("OBJ freeform rejects unsupported and malformed geometry with a file diagnostic") {
    const Fixture f;
    for (const std::string tail : {
            "cstype cardinal\n", "curv2 1 2\n", "trim 0 1 1\n", "hole 0 1 1\n",
            "cstype bezier\ndeg 9\n", "cstype bezier\ndeg 2\ncurv 0 1 1 2 3\n",
            "cstype bezier\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 1\ntrim 0 1 1\nend\n",
            "cstype bezier\ndeg 2\ncurv 0 1 0 2 3\nparm u 0 1\nend\n",
            "cstype bspline\ndeg 2\ncurv 0 1 1 2 3\nparm u 0 0 0 .5 1 1\nend\n"}) {
        CAPTURE(tail);
        const auto path=f.write("v 0 0 0\nv 1 1 0\nv 2 0 0\n"+tail);
        try { (void)woby::loadObjMesh(path); FAIL("Expected rejection"); }
        catch (const std::runtime_error& e) { CHECK(std::string(e.what()).find(woby::pathToUtf8(path))!=std::string::npos); }
    }
}

TEST_CASE("Freeform validation rejects invalid weights control counts and discontinuities") {
    auto p=arc();
    SUBCASE("weight") { p.controls[1][3]=0; }
    SUBCASE("controls") { p.controls.pop_back(); }
    SUBCASE("domain") { p.domainU[1]=2; }
    SUBCASE("knots") { p.knotsU[3]=-1; }
    SUBCASE("UVs") { p.texcoords.push_back({0,0}); }
    SUBCASE("UV overflow") { p.texcoords.resize(3,{1e100,0}); }
    SUBCASE("normals") { p.normals.push_back({0,0,1}); }
    SUBCASE("discontinuity") { p.countU=6; p.controls.resize(6,{0,0,0,1}); p.knotsU={0,0,0,.5,.5,.5,1,1,1}; }
    CHECK_THROWS(woby::validateFreeformPatch(p));
}

TEST_CASE("Freeform GPU packing uses rebased controls and separable partition of unity") {
    auto p=arc(); woby::Mesh m;
    m.origin={1,2,3};
    woby::appendFreeformGeometry(m,{p});
    const auto& grid=m.freeform->grids[0];
    const auto data=woby::packFreeformGpu(p,grid,m.origin);
    REQUIRE(!data.empty());
    CHECK(data[16]==0); CHECK(data[17]==-2); CHECK(data[18]==-3);
    const auto offset=std::bit_cast<uint32_t>(data[9]);
    for (size_t i=0;i<grid.u.size();++i) {
        double sum=0,derivative=0;
        for (size_t j=0;j<=p.degreeU;++j) { sum+=data[offset+i*19+1+j]; derivative+=data[offset+i*19+10+j]; }
        CHECK(sum==doctest::Approx(1)); CHECK(derivative==doctest::Approx(0));
    }
    auto huge=p;
    huge.controls[0][0]=1e30;
    CHECK(woby::packFreeformGpu(huge,grid,m.origin).empty());
    p.controls[1][3]=1e-100;
    CHECK(woby::packFreeformGpu(p,grid,m.origin).empty());
}

TEST_CASE("Freeform nonuniform cubic derivatives agree with finite differences") {
    woby::FreeformPatch p;
    p.degreeU=3; p.countU=5; p.knotsU={0,0,0,0,.2,1,1,1,1}; p.domainU={0,1};
    p.controls={{{0,0,0,1}},{{1,2,0,.5}},{{2,-1,1,2}},{{3,3,0,1}},{{4,0,0,1}}};
    woby::validateFreeformPatch(p);
    for (double u : {.05,.2,.6,.95}) {
        const auto s=woby::evaluateFreeform(p,u);
        const auto a=woby::evaluateFreeform(p,u-1e-6), b=woby::evaluateFreeform(p,u+1e-6);
        for (size_t k=0;k<3;++k) { CHECK(s.du[k]==doctest::Approx((b.position[k]-a.position[k])/2e-6).epsilon(1e-6)); }
    }
    CHECK(woby::evaluateFreeform(p,0).position==woby::Coordinate{0,0,0});
    CHECK(woby::evaluateFreeform(p,1).position==woby::Coordinate{4,0,0});
}

TEST_CASE("Freeform tessellation supports large grids and cancellation within index bounds") {
    woby::Mesh mesh;
    SUBCASE("grid beyond the former two million vertex limit") {
        woby::FreeformPatch p;
        p.surface=true; p.degreeU=1; p.degreeV=1; p.countU=100; p.countV=100;
        p.controls.resize(10000,{0,0,0,1}); p.domainU={0,99}; p.domainV={0,99};
        p.knotsU.push_back(0);
        for (int i=0;i<100;++i) { p.knotsU.push_back(i); }
        p.knotsU.push_back(99); p.knotsV=p.knotsU;
        struct Canceled {};
        size_t planned = 0;
        CHECK_THROWS_AS(woby::appendFreeformGeometry(mesh,{p},[&](const auto& update) {
            planned = update.total;
            throw Canceled{};
        }),Canceled);
        CHECK(planned == 3169u * 3169u);
        CHECK(mesh.vertices.empty());
    }
    SUBCASE("32-bit index format cannot address the generated triangles") {
        woby::FreeformPatch p;
        p.surface=true; p.degreeU=1; p.degreeV=1; p.countU=1000; p.countV=1000;
        p.controls.resize(1000000,{0,0,0,1}); p.domainU={0,999}; p.domainV={0,999};
        p.knotsU.push_back(0);
        for (int i=0;i<1000;++i) { p.knotsU.push_back(i); }
        p.knotsU.push_back(999); p.knotsV=p.knotsU;
        CHECK_THROWS_WITH(woby::appendFreeformGeometry(mesh,{p}),"Freeform geometry exceeds the supported index range.");
        CHECK(mesh.vertices.empty());
    }
    SUBCASE("cancellation") {
        struct Canceled {};
        CHECK_THROWS_AS(woby::appendFreeformGeometry(mesh,{arc()},[](const auto&) { throw Canceled{}; }),Canceled);
    }
}

TEST_CASE("Freeform collapsed surface boundary has a finite interior-limit normal") {
    woby::FreeformPatch p;
    p.surface=true; p.degreeU=1; p.degreeV=1; p.countU=2; p.countV=2;
    p.knotsU={0,0,1,1}; p.knotsV=p.knotsU; p.domainU={0,1}; p.domainV={0,1};
    p.controls={{{0,0,0,1}},{{0,0,0,1}},{{0,1,0,1}},{{1,1,0,1}}};
    woby::validateFreeformPatch(p);
    CHECK(woby::freeformNormal(p,.5,0)==std::array<float,3>{0,0,1});
}
