#include "scene_queries.h"
#include "math_utils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace woby;
using Clock=std::chrono::steady_clock;
struct SlopeAllocation { size_t live=0,peak=0,calls=0; };
struct SlopeMetrics { size_t entries=0,requestedBytes=0,allocations=0; };
SlopeMetrics slopeMetrics;
template<class T> struct SlopeAllocator {
    using value_type=T;
    SlopeAllocation* record=nullptr;
    SlopeAllocator()=default;
    explicit SlopeAllocator(SlopeAllocation& value):record(&value) {}
    template<class U> SlopeAllocator(const SlopeAllocator<U>& other):record(other.record) {}
    T* allocate(size_t count) {
        auto* result=std::allocator<T>{}.allocate(count);
        if(record) { record->live+=count*sizeof(T); record->peak=std::max(record->peak,record->live); ++record->calls; }
        return result;
    }
    void deallocate(T* pointer,size_t count) { if(record) record->live-=count*sizeof(T); std::allocator<T>{}.deallocate(pointer,count); }
    template<class U> bool operator==(const SlopeAllocator<U>& other) const { return record==other.record; }
};
#include "projection_float.inc"
#include "projection_cached.inc"
void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
struct Fixture { Mesh mesh; UiAnnotation item; std::vector<ScenePickPart> parts; AnnotationQuery query; ScenePickView view; };
Fixture fixture(size_t count,bool oblique,bool bridges,bool nearClip,bool uniqueFaces=false) {
    Fixture f;
    auto& g=editMeshGeometry(f.mesh);
    const uint32_t cells=uniqueFaces ? uint32_t(count) : 64u;
    for(uint32_t i=0;i<cells;++i) {
        const float x=float(i%8)/4-.9f,y=float((i%64)/8)/4-.9f;
        const float z=nearClip ? -.1f : .4f;
        const uint32_t first=uint32_t(g.vertices.size());
        g.vertices.push_back({{x,y,z},{},{}}); g.vertices.push_back({{x+.2f,y,z+(oblique?.2f:0)},{},{}});
        g.vertices.push_back({{x,y+.2f,z+(oblique?.1f:0)},{},{}});
        g.indices.insert(g.indices.end(),{first,first+1,first+2});
    }
    f.item.objectId=2; f.item.targetId=1; f.item.targetValid=true;
    AnnotationGeometry geometry;
    geometry.segments.reserve(count);
    for(size_t i=0;i<count;++i) {
        AnnotationSegment s; s.triangle=uint32_t(i%cells); s.a={.8f,.1f,.1f}; s.b={.1f,.8f,.1f};
        if(bridges && i%2==0) s.endTriangle=(s.triangle+1)%cells;
        geometry.segments.push_back(s);
    }
    f.item.geometry=shareAnnotationGeometry(std::move(geometry));
    ScenePickPart part; part.objectId=1; part.mesh=&f.mesh; part.indexCount=g.indices.size(); identityMatrix(part.model.data());
    f.parts.push_back(part); f.query.builds=1;
    identityMatrix(f.view.view.data()); identityMatrix(f.view.renderProjection.data());
    f.view.width=1920; f.view.height=1080;
    return f;
}
void repair(Fixture& f) {
    f.parts[0].mesh=&f.mesh;
    std::vector<const ScenePickPart*> sources;
    annotationWorldLines(f.item,f.parts,f.query.lines,sources);
}
void equalProjection(const AnnotationQuery& a,const AnnotationQuery& b) {
    check(a.projected.size()==b.projected.size(),"Slope-cache output count mismatch");
    check(a.projected.empty() || std::memcmp(a.projected.data(),b.projected.data(),a.projected.size()*sizeof(a.projected.front()))==0,
        "Slope-cache projection is not bitwise equivalent");
}
template<class F> double measure(F&& fn,size_t rounds) {
    const auto start=Clock::now(); for(size_t i=0;i<rounds;++i) fn(i);
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count()/double(rounds);
}
nlohmann::json precisionCases();
void selfTest() {
    for(bool bridges : {false,true}) {
        auto f=fixture(80,true,bridges,false); repair(f);
        updateAnnotationProjection(f.query,f.item,f.parts,f.view);
        check(f.query.projected.size()==480,"Visible segment count mismatch");
        AnnotationQuery cached; cached.lines=f.query.lines; cached.builds=1;
        slopeCachedProjection(cached,f.item,f.parts,f.view); equalProjection(f.query,cached);
        AnnotationQuery singlePrecision; singlePrecision.lines=f.query.lines; singlePrecision.builds=1;
        floatAnnotationProjection(singlePrecision,f.item,f.parts,f.view);
        check(singlePrecision.projected.size()==f.query.projected.size(),"Float-slope output count mismatch");
        for(size_t i=0;i<f.query.projected.size();++i) for(size_t axis=0;axis<3;++axis)
            check(std::abs(singlePrecision.projected[i][axis]-f.query.projected[i][axis])<1e-5f,"Stable triangle float-slope mismatch");
        const auto initial=f.query.projected;
        updateAnnotationProjection(f.query,f.item,f.parts,f.view); check(f.query.projectionBuilds==1,"Cache missed");
        f.view.view[12]=.01f; updateAnnotationProjection(f.query,f.item,f.parts,f.view);
        check(f.query.projectionBuilds==2 && f.query.projected!=initial,"Camera invalidation failed");
        check(std::all_of(f.query.projected.begin(),f.query.projected.end(),[](const auto& v){return std::all_of(v.begin(),v.end(),[](float x){return std::isfinite(x);});}),"Invalid projection");
    }
    auto clipped=fixture(10,true,false,true); repair(clipped);
    updateAnnotationProjection(clipped.query,clipped.item,clipped.parts,clipped.view);
    check(!clipped.query.projected.empty(),"Near clipping lost crossing lines");
    for(const auto& v:clipped.query.projected) check(std::isfinite(v[2]),"Near clipping emitted invalid depth");
    AnnotationQuery nearCached; nearCached.lines=clipped.query.lines; nearCached.builds=1;
    slopeCachedProjection(nearCached,clipped.item,clipped.parts,clipped.view); equalProjection(clipped.query,nearCached);
    auto multi=fixture(128,true,true,false); repair(multi);
    auto second=multi.parts.front(); second.objectId=3; second.model[12]=.025f; multi.parts.push_back(second);
    multi.item.targetIds={1,3};
    auto multiGeometry=*multi.item.geometry;
    for(size_t i=0;i<multiGeometry.segments.size();++i) {
        auto& s=multiGeometry.segments[i]; s.source=uint32_t(i%2);
        if(s.endTriangle) s.endSource=uint32_t((i+1)%2);
    }
    multi.item.geometry=shareAnnotationGeometry(std::move(multiGeometry));
    repair(multi); AnnotationQuery multiCached; multiCached.lines=multi.query.lines; multiCached.builds=1;
    updateAnnotationProjection(multi.query,multi.item,multi.parts,multi.view);
    slopeCachedProjection(multiCached,multi.item,multi.parts,multi.view); equalProjection(multi.query,multiCached);
    auto unique=fixture(128,true,false,false,true); repair(unique);
    AnnotationQuery uniqueCached; uniqueCached.lines=unique.query.lines; uniqueCached.builds=1;
    updateAnnotationProjection(unique.query,unique.item,unique.parts,unique.view);
    slopeCachedProjection(uniqueCached,unique.item,unique.parts,unique.view); equalProjection(unique.query,uniqueCached);
    (void)precisionCases();
}
nlohmann::json precisionCases() {
    nlohmann::json rows=nlohmann::json::array();
    for(float epsilon : {1.0f,.01f,.0001f,.000001f,.0000001f}) {
        auto f=fixture(1,false,false,false);
        auto& g=editMeshGeometry(f.mesh);
        g.vertices[0].position={-.45f,-.45f,.2f};
        g.vertices[1].position={.45f,.45f,.8f};
        g.vertices[2].position={.4f,.4f+epsilon,.5f};
        repair(f);
        double maximumDepth=0,maximumXY=0; size_t countsDiffer=0; bool finite=true;
        for(size_t rotation=0;rotation<180;++rotation) {
            const float angle=float(rotation)*.0008f;
            f.view.view[0]=std::cos(angle); f.view.view[2]=std::sin(angle);
            f.view.view[8]=-std::sin(angle); f.view.view[10]=std::cos(angle);
            AnnotationQuery floatQuery; floatQuery.lines=f.query.lines; floatQuery.builds=1;
            updateAnnotationProjection(f.query,f.item,f.parts,f.view);
            floatAnnotationProjection(floatQuery,f.item,f.parts,f.view);
            AnnotationQuery exactCached; exactCached.lines=f.query.lines; exactCached.builds=1;
            slopeCachedProjection(exactCached,f.item,f.parts,f.view); equalProjection(f.query,exactCached);
            if(f.query.projected.size()!=floatQuery.projected.size()) { ++countsDiffer; continue; }
            for(size_t i=0;i<f.query.projected.size();++i) for(size_t axis=0;axis<3;++axis) {
                const auto delta=std::abs(double(f.query.projected[i][axis])-double(floatQuery.projected[i][axis]));
                finite=finite&&std::isfinite(delta);
                if(axis==2) maximumDepth=std::max(maximumDepth,delta); else maximumXY=std::max(maximumXY,delta);
            }
        }
        rows.push_back({{"triangle_y_separation",epsilon},{"camera_cases",180},{"max_ndc_depth_difference",maximumDepth},
            {"max_ndc_xy_difference",maximumXY},{"different_output_counts",countsDiffer},{"finite",finite}});
    }
    return rows;
}
}
int main(int argc,char** argv) {
    try {
        selfTest();
        if(argc>1 && std::string_view(argv[1])=="--self-test") { std::cout<<"{\"ok\":true}\n"; return 0; }
        nlohmann::json rows=nlohmann::json::array(); uint64_t checksum=0;
        for(size_t count : {size_t{10},size_t{1000},size_t{20000},size_t{100000}}) {
          for(bool uniqueFaces : {false,true}) {
            for(const auto mode : {0,1,2,3}) {
                auto f=fixture(count,mode!=0,mode==2,mode==3,uniqueFaces); repair(f);
                updateAnnotationProjection(f.query,f.item,f.parts,f.view);
                AnnotationQuery slopeQuery; slopeQuery.lines=f.query.lines; slopeQuery.builds=1;
                slopeCachedProjection(slopeQuery,f.item,f.parts,f.view); equalProjection(f.query,slopeQuery);
                const size_t rounds=std::max(size_t{20},size_t{1000000}/count);
                std::vector<std::array<float,3>> upload(f.query.projected.size());
                nlohmann::json samples=nlohmann::json::array();
                for(size_t repeat=0;repeat<5;++repeat) {
                    const auto motion=[&](size_t i,bool cached) {
                        const float angle=float(i%240)*.0005f;
                        f.view.view[0]=std::cos(angle); f.view.view[2]=std::sin(angle);
                        f.view.view[8]=-std::sin(angle); f.view.view[10]=std::cos(angle);
                        if(cached) slopeCachedProjection(slopeQuery,f.item,f.parts,f.view);
                        else updateAnnotationProjection(f.query,f.item,f.parts,f.view);
                    };
                    double project=0,slopeCached=0;
                    if(repeat%2==0) {
                        project=measure([&](size_t i){motion(i,false);},rounds);
                        slopeCached=measure([&](size_t i){motion(i,true);},rounds);
                    } else {
                        slopeCached=measure([&](size_t i){motion(i,true);},rounds);
                        project=measure([&](size_t i){motion(i,false);},rounds);
                    }
                    equalProjection(f.query,slopeQuery);
                    const double cached=measure([&](size_t){updateAnnotationProjection(f.query,f.item,f.parts,f.view);},rounds);
                    upload.resize(f.query.projected.size());
                    const double copy=measure([&](size_t i){
                        std::memcpy(upload.data(),f.query.projected.data(),upload.size()*sizeof(upload.front()));
                        if(!upload.empty()) checksum+=uint64_t(std::abs(upload[i%upload.size()][0])*10000);
                    },rounds);
                    samples.push_back({{"camera_projection_ms",project},{"slope_cached_projection_ms",slopeCached},{"cache_hit_ms",cached},{"memcpy_ms",copy}});
                }
                rows.push_back({{"segments",count},{"source_triangles",meshGeometry(f.mesh).indices.size()/3},{"unique_faces",uniqueFaces},
                    {"mode",mode==0?"flat":mode==1?"oblique":mode==2?"half_bridges":"near_crossing"},
                    {"world_cache_bytes",f.query.lines.capacity()*sizeof(DiagnosticEdge)},
                    {"projection_capacity_bytes",f.query.projected.capacity()*sizeof(f.query.projected.front())},
                    {"submitted_bytes",f.query.projected.size()*sizeof(f.query.projected.front())},
                    {"anchor_bytes",f.item.geometry->segments.size()*sizeof(AnnotationSegment)},
                    {"slope_cache_entries",slopeMetrics.entries},{"slope_cache_requested_bytes",slopeMetrics.requestedBytes},
                    {"slope_cache_allocations",slopeMetrics.allocations},
                    {"rounds",rounds},{"samples",samples}});
            }
          }
        }
        std::cout<<nlohmann::json{{"ok",true},{"rows",rows},{"checksum",checksum},
            {"float_slope_precision",precisionCases()},
            {"scope","Production annotation projection and standalone memcpy; no GPU upload, driver staging, frame or candidate shader timing."}}.dump()<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
