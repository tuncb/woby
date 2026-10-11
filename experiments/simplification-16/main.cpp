#include "allocation_probe.h"
#include "obj_mesh.h"
#include "mesh_topology.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <numeric>
#include <set>
#include <cmath>
#include <numbers>

namespace {
using namespace woby;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
constexpr uint64_t max32 = UINT32_MAX;
uint32_t checked32(uint64_t value) {
    if (value > max32) { throw std::overflow_error("Compact topology capacity exceeded"); }
    return static_cast<uint32_t>(value);
}
// Proved requires immutable geometry from buildAnalysisGeometry and the existing
// cardinality preflights. Keep arithmetic wide until this final narrowing.
template<bool Proved> uint32_t construction32(uint64_t value) {
    if constexpr (Proved) { return static_cast<uint32_t>(value); }
    else { return checked32(value); }
}
bool countsFit(uint64_t vertices, uint64_t faces, uint64_t edges, uint64_t references, uint64_t boundaryIncidences) {
    return vertices <= max32 && faces <= max32 / 3 && edges <= max32 / 2
        && references <= max32 && boundaryIncidences <= max32;
}
struct Slice { uint32_t offset = 0, count = 0; };
template<class T> std::span<const T> view(const std::vector<T>& owner, Slice slice) {
    return std::span<const T>(owner).subspan(slice.offset, slice.count);
}
template<class Id> struct VertexRecord { Slice references, faces, edges, boundaryEdges; };
template<class Id> struct FaceRecord { std::array<Id,3> edges{}; Id component = 0; };
template<class Id> struct EdgeUse { Id face = 0; bool forward = false; };
template<class Id> struct EdgeRecord {
    std::array<Id,2> vertices{}; Slice incidentFaces;
    bool windingConflict = false, orientationContradiction = false;
};
template<class Id> struct Graph {
    std::shared_ptr<const SourceAnalysisGeometry> geometry;
    std::vector<VertexRecord<Id>> vertices;
    std::vector<FaceRecord<Id>> faces;
    std::vector<EdgeRecord<Id>> edges;
    std::vector<Id> vertexFaces, vertexEdges, boundaryEdges;
    std::vector<EdgeUse<Id>> edgeUses;
    std::vector<std::vector<Id>> components;
};
void canceled(std::stop_token stop) { if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); } }
// Same counting sort, canonical edge order, component BFS and flip constraints
// as buildSourceIncidence. Only record storage and temporary ID widths differ.
template<class Id, bool Proved = false> Graph<Id> build(std::shared_ptr<const SourceAnalysisGeometry> geometry, std::stop_token stop = {}) {
    canceled(stop);
    Graph<Id> result; result.geometry = std::move(geometry); const auto& g = *result.geometry;
    require(countsFit(g.vertices.size(),g.faces.size(),0,g.pointReferences.size(),0),"Compact preflight failed");
    result.vertices.resize(g.vertices.size()); result.faces.resize(g.faces.size());
    for (size_t v = 0; v < result.vertices.size(); ++v) {
        canceled(stop); result.vertices[v].references = {construction32<Proved>(g.referenceOffsets[v]),construction32<Proved>(g.referenceOffsets[v+1]-g.referenceOffsets[v])};
    }
    {
        const size_t corners = result.faces.size()*3;
        result.vertexFaces.resize(corners); result.edgeUses.resize(corners);
        std::vector<Id> degrees(result.vertices.size()), starts(result.vertices.size()+1);
        for (const auto& face : g.faces) {
            canceled(stop);
            for (size_t k=0;k<3;++k) {
                ++degrees[face.vertices[k]]; ++starts[std::min(face.vertices[k],face.vertices[(k+1)%3])+1];
            }
        }
        size_t offset=0;
        for (size_t v=0;v<result.vertices.size();++v) {
            result.vertices[v].faces={construction32<Proved>(offset),construction32<Proved>(degrees[v])}; offset+=degrees[v]; starts[v+1]+=starts[v];
        }
        std::vector<std::array<Id,2>> entries(corners); auto cursor=starts;
        std::fill(degrees.begin(),degrees.end(),Id{});
        for (size_t f=0;f<result.faces.size();++f) {
            canceled(stop); const auto& ids=g.faces[f].vertices;
            for (size_t k=0;k<3;++k) {
                const auto a=ids[k],b=ids[(k+1)%3];
                entries[cursor[std::min(a,b)]++]={static_cast<Id>(construction32<Proved>(std::max(a,b))),static_cast<Id>(construction32<Proved>(f*3+k))};
                result.vertexFaces[result.vertices[a].faces.offset+degrees[a]++]=static_cast<Id>(f);
            }
        }
        std::fill(degrees.begin(),degrees.end(),Id{}); std::vector<Id> boundaryDegrees(result.vertices.size());
        size_t edgeCount=0;
        for (size_t a=0;a<result.vertices.size();++a) {
            canceled(stop);
            std::sort(entries.begin()+static_cast<ptrdiff_t>(starts[a]),entries.begin()+static_cast<ptrdiff_t>(starts[a+1]),
                [&](const auto& x,const auto& y){canceled(stop);return x<y;});
            for (size_t i=starts[a];i<starts[a+1];++i) { canceled(stop); edgeCount+=i==starts[a]||entries[i][0]!=entries[i-1][0]; }
        }
        require(countsFit(g.vertices.size(),g.faces.size(),edgeCount,g.pointReferences.size(),0),"Compact edge count failed");
        result.edges.reserve(edgeCount);
        for (size_t a=0;a<result.vertices.size();++a) {
            canceled(stop);
            for (size_t begin=starts[a];begin<starts[a+1];) {
                const auto b=entries[begin][0]; size_t end=begin+1;
                while(end<starts[a+1]&&entries[end][0]==b) { ++end; }
                const auto index=result.edges.size();
                for(size_t i=begin;i<end;++i) {
                    canceled(stop); const size_t corner=entries[i][1];
                    result.edgeUses[i]={static_cast<Id>(corner/3),g.faces[corner/3].vertices[corner%3]==a};
                    result.faces[corner/3].edges[corner%3]=static_cast<Id>(index);
                }
                const auto uses=view(result.edgeUses,Slice{construction32<Proved>(begin),construction32<Proved>(end-begin)});
                EdgeRecord<Id> edge; edge.vertices={static_cast<Id>(a),b}; edge.incidentFaces={construction32<Proved>(begin),construction32<Proved>(end-begin)};
                edge.windingConflict=uses.size()==2&&uses[0].forward==uses[1].forward;
                result.edges.push_back(edge); ++degrees[a]; ++degrees[b];
                if(uses.size()==1) { ++boundaryDegrees[a]; ++boundaryDegrees[b]; }
                begin=end;
            }
        }
        std::vector<std::array<Id,2>>().swap(entries); std::vector<Id>().swap(starts); std::vector<Id>().swap(cursor);
        result.vertexEdges.resize(result.edges.size()*2);
        const size_t boundaryCount=std::accumulate(boundaryDegrees.begin(),boundaryDegrees.end(),size_t{});
        checked32(boundaryCount); result.boundaryEdges.resize(boundaryCount);
        offset=0; size_t boundaryOffset=0;
        for(size_t v=0;v<result.vertices.size();++v) {
            result.vertices[v].edges={construction32<Proved>(offset),construction32<Proved>(degrees[v])};
            result.vertices[v].boundaryEdges={construction32<Proved>(boundaryOffset),construction32<Proved>(boundaryDegrees[v])};
            offset+=degrees[v]; boundaryOffset+=boundaryDegrees[v];
        }
        std::fill(degrees.begin(),degrees.end(),Id{}); std::fill(boundaryDegrees.begin(),boundaryDegrees.end(),Id{});
        for(size_t e=0;e<result.edges.size();++e) {
            canceled(stop);
            for(const auto v:result.edges[e].vertices) {
                result.vertexEdges[result.vertices[v].edges.offset+degrees[v]++]=static_cast<Id>(e);
                if(result.edges[e].incidentFaces.count==1) { result.boundaryEdges[result.vertices[v].boundaryEdges.offset+boundaryDegrees[v]++]=static_cast<Id>(e); }
            }
        }
    }
    {
        std::vector<bool> seenFaces(result.faces.size()),seenEdges(result.edges.size());
        for(size_t seed=0;seed<result.faces.size();++seed) {
            canceled(stop); if(seenFaces[seed]) {continue;}
            std::vector<Id> component{static_cast<Id>(seed)}; seenFaces[seed]=true;
            for(size_t head=0;head<component.size();++head) {
                canceled(stop); auto& face=result.faces[component[head]]; face.component=static_cast<Id>(result.components.size());
                for(const auto edge:face.edges) {
                    if(seenEdges[edge]) {continue;} seenEdges[edge]=true;
                    for(const auto& use:view(result.edgeUses,result.edges[edge].incidentFaces)) {
                        canceled(stop); if(!seenFaces[use.face]) {seenFaces[use.face]=true;component.push_back(use.face);}
                    }
                }
            }
            result.components.push_back(std::move(component));
        }
    }
    if(std::none_of(result.edges.begin(),result.edges.end(),[](const auto& edge){return edge.windingConflict;})) {return result;}
    std::vector<int> flips(result.faces.size(),-1);
    for(size_t seed=0;seed<result.faces.size();++seed) {
        canceled(stop); if(flips[seed]!=-1) {continue;}
        std::vector<Id> queue{static_cast<Id>(seed)}; flips[seed]=0;
        for(size_t head=0;head<queue.size();++head) {
            canceled(stop); const auto face=queue[head];
            for(const auto index:result.faces[face].edges) {
                auto& edge=result.edges[index]; const auto uses=view(result.edgeUses,edge.incidentFaces);
                if(uses.size()!=2) {continue;}
                const auto other=uses[0].face==face?uses[1].face:uses[0].face;
                const int expected=flips[face]^static_cast<int>(edge.windingConflict);
                if(flips[other]==-1) {flips[other]=expected;queue.push_back(other);}
                else if(flips[other]!=expected) {edge.orientationContradiction=true;}
            }
        }
    }
    return result;
}
template<class A,class B> bool equalRange(A a,B b) { return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),b.end()); }
template<class Id> void verify(const SourceTopology& original,const Graph<Id>& candidate) {
    require(original.geometry==candidate.geometry,"Geometry owner changed");
    require(original.vertices.size()==candidate.vertices.size()&&original.faces.size()==candidate.faces.size()
        &&original.edges.size()==candidate.edges.size()&&original.components.size()==candidate.components.size(),"Graph cardinality changed");
    for(size_t v=0;v<original.vertices.size();++v) {
        const auto& a=original.vertices[v]; const auto& b=candidate.vertices[v];
        const auto refs=view(candidate.geometry->pointReferences,b.references);
        require(refs.size()==a.references.size(),"Reference count changed");
        for(size_t r=0;r<refs.size();++r) {require(refs[r].partId==a.references[r].partId&&refs[r].pointId==a.references[r].pointId,"Reference changed");}
        require(equalRange(a.faces,view(candidate.vertexFaces,b.faces))&&equalRange(a.edges,view(candidate.vertexEdges,b.edges))
            &&equalRange(a.boundaryEdges,view(candidate.boundaryEdges,b.boundaryEdges)),"Vertex adjacency changed");
    }
    for(size_t f=0;f<original.faces.size();++f) {require(equalRange(original.faces[f].edges,candidate.faces[f].edges)
        &&original.faces[f].component==candidate.faces[f].component,"Face incidence changed");}
    for(size_t e=0;e<original.edges.size();++e) {
        const auto& a=original.edges[e];const auto& b=candidate.edges[e];const auto uses=view(candidate.edgeUses,b.incidentFaces);
        require(equalRange(a.vertices,b.vertices)&&a.windingConflict==b.windingConflict&&a.orientationContradiction==b.orientationContradiction
            &&a.incidentFaces.size()==uses.size(),"Edge changed");
        for(size_t u=0;u<uses.size();++u) {require(a.incidentFaces[u].face==uses[u].face&&a.incidentFaces[u].forward==uses[u].forward,"Edge use changed");}
    }
    for(size_t c=0;c<original.components.size();++c) {require(equalRange(original.components[c],candidate.components[c]),"Component order changed");}
}
template<class T> size_t bytes(const std::vector<T>& values) {return values.capacity()*sizeof(T);}
size_t payload(const SourceTopology& s) {
    size_t total=bytes(s.vertices)+bytes(s.faces)+bytes(s.edges)+bytes(s.components);
    for(const auto& c:s.components) {total+=bytes(c);}
    const auto& i=*s.incidence;return total+bytes(i.vertexFaces)+bytes(i.vertexEdges)+bytes(i.boundaryEdges)+bytes(i.edgeUses);
}
template<class Id> size_t payload(const Graph<Id>& s) {
    size_t total=bytes(s.vertices)+bytes(s.faces)+bytes(s.edges)+bytes(s.components)+bytes(s.vertexFaces)+bytes(s.vertexEdges)+bytes(s.boundaryEdges)+bytes(s.edgeUses);
    for(const auto& c:s.components) {total+=bytes(c);} return total;
}
uint64_t scan(const SourceTopology& s) {
    uint64_t sum=0;
    for(const auto& v:s.vertices) {for(const auto f:v.faces){sum+=f;} for(const auto e:v.edges){sum+=e;} for(const auto e:v.boundaryEdges){sum+=e;}}
    for(const auto& f:s.faces) {sum+=f.component;for(const auto e:f.edges){sum+=e;}}
    for(const auto& e:s.edges) {sum+=e.vertices[0]+e.vertices[1]+e.windingConflict+e.orientationContradiction;for(const auto u:e.incidentFaces){sum+=u.face+u.forward;}}
    return sum;
}
template<class Id> uint64_t scan(const Graph<Id>& s) {
    uint64_t sum=0;
    for(const auto& v:s.vertices) {for(const auto f:view(s.vertexFaces,v.faces)){sum+=f;} for(const auto e:view(s.vertexEdges,v.edges)){sum+=e;} for(const auto e:view(s.boundaryEdges,v.boundaryEdges)){sum+=e;}}
    for(const auto& f:s.faces) {sum+=f.component;for(const auto e:f.edges){sum+=e;}}
    for(const auto& e:s.edges) {sum+=uint64_t(e.vertices[0])+e.vertices[1]+e.windingConflict+e.orientationContradiction;for(const auto u:view(s.edgeUses,e.incidentFaces)){sum+=uint64_t(u.face)+u.forward;}}
    return sum;
}
Json statistics(allocation_probe::Start start,double milliseconds) {
    const auto live=allocation_probe::live.load()-start.liveBytes,peak=allocation_probe::peak.load()-start.liveBytes;
    const auto total=allocation_probe::total.load()-start.totalBytes,calls=allocation_probe::calls.load()-start.callCount;
    return Json{{"ms",milliseconds},{"live_requested_bytes",live},{"peak_requested_bytes",peak},{"total_requested_bytes",total},{"allocation_calls",calls}};
}
template<class Function> auto measured(Json& result,const char* key,Function work) {
    const auto counters=allocation_probe::start(); const auto start=std::chrono::steady_clock::now();auto value=work();
    const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    auto data=statistics(counters,ms);result[key]=std::move(data);return value;
}
std::vector<DuplicateSource> sources(const Mesh& mesh) {
    DuplicateSource source;source.fileId=7;source.batchId=3;source.name="probe";source.data=mesh.sourceData;
    for(size_t i=0;i<meshGeometry(mesh).nodes.size();++i) {const auto& n=meshGeometry(mesh).nodes[i];source.parts.push_back({i+1,n.indexOffset,n.indexCount,source.unusedPointTransform});}
    return {std::move(source)};
}
void selfTest() {
    require(countsFit(max32,max32/3,max32/2,max32,max32),"Maximum valid counts rejected");
    require(!countsFit(max32+1,0,0,0,0)&&!countsFit(0,max32/3+1,0,0,0)&&!countsFit(0,0,max32/2+1,0,0)
        &&!countsFit(0,0,0,max32+1,0)&&!countsFit(0,0,0,0,max32+1),"32 bit overflow accepted");
    bool rejected=false;try{checked32(max32+1);}catch(const std::overflow_error&){rejected=true;}require(rejected,"Narrow cast overflow accepted");
    const char* text="v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 -1 0\nv 0 0 1\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 10 0 0\nv 11 0 0\nv 10 1 0\nf 1 2 3\nf 1 2 4\nf 1 2 5\nf 6 7 8\nf 9 10 11\nf 1 1 2\n";
    auto input=sources(loadObjMeshText(text));auto repeated=input[0];repeated.parts[0].partId=99;repeated.parts[0].transform[12]=20;input.push_back(repeated);
    auto unavailable=input[0];unavailable.fileId=10;unavailable.data.reset();input.push_back(unavailable);
    for(const auto mode:{TopologyMode::originalIndex,TopologyMode::exactPosition}) {
        const auto geometry=buildAnalysisGeometry(input,mode);const auto wide=buildTopologyIncidence(geometry);
        for(size_t i=0;i<geometry->sources.size();++i) {
            const auto small=build<uint32_t>(geometry->sources[i]);const auto slices=build<size_t>(geometry->sources[i]);
            const auto proved=build<uint32_t,true>(geometry->sources[i]);
            verify(wide.sources[i],small);verify(wide.sources[i],slices);verify(wide.sources[i],proved);
            require(scan(wide.sources[i])==scan(small)&&scan(small)==scan(slices)&&scan(small)==scan(proved),"Traversal changed");
            require(payload(small)==payload(proved),"Proved casts changed graph capacity");
            auto relocated=small;verify(wide.sources[i],relocated);
            auto relocatedProved=proved;verify(wide.sources[i],relocatedProved);
        }
        std::stop_source stop;stop.request_stop();bool canceledResult=false;
        try{(void)build<uint32_t>(geometry->sources[0],stop.get_token());}catch(const std::runtime_error&){canceledResult=true;}require(canceledResult,"Cancellation failed");
        canceledResult=false;try{(void)build<uint32_t,true>(geometry->sources[0],stop.get_token());}catch(const std::runtime_error&){canceledResult=true;}require(canceledResult,"Proved cancellation failed");
    }
    auto mobiusData=std::make_shared<SourceMeshData>();
    for(uint32_t i=0;i<5;++i){const double angle=i*2*std::numbers::pi/5;
        for(const double offset:{-.2,.2}){editSourcePositions(*mobiusData).push_back({(1+offset*std::cos(angle/2))*std::cos(angle),(1+offset*std::cos(angle/2))*std::sin(angle),offset*std::sin(angle/2)});}
        const uint32_t a=i*2,b=a+1,c=i==4?1:a+2,d=i==4?0:a+3;mobiusData->indices.insert(mobiusData->indices.end(),{a,c,d,a,d,b});}
    publishSourcePositions(*mobiusData);DuplicateSource mobius;mobius.fileId=91;mobius.data=mobiusData;mobius.parts.push_back({101,0,mobiusData->indices.size(),mobius.unusedPointTransform});
    for(const auto mode:{TopologyMode::originalIndex,TopologyMode::exactPosition}){const auto geometry=buildAnalysisGeometry({mobius},mode);const auto wide=buildTopologyIncidence(geometry);
        require(std::any_of(wide.sources[0].edges.begin(),wide.sources[0].edges.end(),[](const auto& edge){return edge.orientationContradiction;}),"Mobius fixture lost contradiction");
        const auto checked=build<uint32_t>(geometry->sources[0]);const auto proved=build<uint32_t,true>(geometry->sources[0]);
        verify(wide.sources[0],checked);verify(wide.sources[0],proved);require(payload(checked)==payload(proved),"Mobius proved capacity changed");verify(wide.sources[0],build<size_t>(geometry->sources[0]));}
}
}
int main(int argc,char** argv) {
    try {
        selfTest();if(argc==2&&std::string_view(argv[1])=="--self-test"){return 0;}
        require(argc==3||(argc==4&&std::string_view(argv[3])=="--proved"),"Usage: woby_simplification_16_probe model.obj samples [--proved]");
        const bool provedOnly=argc==4;const int repeats=std::stoi(argv[2]);require(repeats>0&&repeats<=9,"Invalid repeats");
        const auto mesh=loadObjMesh(argv[1]);const auto input=sources(mesh);
        Json output{{"model",argv[1]},{"render_faces",meshGeometry(mesh).indices.size()/3},{"backends",provedOnly?"wide_vs_compact_proved":"all_four"},
            {"sizes",{{"wide_vertex",sizeof(TopologyVertex)},{"wide_face",sizeof(TopologyFace)},{"wide_edge",sizeof(TopologyEdge)},{"wide_use",sizeof(TopologyEdgeUse)},
                {"slices_vertex",sizeof(VertexRecord<size_t>)},{"slices_face",sizeof(FaceRecord<size_t>)},{"slices_edge",sizeof(EdgeRecord<size_t>)},{"slices_use",sizeof(EdgeUse<size_t>)},
                {"compact_vertex",sizeof(VertexRecord<uint32_t>)},{"compact_face",sizeof(FaceRecord<uint32_t>)},{"compact_edge",sizeof(EdgeRecord<uint32_t>)},{"compact_use",sizeof(EdgeUse<uint32_t>)}}},
            {"notes","Release, requested C++ allocation sizes with global new header and atomic counters. Source mesh and shared analysis geometry loaded before graph timings. All arrays, adjacency order, components and flags verified. Findings, serialization, GPU and UI are outside this prototype."}};
        for(const auto mode:{TopologyMode::originalIndex,TopologyMode::exactPosition}) {
            const auto geometry=buildAnalysisGeometry(input,mode);size_t geometryBytes=0;
            for(const auto& g:geometry->sources){geometryBytes+=bytes(g->vertices)+bytes(g->faces)+bytes(g->pointReferences)+bytes(g->referenceOffsets);}
            Json runs=Json::array();
            for(int run=0;run<repeats;++run) {
                std::cerr<<topologyModeName(mode)<<" run "<<run<<'\n';Json record;MeshTopology wide;std::vector<Graph<size_t>> slices;std::vector<Graph<uint32_t>> compact,proved;
                const auto wideWork=[&]{return buildTopologyIncidence(geometry);};
                const auto slicesWork=[&]{std::vector<Graph<size_t>> graphs;graphs.reserve(geometry->sources.size());for(const auto& g:geometry->sources){graphs.push_back(build<size_t>(g));}return graphs;};
                const auto compactWork=[&]{std::vector<Graph<uint32_t>> graphs;graphs.reserve(geometry->sources.size());for(const auto& g:geometry->sources){graphs.push_back(build<uint32_t>(g));}return graphs;};
                const auto provedWork=[&]{std::vector<Graph<uint32_t>> graphs;graphs.reserve(geometry->sources.size());for(const auto& g:geometry->sources){graphs.push_back(build<uint32_t,true>(g));}return graphs;};
                if(run%2==0){wide=measured(record,"wide_build",wideWork);if(!provedOnly){slices=measured(record,"slices_build",slicesWork);compact=measured(record,"compact_build",compactWork);}proved=measured(record,"compact_proved_build",provedWork);}
                else{proved=measured(record,"compact_proved_build",provedWork);if(!provedOnly){compact=measured(record,"compact_build",compactWork);slices=measured(record,"slices_build",slicesWork);}wide=measured(record,"wide_build",wideWork);}
                size_t wideBytes=0,sliceBytes=0,compactBytes=0,provedBytes=0,v=0,f=0,e=0,c=0,uses=0;uint64_t expected=0;
                size_t nonManifold=0,winding=0,contradictions=0,maxIncidence=0,multiReference=0,pointReferences=0,duplicateFaceRefs=0,duplicateVertexSets=0;
                for(size_t i=0;i<wide.sources.size();++i){
                    const auto& source=wide.sources[i];verify(source,proved[i]);
                    wideBytes+=payload(source);provedBytes+=payload(proved[i]);
                    if(!provedOnly){verify(source,slices[i]);verify(source,compact[i]);sliceBytes+=payload(slices[i]);compactBytes+=payload(compact[i]);require(payload(compact[i])==payload(proved[i]),"Proved casts changed graph capacity");}
                    v+=source.vertices.size();f+=source.faces.size();e+=source.edges.size();c+=source.components.size();uses+=source.incidence->edgeUses.size();expected+=scan(source);
                    for(const auto& edge:source.edges){nonManifold+=edge.incidentFaces.size()>2;winding+=edge.windingConflict;contradictions+=edge.orientationContradiction;maxIncidence=std::max(maxIncidence,edge.incidentFaces.size());}
                    for(const auto& vertex:source.vertices){multiReference+=vertex.references.size()>1;pointReferences+=vertex.references.size();}
                    std::set<std::tuple<uint64_t,uint64_t,size_t>> identities;std::vector<std::array<size_t,3>> faceVertices;faceVertices.reserve(source.geometry->faces.size());
                    for(const auto& face:source.geometry->faces){duplicateFaceRefs+=!identities.insert({face.reference.fileId,face.reference.partId,face.reference.triangleId}).second;auto key=face.vertices;std::sort(key.begin(),key.end());faceVertices.push_back(key);}
                    std::sort(faceVertices.begin(),faceVertices.end());for(size_t j=1;j<faceVertices.size();++j){duplicateVertexSets+=faceVertices[j]==faceVertices[j-1];}
                }
                const auto timeScan=[&](const char* key,const auto& range){uint64_t sum=0;auto count=measured(record,key,[&]{for(int pass=0;pass<10;++pass){for(const auto& s:range){sum+=scan(s);}}return sum;});require(count==expected*10,"Timed traversal changed");};
                if(run%2==0){timeScan("wide_scan_10",wide.sources);if(!provedOnly){timeScan("slices_scan_10",slices);timeScan("compact_scan_10",compact);}timeScan("compact_proved_scan_10",proved);}
                else{timeScan("compact_proved_scan_10",proved);if(!provedOnly){timeScan("compact_scan_10",compact);timeScan("slices_scan_10",slices);}timeScan("wide_scan_10",wide.sources);}
                record["counts"]={{"vertices",v},{"faces",f},{"edges",e},{"components",c},{"edge_uses",uses},{"non_manifold_edges",nonManifold},
                    {"winding_conflicts",winding},{"orientation_contradictions",contradictions},{"max_edge_incidence",maxIncidence},{"multi_reference_vertices",multiReference},
                    {"point_references",pointReferences},{"duplicate_face_references",duplicateFaceRefs},{"duplicate_face_vertex_sets",duplicateVertexSets}};
                record["array_capacity_bytes"]={{"geometry",geometryBytes},{"wide_graph",wideBytes},{"compact_proved_graph",provedBytes}};
                if(!provedOnly){record["array_capacity_bytes"]["slices_graph"]=sliceBytes;record["array_capacity_bytes"]["compact_graph"]=compactBytes;}
                record["verified"]=true;runs.push_back(std::move(record));
            }
            output[topologyModeName(mode)]=std::move(runs);
        }
        std::cout<<output.dump(2)<<'\n';
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
