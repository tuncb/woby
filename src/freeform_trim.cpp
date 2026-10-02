#include "freeform_trim.h"
#include <CDT.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <limits>
#include <stdexcept>

namespace woby {
namespace {
using UV = std::array<double,2>;
using HPoint = std::array<double,3>; // Weighted normalized UV and weight.
constexpr double joinTolerance = 1e-10, uvTolerance = 1e-5;
constexpr size_t maxBoundarySamples = 16384;
struct Loop {
    std::vector<UV> points;
    size_t region=0, line=0;
    bool hole=false;
    UV low{1,1}, high{0,0};
    std::array<std::vector<size_t>,64> crossings;
};
using RegionRange = std::array<size_t,2>;
struct TrimContext {
    const FreeformPatch& patch;
    const ModelLoadProgressCallback& progress;
    size_t line=0, samples=0;
    double spatialTolerance=0;
};
[[noreturn]] void fail(const TrimContext& c, const std::string& reason) {
    throw std::runtime_error("Invalid freeform trimming: " + c.patch.trimming->sourceFile
        + (c.line ? " (line " + std::to_string(c.line) + ")" : "") + ": " + reason);
}
double distance(const UV& a,const UV& b) { return std::hypot(a[0]-b[0],a[1]-b[1]); }
UV normalized(const FreeformPatch& p,const Coordinate& a) {
    return {(a[0]-p.domainU[0])/(p.domainU[1]-p.domainU[0]),(a[1]-p.domainV[0])/(p.domainV[1]-p.domainV[0])};
}
UV parameters(const FreeformPatch& p,const UV& a) {
    return {std::lerp(p.domainU[0],p.domainU[1],a[0]),std::lerp(p.domainV[0],p.domainV[1],a[1])};
}
UV euclidean(const HPoint& p) { return {p[0]/p[2],p[1]/p[2]}; }
UV checked(TrimContext& c,UV p) {
    for (auto& x:p) {
        if (!std::isfinite(x) || x < -joinTolerance || x > 1+joinTolerance) { fail(c,"Boundary leaves the surface parameter range."); }
        if (std::abs(x)<joinTolerance) { x=0; }
        if (std::abs(x-1)<joinTolerance) { x=1; }
    }
    return p;
}
Coordinate mapped(TrimContext& c,const UV& p) {
    const auto uv=parameters(c.patch,checked(c,p));
    return evaluateFreeform(c.patch,uv[0],uv[1]).position;
}
double chordDistance(const UV& p,const UV& a,const UV& b) {
    const double dx=b[0]-a[0],dy=b[1]-a[1], length2=dx*dx+dy*dy;
    const double t=length2>0 ? std::clamp(((p[0]-a[0])*dx+(p[1]-a[1])*dy)/length2,0.,1.) : 0;
    return distance(p,{std::lerp(a[0],b[0],t),std::lerp(a[1],b[1],t)});
}
std::pair<std::vector<HPoint>,std::vector<HPoint>> split(const std::vector<HPoint>& control) {
    auto work=control;
    std::vector<HPoint> left{work.front()},right{work.back()};
    for (size_t n=work.size()-1;n>0;--n) {
        for (size_t i=0;i<n;++i) for (size_t k=0;k<3;++k) { work[i][k]=std::midpoint(work[i][k],work[i+1][k]); }
        left.push_back(work[0]); right.push_back(work[n-1]);
    }
    std::reverse(right.begin(),right.end());
    return {std::move(left),std::move(right)};
}
void flatten(TrimContext& c,const std::vector<HPoint>& controls,std::vector<UV>& result,unsigned depth=0) {
    if ((c.samples%128)==0) { reportModelLoadProgress(c.progress,ModelLoadStage::triangulating); }
    const auto a=checked(c,euclidean(controls.front())),b=checked(c,euclidean(controls.back()));
    const auto halves=split(controls);
    const auto middle=euclidean(halves.first.back());
    double flatness=0;
    double previousProjection=-std::numeric_limits<double>::infinity();
    bool monotone=true;
    for (const auto& control:controls) {
        const auto uv=euclidean(control);
        const double projection=(uv[0]-a[0])*(b[0]-a[0])+(uv[1]-a[1])*(b[1]-a[1]);
        monotone=monotone && projection>=previousProjection; previousProjection=projection;
    }
    for (const auto& control:controls) { flatness=std::max(flatness,chordDistance(euclidean(control),a,b)); }
    const auto pa=mapped(c,a),pb=mapped(c,b),pm=mapped(c,middle);
    const double error=std::hypot(pm[0]-std::midpoint(pa[0],pb[0]),pm[1]-std::midpoint(pa[1],pb[1]),pm[2]-std::midpoint(pa[2],pb[2]));
    // Positive rational Bezier weights keep the entire UV curve inside its
    // control hull. That bound catches loops/inflections missed by midpoint tests.
    if (monotone && flatness<=uvTolerance && error<=c.spatialTolerance) {
        if (++c.samples>maxBoundarySamples) { fail(c,"Boundary sampling exceeds 16384 vertices."); }
        result.push_back(b); return;
    }
    if (depth==24) { fail(c,"Boundary sampling could not meet its error tolerance."); }
    flatten(c,halves.first,result,depth+1); flatten(c,halves.second,result,depth+1);
}

// Insert a knot once in homogeneous coordinates, preserving the rational curve.
void insertKnot(std::vector<HPoint>& points,std::vector<double>& knots,uint32_t degree,double t) {
    const size_t count=points.size();
    const size_t span=static_cast<size_t>(std::upper_bound(knots.begin(),knots.end(),t)-knots.begin()-1);
    const size_t multiplicity=static_cast<size_t>(std::count(knots.begin(),knots.end(),t));
    std::vector<HPoint> next(count+1);
    for (size_t i=0;i<=span-degree;++i) { next[i]=points[i]; }
    for (size_t i=span-multiplicity;i<count;++i) { next[i+1]=points[i]; }
    for (size_t i=span-degree+1;i<=span-multiplicity;++i) {
        const double alpha=(t-knots[i])/(knots[i+degree]-knots[i]);
        for (size_t k=0;k<3;++k) { next[i][k]=std::lerp(points[i-1][k],points[i][k],alpha); }
    }
    knots.insert(knots.begin()+static_cast<ptrdiff_t>(span+1),t); points=std::move(next);
}
std::vector<UV> sampleSegment(TrimContext& c,const FreeformTrimSegment& segment) {
    if (!segment.curve || segment.curve->surface || segment.curve->controls.size()>4096) { fail(c,"Invalid trimming curve."); }
    const auto& curve=*segment.curve;
    validateFreeformPatch(curve);
    const double lo=std::min(segment.interval[0],segment.interval[1]),hi=std::max(segment.interval[0],segment.interval[1]);
    if (!std::isfinite(lo) || !std::isfinite(hi) || lo>=hi || lo<curve.domainU[0] || hi>curve.domainU[1]) { fail(c,"Invalid trimming curve interval."); }
    std::vector<double> breaks{lo,hi},knots=curve.knotsU;
    for (double t:knots) if (t>lo && t<hi) { breaks.push_back(t); }
    std::sort(breaks.begin(),breaks.end()); breaks.erase(std::unique(breaks.begin(),breaks.end()),breaks.end());
    double maxWeight=0;
    for (const auto& p:curve.controls) { maxWeight=std::max(maxWeight,p[3]); }
    std::vector<HPoint> points;
    for (const auto& p:curve.controls) {
        const auto uv=normalized(c.patch,{p[0],p[1],0}); const double w=p[3]/maxWeight;
        if (!(w>0) || !std::isfinite(uv[0]*w) || !std::isfinite(uv[1]*w)) { fail(c,"Trimming curve exceeds the numeric range."); }
        points.push_back({uv[0]*w,uv[1]*w,w});
    }
    for (double t:breaks) {
        reportModelLoadProgress(c.progress,ModelLoadStage::triangulating);
        while (std::count(knots.begin(),knots.end(),t)<curve.degreeU) { insertKnot(points,knots,curve.degreeU,t); }
    }
    std::vector<UV> result;
    for (size_t i=0;i+1<breaks.size();++i) {
        const size_t span=static_cast<size_t>(std::upper_bound(knots.begin(),knots.end(),breaks[i])-knots.begin()-1);
        std::vector<HPoint> controls(points.begin()+static_cast<ptrdiff_t>(span-curve.degreeU),points.begin()+static_cast<ptrdiff_t>(span+1));
        if (result.empty()) { result.push_back(checked(c,euclidean(controls.front()))); }
        flatten(c,controls,result);
    }
    if (segment.interval[0]>segment.interval[1]) { std::reverse(result.begin(),result.end()); }
    return result;
}
Loop sampleLoop(TrimContext& c,const FreeformTrimLoop& input,size_t region,bool hole) {
    c.line=input.sourceLine;
    Loop loop; loop.region=region; loop.line=input.sourceLine; loop.hole=hole;
    if (input.segments.empty()) {
        if (hole) { fail(c,"Empty hole boundary."); }
        loop.points={{0,0},{1,0},{1,1},{0,1}}; return loop;
    }
    for (const auto& segment:input.segments) {
        auto points=sampleSegment(c,segment);
        if (loop.points.empty()) { loop.points.push_back(points.front()); }
        else if (distance(loop.points.back(),points.front())>joinTolerance) { fail(c,"Boundary segments do not meet."); }
        loop.points.insert(loop.points.end(),points.begin()+1,points.end());
    }
    if (distance(loop.points.front(),loop.points.back())>joinTolerance) { fail(c,"Boundary is not closed."); }
    loop.points.pop_back();
    if (loop.points.size()<3) { fail(c,"Boundary needs at least three distinct points."); }
    return loop;
}
double orient(const UV& a,const UV& b,const UV& p) { return predicates::adaptive::orient2d(a.data(),b.data(),p.data()); }
size_t bucket(double y) { return static_cast<size_t>(std::clamp(y*64,0.,63.)); }
void indexLoop(Loop& loop) {
    for (size_t i=0;i<loop.points.size();++i) {
        const auto& a=loop.points[i]; const auto& b=loop.points[(i+1)%loop.points.size()];
        for (size_t k=0;k<2;++k) { loop.low[k]=std::min(loop.low[k],a[k]); loop.high[k]=std::max(loop.high[k],a[k]); }
        if (a[1]==b[1]) { continue; }
        for (size_t y=bucket(std::min(a[1],b[1]));y<=bucket(std::max(a[1],b[1]));++y) { loop.crossings[y].push_back(i); }
    }
}
bool inside(const Loop& loop,const UV& p) {
    if (p[0]<loop.low[0] || p[0]>loop.high[0] || p[1]<loop.low[1] || p[1]>loop.high[1]) { return false; }
    bool in=false;
    for (auto i:loop.crossings[bucket(p[1])]) {
        const auto& a=loop.points[i]; const auto& b=loop.points[(i+1)%loop.points.size()];
        if ((a[1]>p[1])!=(b[1]>p[1]) && ((orient(a,b,p)>0)==(b[1]>a[1]))) { in=!in; }
    }
    return in;
}
bool kept(const std::vector<Loop>& loops,const UV& p,const RegionRange& region) {
    if (!inside(loops[region[0]],p)) { return false; }
    for (size_t i=region[0]+1;i<region[1];++i) if (inside(loops[i],p)) { return false; }
    return true;
}
void validateLoops(TrimContext& c,const std::vector<Loop>& loops,const std::vector<RegionRange>& regions) {
    struct Edge { UV a,b; size_t loop,index; double xmin,xmax,ymin,ymax; };
    std::vector<Edge> edges;
    for (size_t n=0;n<loops.size();++n) {
        c.line=loops[n].line; const auto& points=loops[n].points;
        double area=0;
        for (size_t i=0;i<points.size();++i) {
            const auto& a=points[i]; const auto& b=points[(i+1)%points.size()];
            if (distance(a,b)<=1e-14) { fail(c,"Boundary has a collapsed segment."); }
            area+=orient(points[0],a,b);
            edges.push_back({a,b,n,i,std::min(a[0],b[0]),std::max(a[0],b[0]),std::min(a[1],b[1]),std::max(a[1],b[1])});
        }
        if (area==0) { fail(c,"Boundary has zero area."); }
    }
    std::stable_sort(edges.begin(),edges.end(),[](const Edge& a,const Edge& b) { return a.xmin<b.xmin; });
    for (size_t i=0;i<edges.size();++i) {
        if (i%128==0) { reportModelLoadProgress(c.progress,ModelLoadStage::triangulating); }
        const auto& a=edges[i]; c.line=loops[a.loop].line;
        for (size_t j=i+1;j<edges.size() && edges[j].xmin<=a.xmax;++j) {
            const auto& b=edges[j];
            if (b.ymin>a.ymax || a.ymin>b.ymax) { continue; }
            const double ab=orient(a.a,a.b,b.a),ac=orient(a.a,a.b,b.b),ba=orient(b.a,b.b,a.a),bc=orient(b.a,b.b,a.b);
            if ((ab>0 && ac>0) || (ab<0 && ac<0) || (ba>0 && bc>0) || (ba<0 && bc<0)) { continue; }
            if (a.loop==b.loop && ((a.index+1)%loops[a.loop].points.size()==b.index || (b.index+1)%loops[a.loop].points.size()==a.index)) {
                const double dot=(a.b[0]-a.a[0])*(b.b[0]-b.a[0])+(a.b[1]-a.a[1])*(b.b[1]-b.a[1]);
                if (ab!=0 || ac!=0 || dot>0) { continue; }
            }
            fail(c,"Boundaries intersect, overlap or touch.");
        }
    }
    for (const auto& loop:loops) {
        c.line=loop.line;
        if (loop.hole) {
            for (const auto& other:loops) if (other.region==loop.region && &loop!=&other) {
                const bool contained=inside(other,loop.points[0]);
                if ((!other.hole && !contained) || (other.hole && contained)) { fail(c,"Hole is outside its region or overlaps another hole."); }
            }
        } else {
            for (size_t region=0;region<c.patch.trimming->regions.size();++region) {
                if (region!=loop.region && kept(loops,loop.points[0],regions[region])) { fail(c,"Trim regions overlap."); }
            }
        }
    }
}
} // namespace

void triangulateFreeformTrim(const FreeformPatch& patch,FreeformGrid& grid,const ModelLoadProgressCallback& progress) {
    if (!patch.trimming) { return; }
    TrimContext c{patch,progress};
    if (!patch.surface || patch.trimming->regions.empty() || patch.trimming->regions.size()>256) { fail(c,"Invalid trim regions."); }
    Coordinate low{patch.controls[0][0],patch.controls[0][1],patch.controls[0][2]},high=low;
    for (const auto& p:patch.controls) for (size_t k=0;k<3;++k) { low[k]=std::min(low[k],p[k]); high[k]=std::max(high[k],p[k]); }
    c.spatialTolerance=std::max(1e-12,std::hypot(high[0]-low[0],high[1]-low[1],high[2]-low[2])*1e-4);
    std::vector<Loop> loops;
    std::vector<RegionRange> regions;
    for (size_t i=0;i<patch.trimming->regions.size();++i) {
        const auto& region=patch.trimming->regions[i];
        const size_t first=loops.size();
        loops.push_back(sampleLoop(c,region.outer,i,false));
        for (const auto& hole:region.holes) { loops.push_back(sampleLoop(c,hole,i,true)); }
        regions.push_back({first,loops.size()});
        if (loops.size()>512) { fail(c,"Too many trim loops."); }
    }
    for (auto& loop:loops) { indexLoop(loop); }
    validateLoops(c,loops,regions);
    // Constrain every grid edge as well as trim boundaries. CDT splits their
    // intersections, so neighboring cells share vertices and knot lines survive.
    const size_t nu=grid.u.size(),nv=grid.v.size();
    size_t boundary=0; for (const auto& loop:loops) { boundary+=loop.points.size(); }
    if (nu*nv+boundary*(nu+nv+1)>maxFreeformVertices) { fail(c,"Trim tessellation exceeds the work limit."); }
    std::vector<CDT::V2d<double>> vertices;
    std::vector<CDT::Edge> edges;
    for (double v:grid.v) for (double u:grid.u) {
        const auto p=normalized(patch,{u,v,0}); vertices.push_back({p[0],p[1]});
    }
    for (size_t y=0;y<nv;++y) for (size_t x=0;x<nu;++x) {
        const auto i=static_cast<uint32_t>(y*nu+x);
        if (x+1<nu) { edges.emplace_back(i,i+1); }
        if (y+1<nv) { edges.emplace_back(i,i+static_cast<uint32_t>(nu)); }
    }
    for (const auto& loop:loops) {
        const auto offset=static_cast<uint32_t>(vertices.size());
        for (const auto& p:loop.points) { vertices.push_back({p[0],p[1]}); }
        for (size_t i=0;i<loop.points.size();++i) { edges.emplace_back(offset+static_cast<uint32_t>(i),offset+static_cast<uint32_t>((i+1)%loop.points.size())); }
    }
    CDT::RemoveDuplicatesAndRemapEdges(vertices,edges);
    CDT::Triangulation<double> triangulation(CDT::VertexInsertionOrder::Auto,CDT::IntersectingConstraintEdges::TryResolve,0.);
    try {
        // Batch insertion leaves regular opportunities for import cancellation.
        for (size_t first=0;first<vertices.size();first+=1024) {
            reportModelLoadProgress(progress,ModelLoadStage::triangulating);
            const auto last=std::min(first+1024,vertices.size());
            triangulation.insertVertices(std::vector<CDT::V2d<double>>(vertices.begin()+static_cast<ptrdiff_t>(first),vertices.begin()+static_cast<ptrdiff_t>(last)));
        }
        for (size_t first=0;first<edges.size();first+=256) {
            reportModelLoadProgress(progress,ModelLoadStage::triangulating);
            const auto last=std::min(first+256,edges.size());
            triangulation.insertEdges(std::vector<CDT::Edge>(edges.begin()+static_cast<ptrdiff_t>(first),edges.begin()+static_cast<ptrdiff_t>(last)));
            if (triangulation.vertices.size()>maxFreeformVertices) { fail(c,"Trim tessellation exceeds the vertex limit."); }
        }
        triangulation.eraseSuperTriangle();
    } catch (const CDT::Error& error) { fail(c,error.what()); }
    std::vector<uint32_t> remap(triangulation.vertices.size(),UINT32_MAX);
    grid.samples.clear(); grid.triangles.clear();
    size_t visited=0;
    for (const auto& triangle:triangulation.triangles) {
        if (++visited%1024==0) { reportModelLoadProgress(progress,ModelLoadStage::triangulating); }
        UV center{}; std::array<UV,3> corners;
        for (size_t k=0;k<3;++k) {
            const auto& p=triangulation.vertices[triangle.vertices[k]]; corners[k]={p.x,p.y}; center[0]+=p.x/3; center[1]+=p.y/3;
        }
        bool retain=false;
        for (const auto& region:regions) { if (kept(loops,center,region)) { retain=true; break; } }
        if (!retain || orient(corners[0],corners[1],corners[2])==0) { continue; }
        auto indices=triangle.vertices;
        if (orient(corners[0],corners[1],corners[2])<0) { std::swap(indices[1],indices[2]); }
        for (auto old:indices) {
            if (remap[old]==UINT32_MAX) {
                remap[old]=static_cast<uint32_t>(grid.samples.size());
                const auto& p=triangulation.vertices[old]; grid.samples.push_back(parameters(patch,checked(c,{p.x,p.y})));
            }
            grid.triangles.push_back(remap[old]);
        }
    }
    if (grid.triangles.empty()) { fail(c,"Trimming leaves no triangles."); }
}
} // namespace woby
