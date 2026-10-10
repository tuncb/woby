#include "obj_mesh.h"
#include "mesh_comparison.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace woby;
using Json = nlohmann::json;
void require(bool condition,const char* message) { if (!condition) { throw std::runtime_error(message); } }
Mesh input(Mesh mesh) {
    auto provenance=std::make_shared<DuplicateInput>();
    DuplicateSource source; source.fileId=7; source.batchId=3; source.name="probe"; source.data=mesh.sourceData;
    for (size_t i=0;i<meshGeometry(mesh).nodes.size();++i) {
        const auto& node=meshGeometry(mesh).nodes[i];
        source.parts.push_back({i+1,node.indexOffset,node.indexCount,source.unusedPointTransform});
    }
    provenance->sources.push_back(std::move(source)); mesh.duplicateInput=std::move(provenance);
    publishMeshGeometry(mesh); return mesh;
}
Json signature(const MeshIntersections& result) {
    Json findings=Json::array();
    for (const auto& f:result.findings) {
        findings.push_back({{"faces",{{f.faces[0].fileId,f.faces[0].partId,f.faces[0].triangleId},
            {f.faces[1].fileId,f.faces[1].partId,f.faces[1].triangleId}}},{"geometry",f.geometry},
            {"batch",f.batchId},{"source",f.source},{"provenance",int(f.provenance)},{"mode",int(f.mode)}});
    }
    return Json{{"mode",int(result.mode)},{"phase",int(result.phase)},{"has_result",result.hasResult},
        {"available",result.availableSources},{"unavailable",result.unavailableSources},{"collapsed",result.excludedCollapsedFaces},
        {"candidates",result.candidateTests},{"affected",result.affectedFaces},{"truncated",result.truncated},
        {"reason",result.truncationReason},{"limits",{result.limits.pairs,result.limits.candidateTests}},{"findings",findings}};
}
template<class T> size_t bytes(const std::vector<T>& values) { return values.capacity()*sizeof(T); }
Json payload(const MeshTopology& topology) {
    size_t vertices=0,faces=0,edges=0,incidence=0,components=0,vCount=0,fCount=0;
    for (const auto& s:topology.sources) {
        vertices+=bytes(s.vertices)+bytes(s.geometry->vertices); faces+=bytes(s.faces)+bytes(s.geometry->faces);
        incidence+=bytes(s.geometry->pointReferences)+bytes(s.geometry->referenceOffsets); edges+=bytes(s.edges);
        vCount+=s.vertices.size(); fCount+=s.faces.size();
        components+=bytes(s.components); for (const auto& c:s.components) { components+=bytes(c); }
        if (s.incidence) { const auto& i=*s.incidence;
            incidence+=bytes(i.vertexFaces)+bytes(i.vertexEdges)+bytes(i.boundaryEdges)+bytes(i.edgeUses);
        }
    }
    size_t findings=bytes(topology.boundaries)+bytes(topology.nonManifoldEdges)+bytes(topology.windingEdges)+bytes(topology.windingFaces)
        +bytes(topology.nonManifoldVertices)+bytes(topology.boundaryRegions)+bytes(topology.holes)+bytes(topology.finPatches)+bytes(topology.fins);
    for (const auto& b:topology.boundaryRegions) { findings+=bytes(b.vertices)+bytes(b.edges); }
    for (const auto& f:topology.finPatches) { findings+=bytes(f.faces)+bytes(f.physicalBoundaryEdges)+bytes(f.cutBoundaryEdges); }
    return Json{{"vertices",vCount},{"faces",fCount},{"vertex_capacity_bytes",vertices},{"face_capacity_bytes",faces},
        {"edge_capacity_bytes",edges},{"incidence_capacity_bytes",incidence},{"component_capacity_bytes",components},
        {"finding_capacity_bytes",findings},{"total_array_capacity_bytes",vertices+faces+edges+incidence+components+findings},
        {"proposed_geometry_payload_bytes",vCount*sizeof(std::array<double,3>)+fCount*(sizeof(TopologyFaceReference)+sizeof(std::array<size_t,3>))},
        {"sizeof_vertex",sizeof(TopologyVertex)},{"sizeof_face",sizeof(TopologyFace)},{"sizeof_edge",sizeof(TopologyEdge)}};
}
size_t overlayBytes(const SurfaceComparison& s) {
    return bytes(s.topologyBoundaries)+bytes(s.topologyNonManifold)+bytes(s.topologyWinding)+bytes(s.nonManifoldVertexBounds)
        +bytes(s.nonManifoldVertexMarkers)+bytes(s.holeBounds)+bytes(s.holeEdges)+bytes(s.finBounds)+bytes(s.finEdges)+bytes(s.finFill);
}
template<class Work> auto timed(Json& sample,const char* name,Work work) {
    const auto start=std::chrono::steady_clock::now(); auto result=work();
    sample[name]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); return result;
}
void selfTest() {
    auto mesh=input(loadObjMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\nf 4 5 6\nf 1 1 2\n"));
    auto provenance=std::make_shared<DuplicateInput>(*mesh.duplicateInput);
    auto unavailable=provenance->sources[0]; unavailable.fileId=8; unavailable.data.reset(); provenance->sources.push_back(unavailable);
    auto shifted=provenance->sources[0]; shifted.batchId=9; shifted.parts[0].transform[12]=10; provenance->sources.push_back(shifted);
    mesh.duplicateInput=std::move(provenance);
    for (auto mode:{TopologyMode::originalIndex,TopologyMode::exactPosition}) {
        const auto full=buildMeshTopology(mesh.duplicateInput->sources,mode); const auto slim=buildAnalysisGeometry(mesh.duplicateInput->sources,mode);
        require(full.excludedCollapsedFaces==2 && full.unavailableSources==1,"Fixture selection failed");
        for (auto limits:{IntersectionLimits{0,0},IntersectionLimits{1,0},IntersectionLimits{0,1}}) {
            const auto a=inspectIntersections(full,{}, {},limits),b=inspectIntersections(*slim,{}, {},limits);
            require(signature(a)==signature(b),"Geometry-only findings changed");
        }
        std::stop_source stop; stop.request_stop(); bool canceled=false;
        try { (void)inspectIntersections(*slim,{},stop.get_token()); } catch (const std::runtime_error&) { canceled=true; }
        require(canceled,"Cancellation was lost");
    }
}
}
int main(int argc,char** argv) {
    try {
        selfTest();
        if (argc==2 && std::string_view(argv[1])=="--self-test") { return 0; }
        require(argc==3,"Usage: woby_analysis_stages_probe model.obj samples");
        const int count=std::stoi(argv[2]); require(count>0 && count<=5,"Invalid sample count");
        const auto mesh=input(loadObjMesh(argv[1]));
        Json output{{"model",argv[1]},{"render_faces",meshGeometry(mesh).indices.size()/3},
            {"notes","Release timings after load. Full topology remains resident during the repeated intersection-stage run. Cold geometry and cold/warm production stages are measured separately. Geometry includes point provenance for later topology upgrades. Array capacities exclude allocator overhead, source mesh and scratch/BVH memory."}};
        for (auto mode:{TopologyMode::originalIndex,TopologyMode::exactPosition}) {
            Json samples=Json::array();
            for (int run=0;run<count;++run) {
                std::cerr<<topologyModeName(mode)<<" sample "<<run<<'\n';
                Json sample;
                const auto full=timed(sample,"build_full_topology_ms",[&]{return buildMeshTopology(mesh.duplicateInput->sources,mode);});
                auto slim=timed(sample,"build_geometry_ms",[&]{return buildAnalysisGeometry(mesh.duplicateInput->sources,mode);});
                const auto expected=signature(timed(sample,"inspect_existing_topology_ms",[&]{return inspectIntersections(full);}));
                const auto projected=signature(timed(sample,"inspect_geometry_only_ms",[&]{return inspectIntersections(*slim);}));
                require(expected==projected,"Cold geometry changed results");
                size_t geometryBytes=0;
                for (const auto& g:slim->sources) { geometryBytes+=bytes(g->vertices)+bytes(g->faces)+bytes(g->pointReferences)+bytes(g->referenceOffsets); }
                sample["geometry_array_capacity_bytes"]=geometryBytes; slim={};
                const auto job=timed(sample,"current_intersections_stage_ms",[&]{return computeComparisonStages(mesh,{},comparisonIntersections,{}, {},mode);});
                require(expected==signature(job.original.intersections),"Stage changed results");
                const auto cached=comparisonAnalysisInputs(job);
                const auto warm=timed(sample,"warm_intersections_stage_ms",[&]{return computeComparisonStages(mesh,{},comparisonIntersections,{}, {},mode,{},cached);});
                require(expected==signature(warm.original.intersections),"Warm stage changed results");
                require(warm.original.analysis.geometry==job.original.analysis.geometry,"Warm geometry was rebuilt");
                auto upgraded=timed(sample,"upgrade_topology_stage_ms",[&]{return computeComparisonStages(mesh,{},comparisonTopology,{}, {},mode,{},cached);});
                require(upgraded.original.topology.geometry==job.original.analysis.geometry,"Upgrade copied geometry");
                const auto afterTopology=timed(sample,"intersections_after_topology_ms",[&]{return computeComparisonStages(mesh,{},comparisonIntersections,{}, {},mode,{},comparisonAnalysisInputs(upgraded));});
                require(expected==signature(afterTopology.original.intersections),"Topology reuse changed results");
                const TopologyDisplayInputs displays{upgraded.original.topologyDisplayInputs,upgraded.repaired.topologyDisplayInputs};
                TopologyInspectionSettings filters; filters.holeSizeRatioTolerance=1; filters.finMaxAreaRatio=.5f;
                const auto display=timed(sample,"threshold_display_worker_ms",[&]{return prepareComparisonTopologyDisplay(displays,filters);});
                sample["threshold_display_array_capacity_bytes"]=overlayBytes(display.original);
                size_t snapshotBytes=0;
                for (const auto& snapshot:displays[0]) {
                    if (!snapshot) { continue; }
                    snapshotBytes+=bytes(snapshot->boundaryRegions)+bytes(snapshot->finPatches);
                    for (const auto& r:snapshot->boundaryRegions) { snapshotBytes+=bytes(r.vertices)+bytes(r.edges); }
                    for (const auto& f:snapshot->finPatches) { snapshotBytes+=bytes(f.faces)+bytes(f.physicalBoundaryEdges)+bytes(f.cutBoundaryEdges); }
                }
                sample["display_snapshot_array_capacity_bytes"]=snapshotBytes;
                sample["topology"]=payload(full);
                sample["unrequested_topology_overlay_capacity_bytes"]=overlayBytes(job.original);
                sample["candidate_tests"]=job.original.intersections.candidateTests;
                sample["finding_count"]=job.original.intersections.findings.size();
                sample["truncated"]=job.original.intersections.truncated;
                sample["truncation_reason"]=job.original.intersections.truncationReason;
                samples.push_back(std::move(sample));
            }
            output[topologyModeName(mode)]=samples;
        }
        std::cout<<output.dump(2)<<'\n';
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
