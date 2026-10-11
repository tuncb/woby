#include "probe.h"
#include "obj_mesh.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <nlohmann/json.hpp>
#include "adaptive_predicates.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <string_view>

#if defined(WOBY_EXPECT_FMA_PATH) && !defined(PREDICATES_FAST_FMA)
#error "The FMA contract target did not select the predicate FMA path"
#endif

namespace allocation_probe {
struct alignas(std::max_align_t) Header { size_t size=0; bool tracked=false; };
struct Counts { size_t calls=0,requested=0,live=0,peak=0; };
inline Counts counts;
inline bool enabled=false;
void* allocate(size_t size) {
    if(size>std::numeric_limits<size_t>::max()-sizeof(Header)) { throw std::bad_alloc{}; }
    auto* allocation=static_cast<Header*>(std::malloc(sizeof(Header)+std::max(size,size_t{1})));
    if(!allocation) { throw std::bad_alloc{}; }
    allocation->size=size; allocation->tracked=enabled;
    if(enabled) { ++counts.calls; counts.requested+=size; counts.live+=size; counts.peak=std::max(counts.peak,counts.live); }
    return allocation+1;
}
void release(void* pointer) noexcept {
    if(!pointer) { return; }
    auto* allocation=static_cast<Header*>(pointer)-1;
    if(allocation->tracked) { counts.live-=allocation->size; }
    std::free(allocation);
}
}
void* operator new(size_t size) { return allocation_probe::allocate(size); }
void* operator new[](size_t size) { return allocation_probe::allocate(size); }
void operator delete(void* pointer) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer) noexcept { allocation_probe::release(pointer); }
void operator delete(void* pointer,size_t) noexcept { allocation_probe::release(pointer); }
void operator delete[](void* pointer,size_t) noexcept { allocation_probe::release(pointer); }

namespace {
using namespace woby;
using namespace intersection_probe;
using Json=nlohmann::json;
using Exact=boost::multiprecision::cpp_rational;
using Integer=boost::multiprecision::cpp_int;
using ExactPoint=std::array<Exact,3>;
using Clock=std::chrono::steady_clock;
void require(bool condition,const char* message) { if(!condition) { throw std::runtime_error(message); } }
const Triangle base={{{0,0,0},{2,0,0},{0,2,0}}};
const Triangle crossing={{{.5,.5,-1},{.5,.5,1},{1.5,.5,0}}};
Json counterJson() {
    return Json{{"calls",counters.calls},{"filtered",counters.filtered},{"rational",counters.rational},
        {"guarded",counters.guarded},{"signs_2d",counters.signs2d},{"signs_3d",counters.signs3d},
        {"rational_shared",counters.rationalShared}};
}
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
size_t checkedPairs=0,checkedSat=0,checkedSigns=0;
Json unsupportedExamples=Json::array();
void check(const Pair& pair,bool useSat=false) {
    useCandidate=false; forceRational=true;
    const bool oracle=probeBaseline(pair.a,pair.b,pair.aIds,pair.bIds);
    forceRational=false;
    const bool actual=predicate(pair.a,pair.b,pair.aIds,pair.bIds);
    const bool production=trianglesSelfIntersect(pair.a,pair.b,pair.aIds,pair.bIds);
    if(actual!=oracle || production!=oracle) {
        std::cerr<<Json{{"a",pair.a},{"b",pair.b},{"a_ids",pair.aIds},{"b_ids",pair.bIds},
            {"candidate",actual},{"oracle",oracle},{"production",production}}.dump()<<'\n';
        throw std::runtime_error("Predicate/oracle mismatch.");
    }
    if(useSat) { require(actual==rationalSat(pair.a,pair.b),"Independent rational SAT mismatch."); ++checkedSat; }
    ++checkedPairs;
}
void selfTest() {
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
        const double actual2=intersection_probe_predicates::adaptive::orient2d(points[0].data(),points[1].data(),points[2].data());
        const double actual3=intersection_probe_predicates::adaptive::orient3d(points[0].data(),points[1].data(),points[2].data(),points[3].data());
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
            const double raw2=intersection_probe_predicates::adaptive::orient2d(a.data(),b.data(),zeroPoint.data());
            const double raw3=intersection_probe_predicates::adaptive::orient3d(a.data(),b.data(),c.data(),zeroPoint.data());
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
        const double raw2=intersection_probe_predicates::adaptive::orient2d(triangle[0].data(),triangle[1].data(),triangle[2].data());
        const double raw3=intersection_probe_predicates::adaptive::orient3d(triangle[0].data(),triangle[1].data(),triangle[2].data(),fourth.data());
        const auto exactSigns=bitSigns({triangle[0],triangle[1],triangle[2],fourth});
        unsupportedExamples.push_back({{"exponent",exponent},{"raw_orient2_sign",(raw2>0)-(raw2<0)},
            {"exact_orient2_sign",exactSigns[0]},{"raw_orient3_sign",(raw3>0)-(raw3<0)},{"exact_orient3_sign",exactSigns[1]},
            {"raw_orient2_finite",std::isfinite(raw2)},{"raw_orient3_finite",std::isfinite(raw3)}});
    }
    auto invalid=base; invalid[0][0]=std::numeric_limits<double>::infinity();
    bool rejected=false; try { (void)predicate(base,invalid,{0,1,2},{3,4,5}); } catch(const std::invalid_argument&) { rejected=true; }
    require(rejected,"Non-finite coordinate was accepted.");
}
std::shared_ptr<const AnalysisGeometry> grid(size_t n,bool curved) {
    auto source=std::make_shared<SourceAnalysisGeometry>(); source->source=curved?"curved_grid":"flat_grid";
    for(size_t y=0;y<=n;++y) { for(size_t x=0;x<=n;++x) {
        const double z=curved?.03125*std::sin(static_cast<double>(x)*.4)*std::cos(static_cast<double>(y)*.4):0;
        source->vertices.push_back({{static_cast<double>(x),static_cast<double>(y),z}});
    } }
    for(size_t y=0;y<n;++y) { for(size_t x=0;x<n;++x) {
        const size_t a=y*(n+1)+x,b=a+1,c=a+n+1,d=c+1;
        source->faces.push_back({{1,1,source->faces.size()},{a,b,d}});
        source->faces.push_back({{1,1,source->faces.size()},{a,d,c}});
    } }
    auto geometry=std::make_shared<AnalysisGeometry>(); geometry->availableSources=1; geometry->sources.push_back(source); return geometry;
}
std::shared_ptr<const AnalysisGeometry> duplicates(size_t n) {
    auto source=std::make_shared<SourceAnalysisGeometry>(); source->source="duplicate_faces";
    for(size_t i=0;i<n;++i) {
        const size_t first=source->vertices.size(); for(const auto& point:base) { source->vertices.push_back({point}); }
        source->faces.push_back({{1,1,i},{first,first+1,first+2}});
    }
    auto geometry=std::make_shared<AnalysisGeometry>(); geometry->availableSources=1; geometry->sources.push_back(source); return geometry;
}
Json signature(const MeshIntersections& result) {
    Json findings=Json::array();
    for(const auto& f:result.findings) { findings.push_back({{"faces",{{f.faces[0].fileId,f.faces[0].partId,f.faces[0].triangleId},
        {f.faces[1].fileId,f.faces[1].partId,f.faces[1].triangleId}}},{"geometry",f.geometry},{"batch",f.batchId}}); }
    return Json{{"candidates",result.candidateTests},{"affected",result.affectedFaces},{"truncated",result.truncated},
        {"reason",result.truncationReason},{"findings",findings}};
}
std::string signatureHash(const Json& value) {
    uint64_t hash=14695981039346656037ull;
    for(const char byte:value.dump()) { hash^=static_cast<unsigned char>(byte); hash*=1099511628211ull; }
    std::array<char,17> output{};
    constexpr std::string_view digits="0123456789abcdef";
    for(size_t i=0;i<16;++i) { output[15-i]=digits[(hash>>(4*i))&15]; }
    return output.data();
}
Json benchmark(const AnalysisGeometry& geometry,size_t runs,IntersectionLimits limits={0,0}) {
    std::vector<Pair> captured; captured.reserve(captureLimit); capturedPairs=&captured;
    counters={}; useCandidate=false; forceRational=false;
    const auto original=probeInspect(geometry,{}, {},limits);
    const auto expected=signature(original); const Json frequency=counterJson(); capturedPairs=nullptr;
    require(signature(inspectIntersections(geometry,{}, {},limits))==expected,"Instrumented baseline changed the production result.");
    for(const auto& pair:captured) { check(pair); }
    Json result={{"frequency",frequency},{"oracle_pairs",captured.size()},{"findings",original.findings.size()},
        {"candidate_tests",original.candidateTests},{"truncated",original.truncated},{"reason",original.truncationReason},
        {"finding_signature_fnv1a64",signatureHash(expected)},
        {"runs",Json::array()}};
    for(size_t run=0;run<runs;++run) {
        for(size_t side=0;side<2;++side) {
            useCandidate=((run+side)%2)!=0; forceRational=false; counters={};
            const bool candidate=useCandidate;
            const auto start=Clock::now(); const auto actual=probeInspect(geometry,{}, {},limits);
            const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
            require(signature(actual)==expected,"Full intersection result/order changed.");
            result["runs"].push_back({{"candidate",candidate},{"variant",candidate?"candidate":"instrumented_baseline"},{"ms",elapsed},{"counters",counterJson()}});
        }
        const auto start=Clock::now(); const auto production=inspectIntersections(geometry,{}, {},limits);
        const double elapsed=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
        require(signature(production)==expected,"Production result changed during measurement.");
        result["runs"].push_back({{"variant","production"},{"ms",elapsed}});
    }
    // Count allocations in separate passes; tracking is off during timings.
    result["allocations"]=Json::array();
    for(bool candidate:{false,true}) {
        require(allocation_probe::counts.live==0,"Tracked allocations survived the previous pass.");
        allocation_probe::counts={}; counters={}; useCandidate=candidate;
        allocation_probe::enabled=true;
        const auto actual=probeInspect(geometry,{}, {},limits);
        allocation_probe::enabled=false;
        const auto counts=allocation_probe::counts;
        require(signature(actual)==expected,"Allocation-pass result changed.");
        result["allocations"].push_back({{"candidate",candidate},{"calls",counts.calls},{"requested_bytes",counts.requested},
            {"peak_live_requested_bytes",counts.peak},{"retained_requested_bytes",counts.live}});
    }
    return result;
}
Mesh input(Mesh mesh) {
    auto duplicate=std::make_shared<DuplicateInput>(); DuplicateSource source;
    source.fileId=7; source.batchId=3; source.name="probe"; source.data=mesh.sourceData;
    for(size_t i=0;i<meshGeometry(mesh).nodes.size();++i) {
        const auto& node=meshGeometry(mesh).nodes[i]; source.parts.push_back({i+1,node.indexOffset,node.indexCount,source.unusedPointTransform});
    }
    duplicate->sources.push_back(std::move(source)); mesh.duplicateInput=std::move(duplicate); publishMeshGeometry(mesh); return mesh;
}
}
int main(int argc,char** argv) {
    try {
        const std::string_view command=argc>1?argv[1]:"";
        if(command=="--self-test") {
            selfTest(); std::cout<<Json{{"checked_pairs",checkedPairs},{"rational_sat_cases",checkedSat},{"checked_orientation_signs",checkedSigns},
#if defined(PREDICATES_FAST_FMA)
                {"predicates_fma_path",true},
#else
                {"predicates_fma_path",false},
#endif
                {"unsupported_range_examples",unsupportedExamples},{"status","pass"}}.dump(2)<<'\n'; return 0;
        }
        require(argc>=2,"Usage: probe --self-test | --synthetic [runs] | model.obj [runs]");
        const size_t runs=argc>2?static_cast<size_t>(std::stoul(argv[2])):5; require(runs>=1 && runs<=20,"Invalid run count.");
        Json output={{"notes","Release; unmeasured frequency/oracle pass, then alternating baseline/candidate; allocation pass separate. Same BVH/filters/budgets/order. Model uses all parts in one identity-transform source batch."},
            {"sizeof_cpp_rational",sizeof(Exact)},{"sizeof_rational_triangle",sizeof(std::array<ExactPoint,3>)}};
        if(command=="--synthetic") {
            for(bool curved:{false,true}) { const auto geometry=grid(64,curved); output[curved?"curved_grid":"flat_grid"]=benchmark(*geometry,runs); }
            output["duplicate_faces"]=benchmark(*duplicates(100),runs);
        } else {
            const auto mesh=input(loadObjMesh(argv[1])); output["model"]=argv[1]; output["faces"]=meshGeometry(mesh).indices.size()/3;
            for(auto mode:{TopologyMode::originalIndex,TopologyMode::exactPosition}) {
                const auto geometry=buildAnalysisGeometry(mesh.duplicateInput->sources,mode);
                output[topologyModeName(mode)]=benchmark(*geometry,runs,IntersectionLimits{});
                // A fixed real-model prefix also runs without result/work
                // limits. The vertex values/order remain identical.
                auto prefix=std::make_shared<AnalysisGeometry>(*geometry);
                for(auto& source:prefix->sources) {
                    auto copy=std::make_shared<SourceAnalysisGeometry>(*source);
                    if(copy->faces.size()>25000) { copy->faces.resize(25000); }
                    source=std::move(copy);
                }
                output[std::string(topologyModeName(mode))+"_first_25000_faces_complete"]=benchmark(*prefix,runs);
            }
        }
        std::cout<<output.dump(2)<<'\n';
    } catch(const std::exception& e) { allocation_probe::enabled=false; std::cerr<<e.what()<<'\n'; return 1; }
}
