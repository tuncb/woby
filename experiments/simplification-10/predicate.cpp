#include "probe.h"
#include "adaptive_predicates.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

#if defined(_M_FP_FAST) || defined(__FAST_MATH__)
#error "Exact intersection predicates require strict floating point arithmetic"
#endif
static_assert(sizeof(void*)==8 && sizeof(double)==8 && std::numeric_limits<double>::is_iec559);

namespace intersection_probe {
namespace {
int sign(double value) { return (value>0)-(value<0); }
int orient2(const Position& a,const Position& b,const Position& c,size_t x,size_t y) {
    ++counters.signs2d;
    return sign(intersection_probe_predicates::adaptive::orient2d(a[x],a[y],b[x],b[y],c[x],c[y]));
}
int orient3(const Position& a,const Position& b,const Position& c,const Position& d) {
    ++counters.signs3d;
    return sign(intersection_probe_predicates::adaptive::orient3d(a.data(),b.data(),c.data(),d.data()));
}
size_t projection(const Triangle& triangle) {
    for (size_t axis=0;axis<3;++axis) {
        if (orient2(triangle[0],triangle[1],triangle[2],(axis+1)%3,(axis+2)%3)) { return axis; }
    }
    return 3;
}
bool within(const Position& p,const Position& a,const Position& b,size_t x,size_t y) {
    return p[x]>=std::min(a[x],b[x]) && p[x]<=std::max(a[x],b[x])
        && p[y]>=std::min(a[y],b[y]) && p[y]<=std::max(a[y],b[y]);
}
bool contained(const Position& p,const Triangle& triangle,size_t axis) {
    const size_t x=(axis+1)%3,y=(axis+2)%3;
    const int inside=orient2(triangle[0],triangle[1],triangle[2],x,y);
    for(size_t i=0;i<3;++i) {
        const int value=orient2(triangle[i],triangle[(i+1)%3],p,x,y);
        if(value && value!=inside) { return false; }
    }
    return true;
}
bool allowed(const Position& p,const std::array<Position,3>& shared,size_t count) {
    if(count==1) { return p==shared[0]; }
    // The main function handles all two-shared-vertex cases before edge tests.
    return false;
}
bool segment2(const Position& a,const Position& b,const Position& c,const Position& d,
    size_t axis,const std::array<Position,3>& shared,size_t count) {
    const size_t x=(axis+1)%3,y=(axis+2)%3;
    const int ac=orient2(a,b,c,x,y),ad=orient2(a,b,d,x,y);
    const int ca=orient2(c,d,a,x,y),cb=orient2(c,d,b,x,y);
    if(ac*ad<0 && ca*cb<0) { return true; }
    // A closed segment intersection that is not proper contains an original
    // endpoint. Testing each endpoint also detects collinear overlap beyond
    // an allowed common vertex, without constructing an intersection point.
    if(!ac && within(c,a,b,x,y) && !allowed(c,shared,count)) { return true; }
    if(!ad && within(d,a,b,x,y) && !allowed(d,shared,count)) { return true; }
    if(!ca && within(a,c,d,x,y) && !allowed(a,shared,count)) { return true; }
    return !cb && within(b,c,d,x,y) && !allowed(b,shared,count);
}
bool segmentTriangle(const Position& a,const Position& b,const Triangle& triangle,
    size_t axis,const std::array<Position,3>& shared,size_t count) {
    const int da=orient3(triangle[0],triangle[1],triangle[2],a);
    const int db=orient3(triangle[0],triangle[1],triangle[2],b);
    if(da && da==db) { return false; }
    if(!da && !db) {
        if(contained(a,triangle,axis) && !allowed(a,shared,count)) { return true; }
        if(contained(b,triangle,axis) && !allowed(b,shared,count)) { return true; }
        for(size_t i=0;i<3;++i) {
            if(segment2(a,b,triangle[i],triangle[(i+1)%3],axis,shared,count)) { return true; }
        }
        return false;
    }
    if(!da) { return contained(a,triangle,axis) && !allowed(a,shared,count); }
    if(!db) { return contained(b,triangle,axis) && !allowed(b,shared,count); }
    // Opposite plane signs put the plane crossing inside the segment. The
    // three tetrahedron signs classify that crossing against triangle edges.
    std::array<int,3> sides{};
    for(size_t i=0;i<3;++i) { sides[i]=orient3(a,b,triangle[i],triangle[(i+1)%3]); }
    return (*std::min_element(sides.begin(),sides.end())>=0
        || *std::max_element(sides.begin(),sides.end())<=0);
}
}
void enteredRational(const Triangle& a,const Triangle& b,const Ids& aIds,const Ids& bIds) {
    ++counters.rational;
    size_t shared=0;
    for(size_t i=0;i<3;++i) {
        for(size_t j=0;j<3;++j) { if(aIds[i]==bIds[j] && a[i]==b[j]) { ++shared; } }
    }
    ++counters.rationalShared[std::min(size_t{3},shared)];
    if(capturedPairs && capturedPairs->size()<captureLimit) { capturedPairs->push_back({a,b,aIds,bIds}); }
}
bool predicate(const Triangle& a,const Triangle& b,const Ids& aIds,const Ids& bIds) {
    bool safe=true;
    constexpr double lower=0x1p-200,upper=0x1p200;
    for(const auto* triangle:{&a,&b}) {
        for(const auto& p:*triangle) { for(const double value:p) {
            if(!std::isfinite(value)) { throw std::invalid_argument("Non-finite intersection coordinate."); }
            safe &= value==0 || (std::abs(value)>=lower && std::abs(value)<=upper);
        } }
    }
    if(!safe) {
        ++counters.guarded;
        const bool saved=useCandidate; useCandidate=false;
        const auto answer=woby::probeBaseline(a,b,aIds,bIds);
        useCandidate=saved; return answer;
    }
    const size_t ap=projection(a),bp=projection(b);
    if(ap==3 || bp==3) { return false; }
    std::array<Position,3> shared{};
    size_t count=0,aOther=0,bOther=0;
    for(size_t i=0;i<3;++i) {
        bool found=false;
        for(size_t j=0;j<3;++j) { if(aIds[i]==bIds[j] && a[i]==b[j]) {
            if(count<3) { shared[count]=a[i]; } ++count; found=true;
        } }
        if(!found) { aOther=i; }
    }
    // Repeated IDs at different coordinates do not establish shared topology.
    // A noncollapsed triangle has three distinct positions, so count <= 3.
    if(count==3) { return true; }
    if(count==2) {
        for(size_t j=0;j<3;++j) {
            bool found=false;
            for(size_t i=0;i<3;++i) { found|=aIds[i]==bIds[j] && a[i]==b[j]; }
            if(!found) { bOther=j; }
        }
        if(orient3(a[0],a[1],a[2],b[bOther])) { return false; }
        const size_t x=(ap+1)%3,y=(ap+2)%3;
        return orient2(shared[0],shared[1],a[aOther],x,y)
            ==orient2(shared[0],shared[1],b[bOther],x,y);
    }
    for(size_t i=0;i<3;++i) {
        if(segmentTriangle(a[i],a[(i+1)%3],b,bp,shared,count)
            || segmentTriangle(b[i],b[(i+1)%3],a,ap,shared,count)) { return true; }
    }
    return false;
}
}
