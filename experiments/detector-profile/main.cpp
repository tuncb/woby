#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "difference_mesh.h"
#include "mesh_comparison.h"
#include "obj_mesh.h"
#include "parallel_work.h"
#include "profile.h"
#ifdef DETECTOR_PROFILE_QUERY_COUNTERS
#include "query_counts.h"
#endif
#include <manifold/manifold.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <string>

namespace {
using namespace woby;
using Json = nlohmann::json;
struct Options {
    std::string model, output, operations = "auto,quality,intersections,distance,difference";
    size_t grid = 0, sphere = 0, faces = 0, runs = 3, candidateLimit = 1000000, pairLimit = 10000, queryStride = 1, sourceBatches = 1;
    unsigned helpers = analysisWorkerLimit();
    double shift = .001, bump = 0;
    int64_t cancelAfterMs = -1;
    bool exact = false, expanded = false, shuffle = false, emptyToken = false, highQos = false;
};
void require(bool value, const char* message) { if (!value) { throw std::invalid_argument(message); } }
Options options(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--exact") { o.exact = true; continue; }
        if (key == "--expanded") { o.expanded = true; continue; }
        if (key == "--shuffle") { o.shuffle = true; continue; }
        if (key == "--empty-token") { o.emptyToken = true; continue; }
        if (key == "--high-qos") { o.highQos = true; continue; }
        require(i + 1 < argc, "Option requires a value.");
        const std::string value = argv[++i];
        if (key == "--model") { o.model = value; }
        else if (key == "--output") { o.output = value; }
        else if (key == "--operations") { o.operations = value; }
        else if (key == "--grid") { o.grid = std::stoull(value); }
        else if (key == "--sphere") { o.sphere = std::stoull(value); }
        else if (key == "--faces") { o.faces = std::stoull(value); }
        else if (key == "--runs") { o.runs = std::stoull(value); }
        else if (key == "--helpers") { o.helpers = static_cast<unsigned>(std::stoul(value)); }
        else if (key == "--candidate-limit") { o.candidateLimit = std::stoull(value); }
        else if (key == "--pair-limit") { o.pairLimit = std::stoull(value); }
        else if (key == "--query-stride") { o.queryStride = std::stoull(value); }
        else if (key == "--source-batches") { o.sourceBatches = std::stoull(value); }
        else if (key == "--shift") { o.shift = std::stod(value); }
        else if (key == "--bump") { o.bump = std::stod(value); }
        else if (key == "--cancel-after-ms") { o.cancelAfterMs = std::stoll(value); }
        else { throw std::invalid_argument("Unknown option: " + key); }
    }
    require((!o.model.empty()) + (o.grid > 0) + (o.sphere > 0) == 1, "Select one model, grid, or sphere.");
    require(o.runs > 0 && o.runs <= 100 && o.grid <= 4000 && o.sphere <= 4096, "Invalid workload size.");
    require(o.helpers <= analysisWorkerLimit(), "Helper budget exceeds the production limit.");
    require(std::isfinite(o.shift), "Shift must be finite.");
    require(std::isfinite(o.bump) && o.bump >= 0,"Bump must be finite and nonnegative.");
    require(o.queryStride > 0,"Query stride must be positive.");
    require(o.sourceBatches > 0,"Source batch count must be positive.");
    require(o.cancelAfterMs >= -1 && o.cancelAfterMs <= 600000,"Invalid cancellation delay.");
    require(o.cancelAfterMs < 0 || !o.emptyToken,"Cancellation requires a live token.");
    require(o.cancelAfterMs < 0 || (o.operations.find("warm") == std::string::npos && o.operations.find("display") == std::string::npos),
        "Cancellation controls do not support cache preparation.");
#ifndef DETECTOR_PROFILE_QUERY_COUNTERS
    require(o.queryStride == 1,"Subsampled queries require the query-counter executable.");
#endif
    if (!o.model.empty()) { require(std::filesystem::path(o.model).is_absolute(), "Model path must be absolute."); }
    if (!o.output.empty()) { require(std::filesystem::path(o.output).is_absolute(), "Output path must be absolute."); }
    return o;
}
void attach(Mesh& mesh) {
    if (!mesh.sourceData) { captureSourceMesh(mesh, SourceProvenance::importerVertices); }
    auto input = std::make_shared<DuplicateInput>();
    DuplicateSource source; source.fileId = 1; source.batchId = 1; source.name = "profile";
    source.data = mesh.sourceData; source.wholeFile = true;
    for (size_t i = 0; i < meshGeometry(mesh).nodes.size(); ++i) {
        const auto& n = meshGeometry(mesh).nodes[i];
        source.parts.push_back({i + 1, n.indexOffset, n.indexCount, source.unusedPointTransform});
    }
    input->sources.push_back(std::move(source)); mesh.duplicateInput = std::move(input);
    publishMeshGeometry(mesh);
}
void partitionSources(Mesh& mesh, size_t count) {
    if (count == 1 || !mesh.duplicateInput || mesh.duplicateInput->sources.empty()) { return; }
    const auto& original = mesh.duplicateInput->sources.front();
    count = std::min(count,original.parts.size());
    if (count < 2) { return; }
    auto input = std::make_shared<DuplicateInput>(); input->settings = mesh.duplicateInput->settings;
    for (size_t batch = 0; batch < count; ++batch) {
        DuplicateSource source = original; source.batchId = batch+1; source.wholeFile = false;
        source.name = "profile batch " + std::to_string(batch+1);
        source.parts.assign(original.parts.begin()+static_cast<std::ptrdiff_t>(batch*original.parts.size()/count),
            original.parts.begin()+static_cast<std::ptrdiff_t>((batch+1)*original.parts.size()/count));
        input->sources.push_back(std::move(source));
    }
    mesh.duplicateInput = std::move(input);
}
Mesh gridMesh(size_t width) {
    Mesh mesh; auto& g = editMeshGeometry(mesh);
    for (size_t y = 0; y <= width; ++y) {
        for (size_t x = 0; x <= width; ++x) {
            Coordinate p{static_cast<double>(x), static_cast<double>(y), 0};
            Vertex v; v.position = renderPosition(p); g.vertices.push_back(v); g.precisePositions.push_back(p);
        }
    }
    for (size_t y = 0; y < width; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const auto a = static_cast<uint32_t>(y * (width + 1) + x), b = a + 1;
            const auto c = a + static_cast<uint32_t>(width + 1), d = c + 1;
            g.indices.insert(g.indices.end(), {a,b,d,a,d,c});
        }
    }
    g.nodes.push_back({"grid",0,static_cast<uint32_t>(g.indices.size())});
    mesh.bounds = calculateBounds(g.vertices); attach(mesh); return mesh;
}
Mesh sphereMesh(size_t segments) {
    const auto solid = manifold::Manifold::Sphere(1.0, static_cast<int>(segments));
    const auto data = solid.GetMeshGL64();
    Mesh mesh; auto& g = editMeshGeometry(mesh);
    for (size_t i = 0; i < data.vertProperties.size(); i += data.numProp) {
        Coordinate p{data.vertProperties[i], data.vertProperties[i+1], data.vertProperties[i+2]};
        Vertex v; v.position = renderPosition(p); g.vertices.push_back(v); g.precisePositions.push_back(p);
    }
    for (const auto index : data.triVerts) { g.indices.push_back(static_cast<uint32_t>(index)); }
    g.nodes.push_back({"sphere",0,static_cast<uint32_t>(g.indices.size())});
    mesh.bounds = calculateBounds(g.vertices); attach(mesh); return mesh;
}
Mesh prefix(const Mesh& input, size_t count) {
    Mesh result; result.origin = input.origin; auto& g = editMeshGeometry(result);
    std::vector<uint32_t> map(meshGeometry(input).vertices.size(), UINT32_MAX);
    for (size_t corner = 0; corner < std::min(count,meshGeometry(input).indices.size()/3)*3; ++corner) {
        const auto old = meshGeometry(input).indices[corner];
        if (map[old] == UINT32_MAX) {
            map[old] = static_cast<uint32_t>(g.vertices.size());
            const auto p = meshPosition(input,old);
            Vertex v; v.position = renderPosition(p); g.vertices.push_back(v); g.precisePositions.push_back(p);
        }
        g.indices.push_back(map[old]);
    }
    g.nodes.push_back({"prefix",0,static_cast<uint32_t>(g.indices.size())});
    result.bounds = calculateBounds(g.vertices); attach(result); return result;
}
void expand(Mesh& mesh) {
    const auto original = mesh;
    auto& g = editMeshGeometry(mesh); g.vertices.clear(); g.precisePositions.clear();
    g.sourcePositions.reset(); g.positionIndices.clear(); g.positionIndexing = PositionIndexing::identity;
    g.vertices.reserve(g.indices.size()); g.precisePositions.reserve(g.indices.size());
    for (auto& index : g.indices) {
        const auto p = meshPosition(original,index);
        Vertex v; v.position = renderPosition(p); g.precisePositions.push_back(p);
        index = static_cast<uint32_t>(g.vertices.size()); g.vertices.push_back(v);
    }
    publishMeshGeometry(mesh);
}
size_t bumpMesh(const Mesh& original,Mesh& changed,double diagonal,double ratio) {
    if (ratio == 0 || diagonal == 0 || meshGeometry(original).indices.empty()) { return 0; }
    // A compact, smooth Z displacement near the first vertex. This is a repair
    // proxy with unchanged connectivity, not an authentic repaired input.
    const auto center = meshPosition(original,meshGeometry(original).indices.front());
    const double radius = diagonal*.2;
    auto& g = editMeshGeometry(changed);
    g.sourcePositions.reset(); g.positionIndices.clear(); g.positionIndexing = PositionIndexing::identity;
    g.precisePositions.resize(g.vertices.size());
    size_t modified = 0;
    for (size_t i = 0; i < g.vertices.size(); ++i) {
        auto p = meshPosition(original,i);
        const double x = (p[0]-center[0])/radius, y = (p[1]-center[1])/radius;
        const double weight = std::max(0.0,1-x*x-y*y);
        if (weight > 0) { p[2] += ratio*diagonal*weight*weight; ++modified; }
        g.precisePositions[i] = p; g.vertices[i].position = renderPosition(p);
    }
    changed.sourceData.reset(); changed.duplicateInput.reset(); changed.originalBounds.reset();
    changed.bounds = calculateBounds(g.vertices); attach(changed);
    return modified;
}
template<class T> size_t bytes(const std::vector<T>& v) { return v.capacity()*sizeof(T); }
template<class T> void release(T& result, const std::string& operation) {
    const auto label = "release." + operation;
    const auto phase = obj_profile::begin(label.c_str());
    result = {};
    obj_profile::end(phase);
}
Json intersectionResult(const MeshIntersections& value) {
    return {{"candidate_tests",value.candidateTests},{"findings",value.findings.size()},
        {"affected_faces",value.affectedFaces},{"truncated",value.truncated},{"reason",value.truncationReason},
        {"collapsed_excluded",value.excludedCollapsedFaces}};
}
Json summary(const MeshComparison& result) {
    Json counts = Json::array();
    for (const auto& s : result.detectors) { counts.push_back(s.knownCounts); }
    Json surfaces = Json::array();
    for (const auto* s : {&result.original,&result.repaired}) {
        Json qualityStatistics = Json::array();
        for (const auto& q : s->quality.statistics) {
            qualityStatistics.push_back({{"count",q.count},{"minimum",q.minimum},{"p05",q.percentile5},
                {"median",q.median},{"p95",q.percentile95},{"maximum",q.maximum}});
        }
        size_t geometryBytes = 0, incidenceBytes = 0;
        if (s->analysis.geometry) {
            for (const auto& g : s->analysis.geometry->sources) {
                geometryBytes += bytes(g->vertices)+bytes(g->faces)+bytes(g->pointReferences)+bytes(g->referenceOffsets);
            }
        }
        for (const auto& t : s->topology.sources) {
            incidenceBytes += bytes(t.vertices)+bytes(t.faces)+bytes(t.edges)+bytes(t.components);
            for (const auto& component : t.components) { incidenceBytes += bytes(component); }
            if (t.incidence) { const auto& a = *t.incidence; incidenceBytes += bytes(a.vertexFaces)+bytes(a.vertexEdges)+bytes(a.boundaryEdges)+bytes(a.edgeUses); }
        }
        surfaces.push_back({{"distance_samples",s->distances.size()},{"mean",s->mean},{"maximum",s->maximum},{"p95",s->percentile95},
            {"quality_triangles",s->quality.triangles.size()},{"quality_degenerate",s->quality.degenerateTriangles},
            {"quality_statistics",qualityStatistics},
            {"intersection",intersectionResult(s->intersections)},{"analysis_geometry_bytes",geometryBytes},{"incidence_array_bytes",incidenceBytes},
            {"distance_array_bytes",bytes(s->distanceFaces)+bytes(s->distances)+bytes(s->sampleAreas)},
            {"quality_array_bytes",bytes(s->quality.triangles)+bytes(s->quality.sortedSizes)+bytes(s->quality.prefixAreas)+bytes(s->qualityFaces)}});
    }
    return {{"detector_counts",counts},{"surfaces",surfaces}};
}
std::vector<std::string> split(const std::string& value) {
    std::vector<std::string> result; size_t first = 0;
    while (first < value.size()) { const auto end = value.find(',',first); result.push_back(value.substr(first,end-first)); if (end == std::string::npos) { break; } first=end+1; }
    return result;
}
void write(const Options& o, Json result) {
    const auto profile = obj_profile::result(); result["events"] = profile.at("events");
    result["clock_origin"] = obj_profile::clockOrigin();
    if (!o.output.empty()) { std::ofstream file(o.output); file << result.dump(2) << '\n'; if (!file) { throw std::runtime_error("Cannot write probe output."); } }
}
Json processPowerPolicy(bool highQos) {
    PROCESS_POWER_THROTTLING_STATE before{PROCESS_POWER_THROTTLING_CURRENT_VERSION,0,0};
    if (!GetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&before,sizeof(before))) {
        throw std::runtime_error("Cannot read process power policy.");
    }
    if (highQos) {
        PROCESS_POWER_THROTTLING_STATE requested{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            PROCESS_POWER_THROTTLING_EXECUTION_SPEED,0};
        if (!SetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&requested,sizeof(requested))) {
            throw std::runtime_error("Cannot set HighQoS for the probe process.");
        }
    }
    auto after = before;
    if (!GetProcessInformation(GetCurrentProcess(),ProcessPowerThrottling,&after,sizeof(after))) {
        throw std::runtime_error("Cannot verify process power policy.");
    }
    return {{"high_qos_requested",highQos},{"control_mask_before",before.ControlMask},{"state_mask_before",before.StateMask},
        {"control_mask_after",after.ControlMask},{"state_mask_after",after.StateMask}};
}
} // namespace
int main(int argc, char** argv) {
    try {
        const auto o = options(argc,argv);
        Json report{{"schema",1},{"options",{{"model",o.model},{"grid",o.grid},{"sphere",o.sphere},{"faces",o.faces},
            {"runs",o.runs},{"helpers",o.helpers},{"exact",o.exact},{"expanded",o.expanded},{"shuffle",o.shuffle},{"shift_ratio",o.shift},{"bump_ratio",o.bump},
            {"candidate_limit",o.candidateLimit},{"pair_limit",o.pairLimit},{"empty_token",o.emptyToken}}},
            {"cpu",obj_profile::cpuCapabilities()},{"samples",Json::array()}};
        report["options"]["cancel_after_ms"] = o.cancelAfterMs;
        report["options"]["source_batches"] = o.sourceBatches;
        report["process_power_policy"] = processPowerPolicy(o.highQos);
#ifdef DETECTOR_PROFILE_SCOPES
        report["instrumented"] = true;
#else
        report["instrumented"] = false;
#endif
#ifdef DETECTOR_PROFILE_QUERY_COUNTERS
        detector_profile::queryStride = o.queryStride;
        report["query_counters"] = true;
        report["predicate_counters"] = true;
        report["query_batch_stride"] = o.queryStride;
        report["distance_statistics_valid"] = o.queryStride == 1;
#endif
#ifdef DETECTOR_PROFILE_TOKENREF
        report["experimental_change"] = "surface_mesh_quality.cpp canceled helper takes stop_token by const reference";
#endif
#ifdef DETECTOR_PROFILE_MAX3
        report["experimental_change"] = "mesh_comparison.cpp degenerate uses nested two-value max for its three squared edge lengths";
#endif
        auto loadPhase = obj_profile::begin("input.load");
        auto a = o.grid ? gridMesh(o.grid) : o.sphere ? sphereMesh(o.sphere) : loadObjMesh(o.model);
        obj_profile::end(loadPhase);
        auto preparePhase = obj_profile::begin("input.prepare");
        if (o.faces) { a = prefix(a,o.faces); }
        attach(a);
        partitionSources(a,o.sourceBatches);
        if (o.expanded) { expand(a); }
        if (o.shuffle) {
            auto& g = editMeshGeometry(a); std::mt19937 random(42);
            for (const auto& node : g.nodes) {
                const size_t first = node.indexOffset;
                for (size_t i = node.indexCount/3; i > 1; --i) {
                    const size_t j = static_cast<size_t>(random()) % i;
                    for (size_t k = 0; k < 3; ++k) { std::swap(g.indices[first+(i-1)*3+k],g.indices[first+j*3+k]); }
                }
            }
            // Preserve part membership and source order; only render-face order changes.
            publishMeshGeometry(a);
        }
        const auto bounds = originalMeshBounds(a);
        double diagonal = 0;
        for (size_t k = 0; k < 3; ++k) { const double d = bounds[1][k]-bounds[0][k]; diagonal += d*d; }
        const double shift = std::sqrt(diagonal)*o.shift;
        auto b = a; b.origin[2] += shift;
        const auto changedVertices = bumpMesh(a,b,std::sqrt(diagonal),o.bump);
        if (o.bump) { partitionSources(b,o.sourceBatches); }
        obj_profile::end(preparePhase);
        report["mesh"] = {{"triangles",meshGeometry(a).indices.size()/3},{"vertices",meshGeometry(a).vertices.size()},
            {"source_points",sourcePositions(*a.sourceData).size()},{"parts",meshGeometry(a).nodes.size()},{"shift",shift},
            {"source_batches",a.duplicateInput->sources.size()},
            {"bounds",bounds},{"origin",a.origin},{"diagonal",std::sqrt(diagonal)},{"bump_changed_vertices",changedVertices}};
        write(o,report);
        const auto mode = o.exact ? TopologyMode::exactPosition : TopologyMode::originalIndex;
        std::stop_source cancellation; auto stop = o.emptyToken ? std::stop_token{} : cancellation.get_token();
        const IntersectionLimits limits{o.pairLimit,o.candidateLimit};
        const auto reserved = analysisWorkerLimit()-o.helpers;
        analysisWorkerCount.store(reserved);
        const std::map<std::string,uint32_t> masks{{"auto",comparisonDetectors},{"source",comparisonSource},
            {"topology",comparisonTopology},{"duplicate-points",comparisonDuplicatePoints},{"duplicate-triangles",comparisonDuplicateTriangles},
            {"duplicates",comparisonDuplicatePoints|comparisonDuplicateTriangles},{"degenerates",comparisonDegenerates},
            {"quality",comparisonQuality},{"distance",comparisonDistance},{"intersections",comparisonIntersections}};
        for (const auto& operation : split(o.operations)) {
            MeshComparison retained;
            std::array<ComparisonAnalysisInput,2> cached{};
            const bool warm = operation == "topology-warm" || operation == "intersections-warm" || operation == "display";
            if (warm) {
                const auto stage = operation == "intersections-warm" ? comparisonIntersections : comparisonTopology;
                const auto phase = obj_profile::begin("cache.prepare");
                retained = computeComparisonStages(a,{},stage,stop,{},mode,limits);
                obj_profile::end(phase); cached = comparisonAnalysisInputs(retained);
            }
            for (size_t run = 0; run < o.runs; ++run) {
                if (o.cancelAfterMs >= 0) { cancellation = std::stop_source{}; stop = cancellation.get_token(); }
                std::cerr << operation << " run " << run << '\n';
                const std::string label = "op."+operation;
                auto phase = obj_profile::begin(label.c_str());
                std::atomic<int64_t> requestedNs{-1};
                std::jthread timer;
                if (o.cancelAfterMs >= 0) {
                    const auto request = [&] {
                        requestedNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(obj_profile::Clock::now().time_since_epoch()).count());
                        cancellation.request_stop();
                    };
                    if (o.cancelAfterMs == 0) { request(); }
                    else {
                        timer = std::jthread([&,request](std::stop_token timerStop) {
                            std::mutex mutex; std::condition_variable_any wake; std::unique_lock lock(mutex);
                            (void)wake.wait_for(lock,timerStop,std::chrono::milliseconds(o.cancelAfterMs),[] { return false; });
                            if (!timerStop.stop_requested()) { request(); }
                        });
                    }
                }
                bool finished = false;
                Json sample{{"operation",operation},{"run",run},{"status","ok"}};
                const auto finish = [&] {
                    if (finished) { return; }
                    const auto completedAt = obj_profile::Clock::now();
                    obj_profile::end(phase); finished = true;
                    if (timer.joinable()) { timer.request_stop(); timer.join(); }
                    const auto requested = requestedNs.load();
                    if (requested >= 0) {
                        const auto requestAt = obj_profile::Clock::time_point(std::chrono::duration_cast<obj_profile::Clock::duration>(std::chrono::nanoseconds(requested)));
                        sample["cancel_requested_ms"] = std::chrono::duration<double,std::milli>(requestAt-phase.start).count();
                        if (requestAt <= completedAt) { sample["cancel_response_ms"] = std::chrono::duration<double,std::milli>(completedAt-requestAt).count(); }
                        else { sample["cancel_requested_after_completion"] = true; }
                    }
                };
                try {
                    if (operation == "difference") {
                        auto result = buildDifferenceMesh(a,b,stop);
                        finish();
                        sample["result"] = {{"constructed",result.constructed},{"volumes",result.volumes},{"closure_triangles",result.closureTriangles},
                            {"triangles",{meshGeometry(result.parts[0]).indices.size()/3,meshGeometry(result.parts[1]).indices.size()/3,meshGeometry(result.parts[2]).indices.size()/3}},
                            {"message",result.message}};
                        release(result,operation);
                    } else if (operation == "display") {
                        TopologyInspectionSettings settings; settings.holeSizeRatioTolerance = 1; settings.finMaxAreaRatio = .5f;
                        auto result = prepareComparisonTopologyDisplay({retained.original.topologyDisplayInputs,retained.repaired.topologyDisplayInputs},settings,stop);
                        finish(); sample["result"] = summary(result);
                        release(result,operation);
                    } else {
                        const auto key = operation == "topology-warm" ? "topology" : operation == "intersections-warm" ? "intersections" : operation;
                        const auto found = masks.find(key); require(found != masks.end(),"Unknown operation.");
                        auto result = computeComparisonStages(a,operation == "distance" ? b : Mesh{},found->second,stop,{},mode,limits,cached);
                        finish(); sample["result"] = summary(result);
                        if (warm) { sample["geometry_reused"] = result.original.analysis.geometry == cached[0].geometry; }
                        release(result,operation);
                    }
                } catch (const std::exception& error) {
                    finish(); sample["status"] = "error"; sample["error"] = error.what();
                }
                report["samples"].push_back(std::move(sample));
                write(o,report);
            }
        }
        require(analysisWorkerCount.load() == reserved,"Worker budget leaked."); analysisWorkerCount.store(0);
        const auto profile = obj_profile::result(); report["events"] = profile.at("events"); report["clock_origin"] = obj_profile::clockOrigin();
        if (o.output.empty()) { std::cout << report.dump() << '\n'; } else { write(o,report); }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
