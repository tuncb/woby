#include "obj_mesh.h"
#include "scene_buffer_size.h"
#include "utf8_path.h"
#include <rapidobj/prototype.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace woby;
using Clock=std::chrono::steady_clock;
#include "mapping.inc"
struct Input {
    std::vector<Coordinate> positions,normals;
    std::vector<std::array<double,2>> uvs;
    rapidobj::PrototypeResult parsed;
};
struct Baseline { std::vector<Vertex> vertices; std::vector<uint32_t> indices,sourceIndices,positionMap; bool identity=true; };
struct Corner { uint32_t position,normal,uv; };
static_assert(sizeof(Corner)==12);
struct Direct {
    std::vector<std::array<float,3>> positions,normals;
    std::vector<std::array<float,2>> uvs;
    std::vector<Corner> corners;
    std::vector<uint32_t> markerIds,representatives;
};
void check(bool ok,const char* error) { if(!ok) throw std::runtime_error(error); }
double ms(Clock::time_point begin) { return std::chrono::duration<double,std::milli>(Clock::now()-begin).count(); }
template<class F> void visit(const Input& in,F&& fn) { for(const auto& shape:in.parsed.polygons.shapes) for(const auto& i:shape.mesh.indices) fn(i); }
size_t cornerCount(const Input& in) { size_t count=0; for(const auto& shape:in.parsed.polygons.shapes) count+=shape.mesh.indices.size(); return count; }
Vertex gather(const Input& in,const rapidobj::Index& i) {
    Vertex v; v.position=renderPosition(in.positions[size_t(i.position_index)]);
    if(i.normal_index>=0) { const auto& n=in.normals[size_t(i.normal_index)]; v.normal={float(n[0]),float(n[1]),float(n[2])}; }
    if(i.texcoord_index>=0) { const auto& uv=in.uvs[size_t(i.texcoord_index)]; v.texcoord={float(uv[0]),1-float(uv[1])}; }
    return v;
}
Baseline map(const Input& in) {
    Baseline b; const auto count=cornerCount(in), estimate=std::min(in.positions.size(),count);
    b.vertices.reserve(estimate); b.indices.reserve(count); b.sourceIndices.reserve(count);
    VertexIndexTable table; reserveIndexTable(table,in.positions.size(),estimate,in.normals.empty()&&in.uvs.empty());
    visit(in,[&](const auto& i) {
        check(i.position_index>=0 && size_t(i.position_index)<in.positions.size(),"Invalid position");
        b.sourceIndices.push_back(uint32_t(i.position_index));
        const auto id=vertexIndex(table,{i.position_index,i.normal_index,i.texcoord_index});
        if(id==b.vertices.size()) {
            if(b.identity && size_t(i.position_index)!=b.vertices.size()) {
                b.positionMap.reserve(b.vertices.capacity());
                for(size_t j=0;j<b.vertices.size();++j) b.positionMap.push_back(uint32_t(j));
                b.identity=false;
            }
            if(!b.identity) b.positionMap.push_back(uint32_t(i.position_index));
            b.vertices.push_back(gather(in,i));
        }
        b.indices.push_back(id);
    });
    return b;
}
Direct pack(const Input& in,bool markers) {
    Direct d; d.positions.reserve(in.positions.size()); d.normals.reserve(in.normals.size()); d.uvs.reserve(in.uvs.size());
    for(const auto& p:in.positions) d.positions.push_back(renderPosition(p));
    for(const auto& n:in.normals) d.normals.push_back({float(n[0]),float(n[1]),float(n[2])});
    for(const auto& uv:in.uvs) d.uvs.push_back({float(uv[0]),1-float(uv[1])});
    const auto count=cornerCount(in); d.corners.reserve(count);
    VertexIndexTable table;
    if(markers) { d.markerIds.reserve(count); d.representatives.reserve(std::min(in.positions.size(),count)); reserveIndexTable(table,in.positions.size(),count,in.normals.empty()&&in.uvs.empty()); }
    visit(in,[&](const auto& i) {
        check(i.position_index>=0 && size_t(i.position_index)<in.positions.size(),"Invalid position");
        if(markers) {
            const auto id=vertexIndex(table,{i.position_index,i.normal_index,i.texcoord_index});
            if(id==d.representatives.size()) d.representatives.push_back(uint32_t(d.corners.size()));
            d.markerIds.push_back(id);
        }
        d.corners.push_back({uint32_t(i.position_index),uint32_t(i.normal_index),uint32_t(i.texcoord_index)});
    });
    return d;
}
Vertex fetch(const Direct& d,size_t index) {
    const auto& c=d.corners[index]; Vertex v; v.position=d.positions[c.position];
    if(c.normal!=UINT32_MAX) v.normal=d.normals[c.normal];
    if(c.uv!=UINT32_MAX) v.texcoord=d.uvs[c.uv];
    return v;
}
uint64_t checksum(const Vertex& v,uint64_t hash) {
    std::array<uint32_t,8> words; std::memcpy(words.data(),&v,sizeof(v));
    for(const auto word:words) hash=(hash^word)*1099511628211ull;
    return hash;
}
void validate(const Baseline& b,const Direct& d,bool markers) {
    check(b.indices.size()==d.corners.size(),"Corner count mismatch");
    for(size_t i=0;i<b.indices.size();++i) {
        const auto v=fetch(d,i); check(std::memcmp(&v,&b.vertices[b.indices[i]],sizeof(v))==0,"Attribute mismatch");
        if(markers) check(d.markerIds[i]==b.indices[i],"Marker order mismatch");
    }
    if(markers) for(size_t i=0;i<d.representatives.size();++i) check(d.markerIds[d.representatives[i]]==i,"Representative order mismatch");
}
Input parse(std::string_view text) {
    Input in; rapidobj::GeometryBuffers buffers{in.positions,in.uvs,in.normals}; rapidobj::PrototypeOptions settings; settings.material_names_only=true;
    in.parsed=rapidobj::ParseMemoryPrototype(text,buffers,rapidobj::MaterialLibrary::String(""),settings);
    check(!in.parsed.polygons.error,"Parse failed"); check(rapidobj::Triangulate(in.parsed.polygons,in.positions),"Triangulate failed"); return in;
}
void selfTest() {
    const std::string_view text="v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\nv 99 99 99\nvt 0 0\nvt 1 0\nvt 0 1\nvt 0 0\nvn 0 0 1\nvn 0 0 1\ng A\nf 1/1/1 2/2/1 3/3/1\ng B\nf 1/4/2 2/2/1 4/1/1\nf 1 3 4\n";
    const auto in=parse(text);
    const auto b=map(in); validate(b,pack(in,false),false); validate(b,pack(in,true),true);
    check(b.vertices.size()==8,"First-use tuple count changed");
    auto generated=b.vertices; generateSmoothNormals(generated,b.indices);
    const auto production=loadObjMeshText(text);
    const auto& actual=meshGeometry(production);
    check(actual.indices==b.indices && actual.vertices.size()==generated.size(),"Production tuple output changed");
    for(size_t i=0;i<generated.size();++i) check(std::memcmp(&actual.vertices[i],&generated[i],sizeof(Vertex))==0,"Generated-normal control mismatch");
    const auto direct=pack(in,true);
    for(size_t i=0;i<direct.corners.size();++i) {
        auto v=fetch(direct,i); v.normal=generated[direct.markerIds[i]].normal;
        check(std::memcmp(&v,&actual.vertices[actual.indices[i]],sizeof(Vertex))==0,"Tuple normal accessor mismatch");
    }
    const auto plain=parse("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 3 2 1\n");
    const auto p=map(plain); validate(p,pack(plain,false),false); validate(p,pack(plain,true),true);
    check(p.vertices.size()==3,"Position-only mapping changed");
}
}
int main(int argc,char** argv) {
    try {
        selfTest(); if(argc>1 && std::string_view(argv[1])=="--self-test") { std::cout<<"{\"ok\":true}\n"; return 0; }
        check(argc==2,"usage: woby_simplification_14 ABSOLUTE_OBJ_PATH");
        const auto path=pathFromUtf8(argv[1]); check(path.is_absolute(),"Absolute path required");
        Input in; rapidobj::GeometryBuffers buffers{in.positions,in.uvs,in.normals};
        const auto parseStart=Clock::now(); in.parsed=rapidobj::ParseFilePrototype(path,buffers); const double parseMs=ms(parseStart);
        check(!in.parsed.polygons.error,"OBJ parse failed");
        check(std::all_of(in.parsed.statements.begin(),in.parsed.statements.end(),[](const auto& statement) {
            return statement.kind==rapidobj::StatementKind::ordinary || statement.kind==rapidobj::StatementKind::group
                || statement.kind==rapidobj::StatementKind::materialLibrary;
        }),"Freeform input is outside this probe");
        const auto origin=coordinateOrigin(in.positions); for(auto& p:in.positions) p=relativePosition(p,origin);
        const auto triangulateStart=Clock::now(); check(rapidobj::Triangulate(in.parsed.polygons,in.positions),"Triangulation failed"); const double triMs=ms(triangulateStart);
        nlohmann::json samples=nlohmann::json::array(); Baseline b; Direct bare,compatible;
        for(size_t repeat=0;repeat<5;++repeat) {
            b={}; bare={}; compatible={};
            std::array<double,3> times{};
            for(size_t j=0;j<3;++j) {
                const auto which=(repeat+j)%3; const auto begin=Clock::now();
                if(which==0) b=map(in); else if(which==1) bare=pack(in,false); else compatible=pack(in,true);
                times[which]=ms(begin);
            }
            validate(b,bare,false); validate(b,compatible,true);
            samples.push_back({{"current_mapping_ms",times[0]},{"bare_corner_packing_ms",times[1]},{"marker_compatible_packing_ms",times[2]}});
        }
        uint64_t hash=14695981039346656037ull; nlohmann::json fetches=nlohmann::json::array();
        for(size_t repeat=0;repeat<5;++repeat) {
            auto begin=Clock::now(); for(size_t i=0;i<b.indices.size();++i) hash=checksum(b.vertices[b.indices[i]],hash); const auto unifiedMs=ms(begin);
            begin=Clock::now(); for(size_t i=0;i<bare.corners.size();++i) hash=checksum(fetch(bare,i),hash); const auto separateMs=ms(begin);
            fetches.push_back({{"unified_cpu_fetch_ms",unifiedMs},{"separate_cpu_fetch_ms",separateMs}});
        }
        const auto s=in.positions.size(),n=in.normals.size(),u=in.uvs.size(),c=b.indices.size(),r=b.vertices.size();
        const uint64_t attributes=12*s+12*n+8*u;
        const bool authoredNormals=std::all_of(b.vertices.begin(),b.vertices.end(),[](const Vertex& v){return validNormal(v.normal);});
        const uint64_t equivalentAttributes=12*s+12*(authoredNormals?n:r)+8*u;
        std::vector<uint8_t> usedPositions(s),usedNormals(n),usedUvs(u);
        visit(in,[&](const auto& i) {
            usedPositions.at(size_t(i.position_index))=1;
            if(i.normal_index>=0) usedNormals.at(size_t(i.normal_index))=1;
            if(i.texcoord_index>=0) usedUvs.at(size_t(i.texcoord_index))=1;
        });
        const auto usedS=size_t(std::count(usedPositions.begin(),usedPositions.end(),uint8_t{1}));
        const auto usedN=size_t(std::count(usedNormals.begin(),usedNormals.end(),uint8_t{1}));
        const auto usedU=size_t(std::count(usedUvs.begin(),usedUvs.end(),uint8_t{1}));
        const uint64_t compactAttributes=12*usedS+12*(authoredNormals?usedN:r)+8*usedU;
        std::cout<<nlohmann::json{{"ok",true},{"path",pathToUtf8(path)},{"parse_ms",parseMs},{"triangulation_ms",triMs},
            {"source_positions",s},{"normal_records",n},{"uv_records",u},{"corners",c},{"render_vertices",r},
            {"used_positions",usedS},{"used_normals",usedN},{"used_uvs",usedU},
            {"complete_authored_normals",authoredNormals},{"equivalent_direct_gpu_surface_bytes",equivalentAttributes+12*c},
            {"equivalent_marker_direct_gpu_bytes",equivalentAttributes+16*c+4*r},
            {"equivalent_representative_direct_gpu_bytes",equivalentAttributes+12*c+4*r},
            {"used_attributes_representative_direct_gpu_bytes",compactAttributes+12*c+4*r},
            {"current_gpu_surface_bytes",32*r+4*c},{"direct_gpu_surface_bytes",attributes+12*c},
            {"direct_with_marker_identity_bytes",attributes+16*c+4*r},
            {"current_incremental_cpu_bytes",32*r+4*b.positionMap.size()+4*c},{"position_indexing",b.identity?"identity":"mapped"},
            {"direct_incremental_cpu_bytes",attributes+8*c},{"direct_marker_incremental_cpu_bytes",attributes+12*c+4*r},
            {"equivalent_representative_incremental_cpu_bytes",equivalentAttributes+8*c+4*r},
            {"marker_generation_temp_keys_bytes",12*r},{"current_vertex_capacity_bytes",b.vertices.capacity()*sizeof(Vertex)},
            {"samples",samples},{"fetch_samples",fetches},{"checksum",hash},
            {"scope","Current production tuple-index helper and mapping representation versus separate attributes/corners. Parse and triangulate once. No generated normals, mixed primitives, application load total, GPU or frames."}}.dump()<<'\n';
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
