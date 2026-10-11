#if defined(WOBY_INTERSECTION_STANDALONE_TEST)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#endif
#include <doctest/doctest.h>
#include "mesh_intersections.h"
#include "mesh_intersection_predicates.h"
#include "mesh_intersection_reference.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

// Match the production arithmetic options, but keep test template symbols
// separate from both the production classifier and CDT's other consumers.
#define predicates woby_intersection_test_predicates
#include <predicates.h>
#undef predicates
#if defined(WOBY_EXPECT_INTERSECTION_FMA) && !defined(PREDICATES_FAST_FMA)
#error "The predicate test did not select the FMA implementation"
#endif

namespace {
using namespace woby;
using Position = std::array<double, 3>;
using Triangle = std::array<Position, 3>;
using Ids = std::array<size_t, 3>;
struct Pair { Triangle a, b; Ids aIds, bIds; };
using Json = nlohmann::json;
using Exact = boost::multiprecision::cpp_rational;
using Integer = boost::multiprecision::cpp_int;
using ExactPoint = std::array<Exact, 3>;
void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
const Triangle base = {{{0,0,0},{2,0,0},{0,2,0}}};
const Triangle crossing = {{{.5,.5,-1},{.5,.5,1},{1.5,.5,0}}};
ExactPoint sub(const ExactPoint& a,const ExactPoint& b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
ExactPoint cross(const ExactPoint& a,const ExactPoint& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
Exact dot(const ExactPoint& a,const ExactPoint& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
std::array<ExactPoint,3> rational(const Triangle& t) {
    std::array<ExactPoint,3> result;
    for(size_t i=0;i<3;++i) { for(size_t k=0;k<3;++k) { result[i][k]=t[i][k]; } }
    return result;
}
// This oracle decodes IEEE-754 bits directly. It does not use floating point
// arithmetic or Boost's floating-point-to-rational conversion.
std::array<int,2> bitSigns(const std::array<Position,4>& points) {
    struct Dyadic { Integer significand; int exponent=0; };
    std::array<std::array<Dyadic,3>,4> decoded;
    int minimum=1024;
    for(size_t i=0;i<4;++i) { for(size_t k=0;k<3;++k) {
        const uint64_t bits=std::bit_cast<uint64_t>(points[i][k]);
        const uint64_t fraction=bits&((uint64_t{1}<<52)-1);
        const int exponent=static_cast<int>((bits>>52)&2047);
        require(exponent!=2047,"Non-finite sign oracle input.");
        decoded[i][k].significand=exponent ? (uint64_t{1}<<52)|fraction : fraction;
        decoded[i][k].exponent=exponent ? exponent-1023-52 : -1074;
        if(bits>>63) { decoded[i][k].significand=-decoded[i][k].significand; }
        if(decoded[i][k].significand!=0) { minimum=std::min(minimum,decoded[i][k].exponent); }
    } }
    std::array<std::array<Integer,3>,4> integers{};
    for(size_t i=0;i<4;++i) { for(size_t k=0;k<3;++k) {
        if(decoded[i][k].significand!=0) {
            integers[i][k]=decoded[i][k].significand<<static_cast<unsigned>(decoded[i][k].exponent-minimum);
        }
    } }
    const Integer ax=integers[0][0]-integers[2][0],ay=integers[0][1]-integers[2][1];
    const Integer bx=integers[1][0]-integers[2][0],by=integers[1][1]-integers[2][1];
    const Integer determinant2=ax*by-ay*bx;
    std::array<std::array<Integer,3>,3> differences;
    for(size_t i=0;i<3;++i) { for(size_t k=0;k<3;++k) { differences[i][k]=integers[i][k]-integers[3][k]; } }
    const auto& a=differences[0]; const auto& b=differences[1]; const auto& c=differences[2];
    const Integer determinant3=a[0]*(b[1]*c[2]-b[2]*c[1])+a[1]*(b[2]*c[0]-b[0]*c[2])+a[2]*(b[0]*c[1]-b[1]*c[0]);
    return {(determinant2>0)-(determinant2<0),(determinant3>0)-(determinant3<0)};
}
// Independent closed-triangle oracle: exact separating axes. Shared features
// are checked against the unchanged rational clipping algorithm instead.
bool rationalSat(const Triangle& ap,const Triangle& bp) {
    const auto a=rational(ap),b=rational(bp);
    std::array<ExactPoint,3> ae,be;
    for(size_t i=0;i<3;++i) { ae[i]=sub(a[(i+1)%3],a[i]); be[i]=sub(b[(i+1)%3],b[i]); }
    const auto an=cross(ae[0],ae[1]),bn=cross(be[0],be[1]);
    if(an==ExactPoint{} || bn==ExactPoint{}) { return false; }
    std::array<ExactPoint,17> axes; axes[0]=an; axes[1]=bn; size_t next=2;
    for(size_t i=0;i<3;++i) {
        axes[next++]=cross(an,ae[i]); axes[next++]=cross(bn,be[i]);
        for(size_t j=0;j<3;++j) { axes[next++]=cross(ae[i],be[j]); }
    }
    for(const auto& axis:axes) {
        auto alo=dot(axis,a[0]),ahi=alo,blo=dot(axis,b[0]),bhi=blo;
        for(size_t i=1;i<3;++i) {
            const auto av=dot(axis,a[i]),bv=dot(axis,b[i]);
            alo=std::min(alo,av); ahi=std::max(ahi,av); blo=std::min(blo,bv); bhi=std::max(bhi,bv);
        }
        if(ahi<blo || bhi<alo) { return false; }
    }
    return true;
}
size_t checkedPairs = 0, checkedSat = 0, checkedSigns = 0;
Json unsupportedExamples = Json::array();
void check(const Pair& pair, bool useSat = false)
{
    const bool expected = intersection_reference::intersects(pair.a, pair.b, pair.aIds, pair.bIds);
    const bool actual = trianglesSelfIntersect(pair.a, pair.b, pair.aIds, pair.bIds);
    const auto adaptive = adaptiveTrianglesSelfIntersect(pair.a, pair.b, pair.aIds, pair.bIds);
    INFO("pair " << checkedPairs);
    REQUIRE(actual == expected);
    if (adaptive) { REQUIRE(*adaptive == expected); }
    if (useSat) { REQUIRE(actual == rationalSat(pair.a, pair.b)); ++checkedSat; }
    ++checkedPairs;
}
void selfTest() {
    checkedPairs = 0; checkedSat = 0; checkedSigns = 0; unsupportedExamples.clear();
    std::vector<Pair> fixtures;
    const auto add=[&](const Triangle& b,Ids ids=Ids{3,4,5}) { fixtures.push_back({base,b,{0,1,2},ids}); };
    add(crossing); add(base); add(base,{0,1,2});
    add({{{.25,.25,0},{.5,.25,0},{.25,.5,0}}});
    add({{{3,0,0},{4,0,0},{3,1,0}}});
    add({{{0,0,0},{2,0,0},{0,-2,0}}},{0,1,3});
    add({{{0,0,0},{2,0,0},{0,-2,0}}});
    add({{{0,0,0},{2,0,0},{0,0,2}}},{0,1,3});
    add({{{0,0,0},{2,0,0},{1,1,0}}},{0,1,3});
    add({{{0,0,0},{-1,0,1},{0,-1,1}}},{0,3,4});
    add({{{0,0,0},{1,1,-1},{1,1,1}}},{0,3,4});
    add({{{1,0,0},{1,-1,1},{1,-1,-1}}});
    add({{{0,0,0},{1,0,0},{2,0,0}}});
    add({{{0,0,0},{-1,0,0},{0,-1,0}}},{0,3,4});
    add({{{0,0,0},{1,0,0},{0,-1,0}}},{0,3,4}); // Collinear contact beyond shared vertex.
    add({{{0,0,0},{1,1,0},{-1,1,0}}},{0,3,4});
    add({{{0,0,0},{-1,1,0},{-1,0,0}}},{0,3,4});
    add({{{0,0,0},{-1,0,0},{-1,-1,0}}},{0,3,4});
    add({{{0,0,0},{2,0,0},{0,2,0}}},{7,1,9}); // Coordinate equality alone does not allow contact.
    auto lifted=base; for(auto& p:lifted) { p[2]=std::numeric_limits<double>::denorm_min(); } add(lifted);
    auto signedDuplicate=base;
    Triangle signedNeighbor={{{0,0,0},{2,0,0},{0,-2,0}}};
    for(auto* triangle:{&signedDuplicate,&signedNeighbor}) {
        for(auto& point:*triangle) { for(auto& coordinate:point) { if(coordinate==0) { coordinate=-0.0; } } }
    }
    add(signedDuplicate,{0,1,2}); // Signed zero still identifies duplicate faces.
    add(signedNeighbor,{0,1,3}); // Signed zero still allows a shared edge.
    std::array<size_t,3> ai={0,1,2},bi={0,1,2};
    for(const auto& fixture:fixtures) {
        for(const int exponent:{-1074,-530,-200,-100,0,100,200,510,1000}) {
            auto scaled=fixture;
            for(auto* t:{&scaled.a,&scaled.b}) { for(auto& p:*t) { for(auto& value:p) { value=std::scalbn(value,exponent); } } }
            ai={0,1,2};
            do { bi={0,1,2}; do {
                Pair p;
                for(size_t i=0;i<3;++i) { p.a[i]=scaled.a[ai[i]]; p.b[i]=scaled.b[bi[i]]; p.aIds[i]=scaled.aIds[ai[i]]; p.bIds[i]=scaled.bIds[bi[i]]; }
                check(p);
                check({p.b,p.a,p.bIds,p.aIds});
            } while(std::next_permutation(bi.begin(),bi.end())); } while(std::next_permutation(ai.begin(),ai.end()));
        }
    }
    std::mt19937_64 random(0x875aa003);
    for(size_t trial=0;trial<16000;++trial) {
        Pair p; p.aIds={0,1,2}; p.bIds={3,4,5};
        const int scale=trial%7==0 ? static_cast<int>(random()%361)-180 : 0;
        for(auto* triangle:{&p.a,&p.b}) { for(auto& point:*triangle) { for(size_t k=0;k<3;++k) {
            double value=static_cast<double>(static_cast<int>(random()%17)-8);
            if(trial%5==0) { value+=static_cast<double>(random()>>11)*0x1p-53; }
            if(trial%3==0 && k==2) { value=0; }
            point[k]=std::scalbn(value,scale);
        } } }
        const size_t shared=trial%4;
        for(size_t i=0;i<shared;++i) { p.b[i]=p.a[i]; p.bIds[i]=p.aIds[i]; }
        if(trial%11==0) { p.b[2][0]=std::nextafter(p.b[2][0],std::numeric_limits<double>::infinity()); }
        check(p,shared==0 && trial%16==0);
    }
    // Raw bit-pattern tests stay inside the guarded exponent range. This
    // includes very small determinant signs with unrelated coordinate scales.
    for(size_t trial=0;trial<4000;++trial) {
        std::array<Position,4> points{};
        for(auto& point:points) { for(auto& value:point) {
            const int exponent=static_cast<int>(random()%361)-180;
            value=std::scalbn((static_cast<double>(random()>>11)*0x1p-53+.5)*(random()%2?1:-1),exponent);
        } }
        const auto exactPoints=rational({points[0],points[1],points[2]});
        const Exact expected2=(exactPoints[0][0]-exactPoints[2][0])*(exactPoints[1][1]-exactPoints[2][1])
            -(exactPoints[0][1]-exactPoints[2][1])*(exactPoints[1][0]-exactPoints[2][0]);
        const auto exactD=rational({points[3],points[3],points[3]})[0];
        const Exact expected3=dot(sub(exactPoints[0],exactD),cross(sub(exactPoints[1],exactD),sub(exactPoints[2],exactD)));
        const double actual2=woby_intersection_test_predicates::adaptive::orient2d(points[0].data(),points[1].data(),points[2].data());
        const double actual3=woby_intersection_test_predicates::adaptive::orient3d(points[0].data(),points[1].data(),points[2].data(),points[3].data());
        const auto exactSigns=bitSigns(points);
        require(exactSigns[0]==((expected2>0)-(expected2<0)) && exactSigns[1]==((expected3>0)-(expected3<0)),"Bit/rational sign oracle disagreement.");
        require(((actual2>0)-(actual2<0))==exactSigns[0],"Adaptive orient2d sign mismatch.");
        require(((actual3>0)-(actual3<0))==exactSigns[1],"Adaptive orient3d sign mismatch.");
        checkedSigns+=2;
    }
    for(const int exponent:{-180,-100,0,100,180}) {
        for(size_t trial=0;trial<400;++trial) {
            const double one=std::scalbn(1.0,exponent);
            const double two=std::nextafter(one,std::numeric_limits<double>::infinity());
            const double three=std::nextafter(two,std::numeric_limits<double>::infinity());
            const Position zeroPoint={0,0,0};
            const Position a={one,one,one},b={one,two,three};
            Position c={one,three,std::nextafter(three,std::numeric_limits<double>::infinity())};
            c[trial%3]=std::nextafter(c[trial%3],trial%2?std::numeric_limits<double>::infinity():0.0);
            const auto exactPoints=rational({a,b,c});
            const Exact expected2=exactPoints[0][0]*exactPoints[1][1]-exactPoints[0][1]*exactPoints[1][0];
            const Exact expected3=dot(exactPoints[0],cross(exactPoints[1],exactPoints[2]));
            const double raw2=woby_intersection_test_predicates::adaptive::orient2d(a.data(),b.data(),zeroPoint.data());
            const double raw3=woby_intersection_test_predicates::adaptive::orient3d(a.data(),b.data(),c.data(),zeroPoint.data());
            const auto signs2=bitSigns({a,b,zeroPoint,zeroPoint}),signs3=bitSigns({a,b,c,zeroPoint});
            require(signs2[0]==((expected2>0)-(expected2<0)) && signs3[1]==((expected3>0)-(expected3<0)),"Bit/rational near-cancellation oracle disagreement.");
            require(((raw2>0)-(raw2<0))==signs2[0],"Near-cancellation orient2d mismatch.");
            require(((raw3>0)-(raw3<0))==signs3[1],"Near-cancellation orient3d mismatch.");
            checkedSigns+=2;
        }
    }
    for(const int exponent:{-1074,-530,510,1000}) {
        const double scale=std::scalbn(1.0,exponent);
        const Triangle triangle={{{0,0,0},{scale,0,0},{0,scale,0}}};
        const Position fourth={0,0,scale};
        const double raw2=woby_intersection_test_predicates::adaptive::orient2d(triangle[0].data(),triangle[1].data(),triangle[2].data());
        const double raw3=woby_intersection_test_predicates::adaptive::orient3d(triangle[0].data(),triangle[1].data(),triangle[2].data(),fourth.data());
        const auto exactSigns=bitSigns({triangle[0],triangle[1],triangle[2],fourth});
        unsupportedExamples.push_back({{"exponent",exponent},{"raw_orient2_sign",(raw2>0)-(raw2<0)},
            {"exact_orient2_sign",exactSigns[0]},{"raw_orient3_sign",(raw3>0)-(raw3<0)},{"exact_orient3_sign",exactSigns[1]},
            {"raw_orient2_finite",std::isfinite(raw2)},{"raw_orient3_finite",std::isfinite(raw3)}});
    }
    auto invalid=base; invalid[0][0]=std::numeric_limits<double>::infinity();
    bool rejected=false; try { (void)trianglesSelfIntersect(base,invalid,{0,1,2},{3,4,5}); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,"Non-finite coordinate was accepted.");
}

} // namespace

TEST_CASE("adaptive intersections agree with independent exact oracles")
{
    selfTest();
    CHECK(checkedPairs == 30256);
    CHECK(checkedSat == 1000);
    CHECK(checkedSigns == 12000);
}

TEST_CASE("adaptive intersection range guards include boundaries and reject adjacent values")
{
    constexpr double lower = 0x1p-200, upper = 0x1p200;
    for (const double value : {std::nextafter(lower, 0.0), lower, std::nextafter(lower, 1.0),
            std::nextafter(upper, 0.0), upper, std::nextafter(upper, std::numeric_limits<double>::infinity()),
            std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::min(),
            std::numeric_limits<double>::max()}) {
        for (const double direction : {-1.0, 1.0}) {
            const double scale = direction*value;
            const Triangle triangle = {{{0,0,0},{scale,0,0},{0,scale,0}}};
            const auto adaptive = adaptiveTrianglesSelfIntersect(triangle, triangle, {0,1,2}, {0,1,2});
            CHECK(adaptive.has_value() == (value >= lower && value <= upper));
            if (adaptive) { CHECK(*adaptive); }
            CHECK(trianglesSelfIntersect(triangle, triangle, {0,1,2}, {0,1,2}));
        }
    }
    CHECK(adaptiveTrianglesSelfIntersect(Triangle{}, Triangle{}, {0,1,2}, {3,4,5}) == false);
    auto signedBase = base;
    for (auto& point : signedBase) { for (auto& value : point) { if (value == 0) { value = -0.0; } } }
    CHECK(adaptiveTrianglesSelfIntersect(base, signedBase, {0,1,2}, {0,1,2}) == true);
    // Neighboring coordinates at either end of the guard produce very small
    // differences relative to their magnitudes. Exercise degree-three signs,
    // including the 2^-756 determinant lattice at the lower boundary.
    for (const auto values : {std::array{lower, std::nextafter(lower, upper)},
            std::array{upper, std::nextafter(upper, lower)}, std::array{lower, upper}}) {
        const double x = values[0], y = values[1];
        const Triangle a = {{{x,x,x},{y,x,x},{x,y,x}}};
        const Triangle b = {{{x,x,x},{y,x,x},{x,x,y}}};
        auto separated = a;
        for (auto& point : separated) { point[2] = y; }
        // A zero returned for the exact nonzero plane sign would turn this
        // disjoint pair into a false coplanar intersection.
        CHECK(adaptiveTrianglesSelfIntersect(a, separated, {0,1,2}, {3,4,5}) == false);
        check({a,separated,{0,1,2},{3,4,5}});
        for (const Ids ids : {Ids{3,4,5}, Ids{0,4,5}, Ids{0,1,5}}) {
            const auto adaptive = adaptiveTrianglesSelfIntersect(a, b, {0,1,2}, ids);
            REQUIRE(adaptive.has_value());
            check({a,b,{0,1,2},ids});
            check({b,a,ids,{0,1,2}});
        }
    }
    // Reused IDs at different coordinates are not shared geometric features.
    check({base, crossing, {0,1,2}, {0,1,2}});
    for (const double value : {std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        auto invalid = base; invalid[0][0] = value;
        CHECK_FALSE(adaptiveTrianglesSelfIntersect(base, invalid, {0,1,2}, {3,4,5}).has_value());
        CHECK_THROWS_AS((void)trianglesSelfIntersect(base, invalid, {0,1,2}, {3,4,5}), std::invalid_argument);
    }
}

TEST_CASE("intersection rounding guard uses rational fallback and restores adaptive work")
{
    struct RoundingScope {
        int saved = std::fegetround();
        ~RoundingScope() { (void)std::fesetround(saved); }
    } scope;
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    auto lifted = base;
    for (auto& point : lifted) { point[2] = std::numeric_limits<double>::denorm_min(); }
    const std::array<Pair, 6> pairs = {{
        {base, crossing, {0,1,2}, {3,4,5}},
        {base, base, {0,1,2}, {0,1,2}},
        {base, {{{0,0,0},{2,0,0},{0,-2,0}}}, {0,1,2}, {0,1,3}},
        {base, {{{0,0,0},{1,1,-1},{1,1,1}}}, {0,1,2}, {0,3,4}},
        {base, lifted, {0,1,2}, {3,4,5}},
        {base, {{{0,0,0},{1,0,0},{2,0,0}}}, {0,1,2}, {3,4,5}}
    }};
    std::array<bool, 6> expected{};
    for (size_t i = 0; i < pairs.size(); ++i) {
        const auto& p = pairs[i];
        expected[i] = intersection_reference::intersects(p.a, p.b, p.aIds, p.bIds);
    }
    auto source = std::make_shared<SourceAnalysisGeometry>();
    source->source = "rounding fixture";
    for (const auto& triangle : {base, crossing}) {
        for (const auto& point : triangle) { source->vertices.push_back({point}); }
    }
    source->faces.push_back({{1,2,0},{0,1,2}});
    source->faces.push_back({{1,2,1},{3,4,5}});
    AnalysisGeometry geometry; geometry.availableSources = 1; geometry.sources.push_back(source);
    for (const int mode : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO, FE_TONEAREST}) {
        REQUIRE(std::fesetround(mode) == 0);
        CHECK(adaptiveTrianglesSelfIntersect(base, crossing, {0,1,2}, {3,4,5}).has_value() == (mode == FE_TONEAREST));
        for (size_t i = 0; i < pairs.size(); ++i) {
            const auto& p = pairs[i];
            CHECK(trianglesSelfIntersect(p.a, p.b, p.aIds, p.bIds) == expected[i]);
        }
        CHECK(std::fegetround() == mode);
        const auto result = inspectIntersections(geometry, {true,true}, {}, {0,0});
        REQUIRE(result.findings.size() == 1);
        CHECK(result.findings[0].faces[0] == TopologyFaceReference{1,2,0});
        CHECK(result.findings[0].faces[1] == TopologyFaceReference{1,2,1});
        CHECK(std::fegetround() == mode);
    }
}
