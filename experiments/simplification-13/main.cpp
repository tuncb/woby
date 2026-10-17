// Include the production distance implementation to use its private BVH and
// narrow phase unchanged. The static library does not supply this object again.
#include "../../src/mesh_comparison.cpp"
#include "comparison_scene.h"
#include "obj_mesh.h"
#include "point_cloud.h"
#include "surface_annotation.h"
#include <nlohmann/json.hpp>
#include <bit>
#include <chrono>
#include <iostream>

namespace probe {
using namespace woby;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
struct GenericPointRecord {
    std::array<double,3> low{},high{};
    size_t begin=0,count=0,left=0,right=0;
    uint32_t proxyBegin=0,proxyCount=0,group=0;
    std::array<uint32_t,6> extrema{};
};
void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
template<class T> size_t bytes(const std::vector<T>& v) { return v.capacity() * sizeof(T); }
size_t treeBytes(const DistanceTree& t) { return bytes(t.triangles) + bytes(t.nodes) + bytes(t.order); }
size_t meshBytes(const Mesh& m) {
    const auto& g = meshGeometry(m);
    return bytes(g.vertices) + bytes(g.precisePositions) + bytes(g.indices) + bytes(g.analysisVertexBatches);
}
template<class F> auto timed(double& ms, F work) {
    const auto start = Clock::now(); auto result = work();
    ms = std::chrono::duration<double,std::milli>(Clock::now()-start).count(); return result;
}
Mesh grid(unsigned n) {
    Mesh m; auto& g = editMeshGeometry(m);
    for (unsigned y=0;y<=n;++y) { for(unsigned x=0;x<=n;++x) {
        const double z=.1*std::sin(x*.17)*std::cos(y*.13);
        g.precisePositions.push_back({double(x),double(y),z});
        Vertex v; v.position=renderPosition(g.precisePositions.back()); g.vertices.push_back(v);
    } }
    for(unsigned y=0;y<n;++y) { for(unsigned x=0;x<n;++x) {
        const auto a=y*(n+1)+x,b=a+1,c=a+n+1,d=c+1;
        g.indices.insert(g.indices.end(),{a,b,d,a,d,c});
    } }
    MeshNode node; node.name="grid"; node.indexCount=static_cast<uint32_t>(g.indices.size()); g.nodes.push_back(node);
    m.bounds=calculateBounds(g.vertices); publishMeshGeometry(m); return m;
}
struct Instance {
    CoordinateMatrix model{};
    double scale=1;
    DistanceBounds bounds;
};
Instance instance(const Mesh& mesh,unsigned id,bool rotate,double scale) {
    Instance t; coordinateIdentity(t.model.data()); t.scale=scale;
    const double angle=rotate?.731:0,c=std::cos(angle),s=std::sin(angle);
    t.model[0]=scale*c; t.model[1]=scale*s; t.model[4]=-scale*s; t.model[5]=scale*c; t.model[10]=scale;
    std::array<Coordinate,2> bounds;
    bounds[0].fill(std::numeric_limits<double>::infinity());
    bounds[1].fill(-std::numeric_limits<double>::infinity());
    for(size_t i=0;i<meshGeometry(mesh).vertices.size();++i) {
        const auto p=meshPosition(mesh,i);
        for(size_t k=0;k<3;++k) {bounds[0][k]=std::min(bounds[0][k],p[k]);bounds[1][k]=std::max(bounds[1][k],p[k]);}
    }
    const double width=bounds[1][0]-bounds[0][0]+bounds[1][1]-bounds[0][1]+10;
    t.model[12]=id*width*scale; t.model[13]=17; t.model[14]=3;
    t.bounds.minimum.fill(std::numeric_limits<double>::infinity());
    t.bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    for(unsigned bits=0;bits<8;++bits) {
        Coordinate p{}; for(size_t k=0;k<3;++k) { p[k]=bounds[(bits>>k)&1][k]; }
        p=transformCoordinate(t.model.data(),p);
        for(size_t k=0;k<3;++k) { t.bounds.minimum[k]=std::min(t.bounds.minimum[k],p[k]); t.bounds.maximum[k]=std::max(t.bounds.maximum[k],p[k]); }
    }
    // Ordinary-range prototype padding only; a production fast path needs a
    // proved outward rounding policy and rejection of unsafe exponent ranges.
    for(size_t k=0;k<3;++k) {
        const double pad=16*std::numeric_limits<double>::epsilon()*std::max({1.,std::abs(t.bounds.minimum[k]),std::abs(t.bounds.maximum[k])});
        t.bounds.minimum[k]-=pad;t.bounds.maximum[k]+=pad;
    }
    return t;
}
Point inverse(const Instance& t,const Point& p) {
    Point q{},r{}; for(size_t k=0;k<3;++k) { q[k]=p[k]-t.model[12+k]; }
    for(size_t k=0;k<3;++k) { for(size_t j=0;j<3;++j) { r[k]+=t.model[k*4+j]*q[j]/(t.scale*t.scale); } }
    return r;
}
double boxDistance(const Instance& t,const Point& p) {
    double result=0; for(size_t k=0;k<3;++k) { const auto d=p[k]-std::clamp(p[k],t.bounds.minimum[k],t.bounds.maximum[k]); result+=d*d; } return result;
}
Mesh world(const Mesh& mesh,const std::vector<Instance>& instances) {
    ComparisonInputSnapshot snapshot;
    for(size_t i=0;i<instances.size();++i) {
        UiFileState file; file.mesh=mesh; file.objectId=i+1; file.path="instance.obj";
        file.groupSettings.resize(meshGeometry(mesh).nodes.size());
        snapshot.files.push_back(std::move(file));
        for(size_t j=0;j<meshGeometry(mesh).nodes.size();++j) {
            if(meshGeometry(mesh).nodes[j].indexCount) { snapshot.parts[0].push_back({i,j,instances[i].model,instances[i].model}); }
        }
    }
    return comparisonWorldMesh(snapshot,ComparisonSide::a);
}
std::vector<double> worldQueries(const DistanceTree& tree,const std::vector<Point>& queries) {
    std::vector<double> values; values.reserve(queries.size()); uint32_t hint=0;
    for(const auto& p:queries) { double best=0; nearest(tree,p,best,hint); values.push_back(std::sqrt(best)); } return values;
}
std::vector<double> localQueries(const DistanceTree& tree,const std::vector<Instance>& instances,const std::vector<Point>& queries) {
    std::vector<double> values; values.reserve(queries.size()); std::vector<uint32_t> hints(instances.size());
    for(const auto& p:queries) {
        double best=std::numeric_limits<double>::infinity();
        for(size_t i=0;i<instances.size();++i) {
            const auto& t=instances[i]; if(boxDistance(t,p)>best) { continue; }
            double local=0; nearest(tree,inverse(t,p),local,hints[i]); best=std::min(best,local*t.scale*t.scale);
        }
        values.push_back(std::sqrt(best));
    } return values;
}
Json errors(const std::vector<double>& a,const std::vector<double>& b) {
    require(a.size()==b.size(),"Query cardinality changed"); double maximum=0,relative=0; size_t unequal=0,thresholdFlips=0;
    for(size_t i=0;i<a.size();++i) {
        require(std::isfinite(a[i])&&std::isfinite(b[i]),"Non-finite query result");
        const auto delta=std::abs(a[i]-b[i]); maximum=std::max(maximum,delta); relative=std::max(relative,delta/std::max(1.,a[i]));
        unequal+=std::bit_cast<uint64_t>(a[i])!=std::bit_cast<uint64_t>(b[i]); thresholdFlips+=(a[i]>.25)!=(b[i]>.25);
    }
    return Json{{"max_absolute_error",maximum},{"max_relative_to_max_1_distance_error",relative},{"bitwise_unequal_queries",unequal},{"tolerance_0_25_classification_changes",thresholdFlips}};
}
Json run(const Mesh& mesh,unsigned count,bool rotate,double scale,unsigned samples,unsigned queryCount) {
    const auto metadataStart=Clock::now();
    std::vector<Instance> instances; for(unsigned i=0;i<count;++i) { instances.push_back(instance(mesh,i,rotate,scale)); }
    const auto metadataMs=std::chrono::duration<double,std::milli>(Clock::now()-metadataStart).count();
    const auto triangles=meshTriangles(mesh,{}); std::vector<Point> queries; queries.reserve(queryCount);
    for(unsigned i=0;i<queryCount;++i) {
        const auto& t=triangles[(size_t(i)*7919)%triangles.size()]; Point p{};
        for(size_t k=0;k<3;++k) { p[k]=(t[0][k]+t[1][k]+t[2][k])/3; }
        p[2]+=(i%3)*.2+.1; queries.push_back(transformCoordinate(instances[i%count].model.data(),p));
    }
    Json runs=Json::array();
    // Warm both production and candidate code paths before collecting samples.
    { const auto a=buildTree(world(mesh,instances),{}),b=buildTree(mesh,{}); (void)worldQueries(a,queries); (void)localQueries(b,instances,queries); }
    for(unsigned sample=0;sample<samples;++sample) {
        double bakeMs=0,worldBuildMs=0,localBuildMs=0,worldQueryMs=0,localQueryMs=0;
        Mesh baked; DistanceTree worldTree,localTree;
        const auto buildWorld=[&] { baked=timed(bakeMs,[&]{return world(mesh,instances);}); worldTree=timed(worldBuildMs,[&]{return buildTree(baked,{});}); };
        const auto buildLocal=[&] { localTree=timed(localBuildMs,[&]{return buildTree(mesh,{});}); };
        if(sample%2) { buildLocal(); buildWorld(); } else { buildWorld(); buildLocal(); }
        std::vector<double> a,b;
        const auto wa=[&]{a=timed(worldQueryMs,[&]{return worldQueries(worldTree,queries);});};
        const auto lb=[&]{b=timed(localQueryMs,[&]{return localQueries(localTree,instances,queries);});};
        if(sample%2) {lb();wa();} else {wa();lb();}
        const auto error=errors(a,b); require(error["max_relative_to_max_1_distance_error"].get<double>()<1e-7,"Ordinary-coordinate distance error exceeded 1e-7");
        runs.push_back({{"bake_ms",bakeMs},{"world_tree_build_ms",worldBuildMs},{"local_tree_build_ms",localBuildMs},
            {"world_query_ms",worldQueryMs},{"local_query_ms",localQueryMs},{"world_mesh_array_capacity_bytes",meshBytes(baked)},
            {"world_tree_array_capacity_bytes",treeBytes(worldTree)},{"local_tree_array_capacity_bytes",treeBytes(localTree)},
            {"instance_array_capacity_bytes",bytes(instances)},{"distance_equivalence",error}});
    }
    return Json{{"instances",count},{"rotation_radians",rotate?.731:0.},{"scale",scale},{"queries",queryCount},{"source_faces",triangles.size()},
        {"initial_uncached_instance_metadata_ms",metadataMs},
        {"samples",runs},{"candidate_top_level","linear padded experimental world box scan; no TLAS build"}};
}
Json extreme() {
    Mesh m; auto& g=editMeshGeometry(m); g.precisePositions={{0,0,0},{1,0,0},{0,1,0}};
    for(const auto& p:g.precisePositions) { Vertex v; v.position=renderPosition(p);g.vertices.push_back(v); }
    g.indices={0,1,2}; MeshNode node;node.name="extreme";node.indexCount=3;g.nodes.push_back(node);publishMeshGeometry(m);
    auto t=instance(m,0,false,1);t.model[12]=1e16;t.model[13]=1e16;t.model[14]=0;
    const auto a=buildTree(world(m,{t}),{}),b=buildTree(m,{});const Point p{1e16+2,1e16,0};
    double worldDistance=0,localDistance=0;uint32_t hint=0;nearest(a,p,worldDistance,hint);hint=0;nearest(b,inverse(t,p),localDistance,hint);
    require(worldDistance==4&&localDistance==1,"Extreme-coordinate counterexample changed");
    return Json{{"world_distance",std::sqrt(worldDistance)},{"local_distance",std::sqrt(localDistance)},
        {"translation",1e16},{"reason","World bake rounds distinct unit vertices to the same position. The local tree retains the unit triangle."}};
}
void selfTest() {
    (void)run(grid(3),2,true,.5,1,64);(void)extreme();
    std::stop_source stop;stop.request_stop();bool canceled=false;
    try { (void)buildTree(grid(2),stop.get_token()); } catch(const std::runtime_error&) {canceled=true;}
    require(canceled,"Production distance tree cancellation lost");
}
}
int main(int argc,char** argv) {
    try {
        if(argc==2&&std::string_view(argv[1])=="--self-test") {probe::selfTest();return 0;}
        using namespace probe; Json output{{"extreme_counterexample",extreme()},{"sizeof",{{"distance_node",sizeof(DistanceNode)},
            {"distance_triangle",sizeof(Triangle)},{"point_cloud_node",sizeof(points::Node)},{"mesh_annotation_node",sizeof(MeshAnnotationNode)},
            {"annotation_projection_node",sizeof(AnnotationProjectionNode)},{"hypothetical_generic_point_record",sizeof(GenericPointRecord)}}},
            {"notes","Release stage timings, one warmup per case, alternating build/query order, no concurrent work. CPU capacity payload excludes input mesh, metadata, allocator overhead and process RSS. Linear instance broad phase is a feasibility prototype. This is not a full distance-comparison or renderer benchmark."}};
        if(argc==2) { const auto mesh=loadObjMesh(argv[1]);output["model"]=argv[1];output["cases"]={run(mesh,1,true,1,5,2048),run(mesh,1,true,.01,5,2048)}; }
        else { require(argc==1,"Usage: woby_instance_query_probe [model.obj|--self-test]");const auto mesh=grid(80);
            output["cases"]={run(mesh,1,false,1,7,4096),run(mesh,1,true,.5,7,4096),run(mesh,8,true,1,7,4096),run(mesh,32,true,1,7,4096)}; }
        std::cout<<output.dump(2)<<'\n';
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
