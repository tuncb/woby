// Research only: reproduce production CDT batches and count exceptional cells.
// Boundaries here are polylines. This does not replace production curve sampling.
#include <CDT.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numbers>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Point = std::array<double,2>;
using Loop = std::vector<Point>;
using Clock = std::chrono::steady_clock;
struct Fixture { std::string name; Loop outer; std::vector<Loop> holes; };
struct Input {
    std::vector<CDT::V2d<double>> vertices;
    std::vector<CDT::Edge> edges;
    std::vector<double> u,v;
};
struct Run {
    CDT::Triangulation<double> mesh{CDT::VertexInsertionOrder::Auto,CDT::IntersectingConstraintEdges::TryResolve,0.};
    double vertexMs=0,edgeMs=0,totalMs=0;
    size_t checkpoints=0;
};
void require(bool ok,const char* message) { if (!ok) { throw std::runtime_error(message); } }
double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
Loop circle(double radius) {
    Loop result;
    for (size_t i=0;i<512;++i) {
        const double t=2*std::numbers::pi*static_cast<double>(i)/512.;
        result.push_back({.5+radius*std::cos(t),.5+radius*std::sin(t)});
    }
    return result;
}
std::vector<Fixture> fixtures() {
    return {
        {"full_rectangle",{{0,0},{1,0},{1,1},{0,1}},{}},
        {"diagonal_strip",{{.02,.04},{.04,.02},{.98,.96},{.96,.98}},{}},
        {"annulus",circle(.48),{circle(.44)}},
        {"small_rectangle",{{.45,.45},{.55,.45},{.55,.55},{.45,.55}},{}},
        {"tiny_hole",{{0,0},{1,0},{1,1},{0,1}},{{{.501,.501},{.502,.501},{.502,.502},{.501,.502}}}}
    };
}
std::vector<double> croppedAxis(size_t cells,double low,double high) {
    std::vector<double> values;
    for (size_t i=0;i<=cells;++i) { values.push_back(static_cast<double>(i)/static_cast<double>(cells)); }
    auto first=std::lower_bound(values.begin(),values.end(),low);
    if (first!=values.begin()) { --first; }
    auto last=std::upper_bound(first,values.end(),high);
    if (last!=values.end()) { ++last; }
    return {first,last};
}
Input prepare(const Fixture& fixture,size_t cells) {
    Point low{1,1},high{0,0};
    for (const auto& p:fixture.outer) for (size_t k=0;k<2;++k) {
        low[k]=std::min(low[k],p[k]); high[k]=std::max(high[k],p[k]);
    }
    Input input; input.u=croppedAxis(cells,low[0],high[0]); input.v=croppedAxis(cells,low[1],high[1]);
    const auto nu=static_cast<uint32_t>(input.u.size()),nv=static_cast<uint32_t>(input.v.size());
    for (double y:input.v) for (double x:input.u) { input.vertices.push_back({x,y}); }
    for (uint32_t y=0;y<nv;++y) for (uint32_t x=0;x<nu;++x) {
        const auto index=y*nu+x;
        if (x+1<nu) { input.edges.emplace_back(index,index+1); }
        if (y+1<nv) { input.edges.emplace_back(index,index+nu); }
    }
    const auto append=[&](const Loop& loop) {
        const auto offset=static_cast<uint32_t>(input.vertices.size());
        for (const auto& p:loop) { input.vertices.push_back({p[0],p[1]}); }
        for (uint32_t i=0;i<loop.size();++i) { input.edges.emplace_back(offset+i,offset+(i+1)%static_cast<uint32_t>(loop.size())); }
    };
    append(fixture.outer); for (const auto& hole:fixture.holes) { append(hole); }
    CDT::RemoveDuplicatesAndRemapEdges(input.vertices,input.edges);
    return input;
}
Run run(const Input& input,bool iterators) {
    Run result;
    const auto start=Clock::now();
    for (size_t first=0;first<input.vertices.size();first+=1024) {
        ++result.checkpoints;
        const auto a=input.vertices.begin()+static_cast<ptrdiff_t>(first);
        const auto b=input.vertices.begin()+static_cast<ptrdiff_t>(std::min(first+1024,input.vertices.size()));
        if (iterators) { result.mesh.insertVertices(a,b,CDT::getX_V2d<double>,CDT::getY_V2d<double>); }
        else { result.mesh.insertVertices(std::vector<CDT::V2d<double>>(a,b)); }
    }
    result.vertexMs=elapsed(start);
    const auto edgeStart=Clock::now();
    for (size_t first=0;first<input.edges.size();first+=256) {
        ++result.checkpoints;
        const auto a=input.edges.begin()+static_cast<ptrdiff_t>(first);
        const auto b=input.edges.begin()+static_cast<ptrdiff_t>(std::min(first+256,input.edges.size()));
        if (iterators) { result.mesh.insertEdges(a,b,CDT::edge_get_v1,CDT::edge_get_v2); }
        else { result.mesh.insertEdges(std::vector<CDT::Edge>(a,b)); }
    }
    result.edgeMs=elapsed(edgeStart);
    result.mesh.eraseSuperTriangle(); result.totalMs=elapsed(start);
    return result;
}
void equal(const Run& a,const Run& b) {
    require(a.checkpoints==b.checkpoints,"Checkpoint count differs");
    require(a.mesh.vertices.size()==b.mesh.vertices.size(),"Vertex count differs");
    require(a.mesh.triangles.size()==b.mesh.triangles.size(),"Triangle count differs");
    for (size_t i=0;i<a.mesh.vertices.size();++i) {
        require(a.mesh.vertices[i].x==b.mesh.vertices[i].x && a.mesh.vertices[i].y==b.mesh.vertices[i].y,"Vertex differs");
    }
    for (size_t i=0;i<a.mesh.triangles.size();++i) {
        require(a.mesh.triangles[i].vertices==b.mesh.triangles[i].vertices,"Triangle order differs");
        require(a.mesh.triangles[i].neighbors==b.mesh.triangles[i].neighbors,"Neighbors differ");
    }
    require(a.mesh.fixedEdges==b.mesh.fixedEdges,"Constraints differ");
}
bool inside(const Loop& loop,Point p) {
    bool result=false;
    for (size_t i=0;i<loop.size();++i) {
        const auto& a=loop[i]; const auto& b=loop[(i+1)%loop.size()];
        if ((a[1]>p[1])!=(b[1]>p[1])) {
            const double orientation=predicates::adaptive::orient2d(a.data(),b.data(),p.data());
            if ((orientation>0)==(b[1]>a[1])) { result=!result; }
        }
    }
    return result;
}
// Closed boxes deliberately mark both cells when a boundary lies on a grid edge.
// This is a measurement helper, not a production robust intersection algorithm.
bool touches(Point a,Point b,Point low,Point high) {
    double lo=0,hi=1;
    for (size_t k=0;k<2;++k) {
        const double d=b[k]-a[k];
        if (d==0) { if (a[k]<low[k] || a[k]>high[k]) { return false; } }
        else {
            double x=(low[k]-a[k])/d,y=(high[k]-a[k])/d;
            if (x>y) { std::swap(x,y); }
            lo=std::max(lo,x); hi=std::min(hi,y);
            if (lo>hi) { return false; }
        }
    }
    return true;
}
nlohmann::json classify(const Fixture& fixture,const Input& input) {
    const auto start=Clock::now();
    size_t boundary=0,interior=0,exterior=0;
    const auto crosses=[&](const Loop& loop,Point low,Point high) {
        for (size_t i=0;i<loop.size();++i) if (touches(loop[i],loop[(i+1)%loop.size()],low,high)) { return true; }
        return false;
    };
    for (size_t y=0;y+1<input.v.size();++y) for (size_t x=0;x+1<input.u.size();++x) {
        const Point low{input.u[x],input.v[y]},high{input.u[x+1],input.v[y+1]};
        bool exceptional=crosses(fixture.outer,low,high);
        for (const auto& hole:fixture.holes) { exceptional=exceptional || crosses(hole,low,high); }
        if (exceptional) { ++boundary; continue; }
        const Point center{std::midpoint(low[0],high[0]),std::midpoint(low[1],high[1])};
        bool retained=inside(fixture.outer,center);
        for (const auto& hole:fixture.holes) { retained=retained && !inside(hole,center); }
        if (retained) { ++interior; } else { ++exterior; }
    }
    return {{"boundary_cells",boundary},{"interior_cells",interior},{"exterior_cells",exterior},
        {"cropped_cells",boundary+interior+exterior},{"naive_classification_ms",elapsed(start)}};
}
void selfTest() {
    for (const auto& fixture:fixtures()) for (size_t cells:{8u,32u}) {
        const auto input=prepare(fixture,cells);
        equal(run(input,false),run(input,true));
        const auto counts=classify(fixture,input);
        require(counts.at("cropped_cells").get<size_t>()==(input.u.size()-1)*(input.v.size()-1),"Cell coverage differs");
        if (fixture.name=="full_rectangle") {
            require(counts.at("boundary_cells").get<size_t>()==4*cells-4,"Aligned boundary lost");
            require(counts.at("interior_cells").get<size_t>()==(cells-2)*(cells-2),"Interior count differs");
        }
        if (fixture.name=="tiny_hole") { require(counts.at("boundary_cells").get<size_t>()==4*cells-3,"Subcell hole lost"); }
    }
    require(touches({.5,.5},{.6,.6},{.5,0},{1,.5}),"Corner contact lost");
    require(!touches({.6,.6},{.7,.7},{0,0},{.5,.5}),"Disjoint cell marked");
}
double median(std::vector<double> values) { std::sort(values.begin(),values.end()); return values[values.size()/2]; }
}
int main(int argc,char** argv) {
    try {
        selfTest();
        if (argc==2 && std::string(argv[1])=="--self-test") { std::cout<<"CDT batch and cell checks passed\n"; return 0; }
        require(argc==1,"Usage: woby_trim_input_probe [--self-test]");
        nlohmann::json output={{"repetitions",7},{"timing_scope","CDT insertion and supertriangle removal; excludes input preparation, classification and final filtering"},
            {"fixtures",nlohmann::json::array()}};
        for (const auto& fixture:fixtures()) for (size_t cells:{32u,128u,256u}) {
            const auto input=prepare(fixture,cells);
            equal(run(input,false),run(input,true)); // Warm both paths.
            std::array<std::vector<double>,2> total,vertex,edge;
            for (size_t repeat=0;repeat<7;++repeat) {
                const bool first=(repeat%2)!=0;
                auto a=run(input,first),b=run(input,!first); equal(a,b);
                for (size_t mode=0;mode<2;++mode) {
                    const auto& r=(mode==static_cast<size_t>(first)) ? a : b;
                    total[mode].push_back(r.totalMs); vertex[mode].push_back(r.vertexMs); edge[mode].push_back(r.edgeMs);
                }
            }
            auto row=classify(fixture,input);
            row["fixture"]=fixture.name; row["domain_cells_per_axis"]=cells;
            row["input_vertices"]=input.vertices.size(); row["input_edges"]=input.edges.size();
            row["removed_copy_bytes"]=input.vertices.size()*sizeof(CDT::V2d<double>)+input.edges.size()*sizeof(CDT::Edge);
            row["removed_batch_allocations"]=(input.vertices.size()+1023)/1024+(input.edges.size()+255)/256;
            row["max_temporary_payload_bytes"]=std::max(std::min(input.vertices.size(),size_t{1024})*sizeof(CDT::V2d<double>),std::min(input.edges.size(),size_t{256})*sizeof(CDT::Edge));
            row["exact_cdt_output_equal"]=true;
            for (size_t mode=0;mode<2;++mode) {
                row[mode ? "iterator" : "copied"]={{"median_ms",median(total[mode])},{"vertex_median_ms",median(vertex[mode])},
                    {"edge_median_ms",median(edge[mode])},{"total_samples_ms",total[mode]}};
            }
            output["fixtures"].push_back(row);
        }
        std::cout<<output.dump(2)<<'\n';
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
