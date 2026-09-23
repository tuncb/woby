#include "analysis_results.h"
#include "utf8_path.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace woby {
namespace {
using Json = nlohmann::json;
struct Node;
struct Array {
    size_t size = 0, summaryLimit = 100;
    std::function<Node(size_t)> at;
    std::string truncated;
    bool incomplete = false;
};
// A lazy tree over an immutable analysis snapshot. Callbacks never capture local nodes.
struct Node {
    Json value = Json::object();
    std::map<std::string, Array> arrays;
};
void array(Node& node, const std::string& name, size_t size, std::function<Node(size_t)> at,
    const std::string& truncated = {}, size_t summaryLimit = 100, bool incomplete = false)
{
    node.arrays.emplace(name, Array{size, summaryLimit, std::move(at), truncated, incomplete});
}
Node scalar(Json value) { return {std::move(value), {}}; }
Json face(const TopologyFaceReference& f)
{
    return {{"sourceId", std::to_string(f.fileId)}, {"partId", std::to_string(f.partId)}, {"triangleId", f.triangleId + 1}};
}
Json point(const TopologyPointReference& p)
{
    return {{"partId", std::to_string(p.partId)}, {"pointId", p.pointId + 1}};
}
Node topologyBase(const MeshTopology& t)
{
    Node n = scalar({{"status", topologyStatus(t)}, {"algorithm", "woby-source-topology-v1"},
        {"topologyMode", topologyModeName(t.mode)}, {"scope", "per-source; selected parts"},
        {"excludedCollapsedFaces", t.excludedCollapsedFaces}, {"unavailableSources", t.unavailableSources}, {"sourceCount", t.sources.size()}});
    array(n, "sources", t.sources.size(), [&t](size_t i) {
        const auto& s = t.sources[i];
        return scalar({{"sourceId", std::to_string(s.fileId)}, {"source", s.source},
            {"provenance", triangleProvenanceName(s.provenance)}, {"topologyMode", topologyModeName(s.mode)},
            {"status", s.available ? "complete" : "unavailable"}, {"excludedCollapsedFaces", s.excludedCollapsedFaces}});
    }, "sourcesTruncated");
    return n;
}
Node topologyEdges(const MeshTopology& t, const std::vector<TopologyEdgeFinding>& findings, bool winding)
{
    auto n = topologyBase(t);
    const size_t count = winding ? t.windingFaces.size() : findings.size();
    n.value.update({{"count", t.unavailableSources ? Json(nullptr) : Json(count)}, {"knownCount", count},
        {"findingCount", findings.size()}, {"countUnit", winding ? "unique triangle instances" : "edges"}});
    array(n, "findings", findings.size(), [&t, &findings](size_t i) {
        const auto& f = findings[i]; const auto& s = t.sources[f.source]; const auto& e = s.edges[f.edge];
        auto item = scalar({{"sourceId", std::to_string(s.fileId)}, {"source", s.source}, {"edgeId", f.edge + 1},
            {"topologyMode", topologyModeName(s.mode)}, {"provenance", triangleProvenanceName(s.provenance)},
            {"incidentFaceCount", e.incidentFaces.size()}, {"sameDirection", e.windingConflict}, {"orientationContradiction", e.orientationContradiction}});
        array(item, "incidentFaces", e.incidentFaces.size(), [&s, &e](size_t k) {
            const auto& use = e.incidentFaces[k]; auto v = face(s.faces[use.face].reference); v["forward"] = use.forward; return scalar(std::move(v));
        }, "incidentFacesTruncated");
        array(item, "endpoints", 2, [&s, &e](size_t k) {
            const auto& v = s.vertices[e.vertices[k]];
            auto endpoint = scalar({{"position", v.position}, {"sourcePointCount", v.references.size()}});
            array(endpoint, "sourcePoints", v.references.size(), [&v](size_t j) { return scalar(point(v.references[j])); }, "sourcePointsTruncated");
            return endpoint;
        });
        return item;
    }, "findingsTruncated");
    if (winding) {
        n.value.update({{"orientationContradictions", t.orientationContradictions}});
        array(n, "affectedFaces", t.windingFaces.size(), [&t](size_t i) { return scalar(face(t.windingFaces[i])); }, "affectedFacesTruncated");
    }
    return n;
}
Node vertex(const SourceTopology& s, size_t index)
{
    const auto& v = s.vertices[index];
    auto n = scalar({{"sourceId", std::to_string(s.fileId)}, {"source", s.source}, {"vertexId", index + 1},
        {"position", v.position}, {"topologyMode", topologyModeName(s.mode)},
        {"pointReferenceCount", v.references.size()}, {"incidentFaceCount", v.faces.size()}});
    array(n, "pointReferences", v.references.size(), [&v](size_t i) { return scalar(point(v.references[i])); }, "pointReferencesTruncated");
    array(n, "incidentFaces", v.faces.size(), [&s, &v](size_t i) { return scalar(face(s.faces[v.faces[i]].reference)); }, "incidentFacesTruncated");
    return n;
}
Node boundary(const MeshTopology& t, size_t index)
{
    const auto& b = t.boundaryRegions[index]; const auto& s = t.sources[b.source];
    auto n = scalar({{"sourceId", std::to_string(s.fileId)}, {"source", s.source}, {"boundaryId", index + 1},
        {"componentId", b.component + 1}, {"topologyMode", topologyModeName(s.mode)}, {"kind", boundaryKindName(b.kind)},
        {"diagonal", b.diagonal}, {"componentDiagonal", b.componentDiagonal},
        {"sizeRatio", b.ratioAvailable ? Json(b.sizeRatio) : Json(nullptr)}, {"vertexCount", b.vertices.size()}, {"edgeCount", b.edges.size()}});
    array(n, "vertices", b.vertices.size(), [&s, &b](size_t i) {
        const auto index = b.vertices[i]; const auto& v = s.vertices[index];
        auto item = scalar({{"vertexId", index + 1}, {"position", v.position}, {"pointReferenceCount", v.references.size()}});
        array(item, "pointReferences", v.references.size(), [&v](size_t j) { return scalar(point(v.references[j])); }, "pointReferencesTruncated", 1);
        return item;
    }, "verticesTruncated");
    array(n, "edgeIds", b.edges.size(), [&b](size_t i) { return scalar(b.edges[i] + 1); }, "edgesTruncated");
    array(n, "incidentFacesByEdge", b.edges.size(), [&s, &b](size_t i) { return scalar(face(s.faces[s.edges[b.edges[i]].incidentFaces[0].face].reference)); });
    return n;
}
Node topologyInspection(const MeshTopology& t, bool holes)
{
    const bool enabled = holes ? t.inspection.holes : t.inspection.nonManifoldVertices;
    const size_t count = enabled ? (holes ? t.holes.size() : t.nonManifoldVertices.size()) : 0;
    auto n = topologyBase(t);
    n.value.update({{"algorithm", holes ? "woby-boundary-loops-v1" : "woby-vertex-links-v1"},
        {"status", enabled ? topologyStatus(t) : "disabled"}, {"count", !enabled || t.unavailableSources ? Json(nullptr) : Json(count)},
        {"knownCount", count}, {"findingCount", count}, {"countUnit", "edges"}});
    array(n, "findings", count, [&t, holes](size_t i) {
        if (holes) { return boundary(t, t.holes[i]); }
        const auto& f = t.nonManifoldVertices[i]; auto item = vertex(t.sources[f.source], f.vertex);
        item.value["linkComponents"] = f.linkComponents; return item;
    }, "findingsTruncated");
    if (holes) {
        size_t loops = 0, open = 0, branched = 0, unavailable = 0;
        if (enabled) { for (const auto& b : t.boundaryRegions) {
            loops += b.kind == BoundaryKind::loop; open += b.kind == BoundaryKind::open;
            branched += b.kind == BoundaryKind::branched; unavailable += !b.ratioAvailable;
        } }
        n.value.update({{"holeSizeRatioTolerance", t.inspection.holeSizeRatioTolerance}, {"boundaryRegionCount", loops + open + branched},
            {"loopCount", loops}, {"openBoundaryCount", open}, {"branchedBoundaryCount", branched}, {"unavailableSizeRatioCount", unavailable}});
        array(n, "boundaryRegions", enabled ? t.boundaryRegions.size() : 0, [&t](size_t i) { return boundary(t, i); }, "boundaryRegionsTruncated");
    } else { n.value["excludedNonManifoldEdgeVertices"] = enabled ? t.excludedNonManifoldEdgeVertices : 0; }
    return n;
}
Node fins(const MeshTopology& t)
{
    const bool enabled = t.inspection.fins; const size_t count = enabled ? t.fins.size() : 0;
    auto n = topologyBase(t);
    n.value.update({{"algorithm", "woby-fin-candidates-v1"}, {"heuristic", true}, {"status", enabled ? finStatus(t) : "disabled"},
        {"unavailableAreaSources", t.unavailableFinAreaSources},
        {"count", !enabled || t.unavailableSources || t.unavailableFinAreaSources ? Json(nullptr) : Json(count)},
        {"knownCount", count}, {"findingCount", count}, {"countUnit", "edges"}, {"maxAreaRatio", t.inspection.finMaxAreaRatio},
        {"boundaryDefinition", "physical edges only; non-manifold cuts retained separately"},
        {"denominatorDefinition", "largest physical-boundary-bearing split patch per source, before boundary-shape filtering"}});
    array(n, "findings", count, [&t](size_t i) {
        const auto& p = t.finPatches[t.fins[i]]; const auto& s = t.sources[p.source];
        auto item = scalar({{"sourceId", std::to_string(s.fileId)}, {"source", s.source}, {"provenance", triangleProvenanceName(s.provenance)},
            {"patchId", p.patch + 1}, {"topologyMode", topologyModeName(s.mode)}, {"splitComponentCount", p.splitComponentCount},
            {"boundaryKind", finBoundaryKindName(p.boundary)}, {"boundaryComponentCount", p.boundaryComponents},
            {"area", p.area}, {"denominatorArea", p.denominatorArea}, {"areaRatio", p.areaRatio}, {"faceCount", p.faces.size()},
            {"physicalBoundaryEdgeCount", p.physicalBoundaryEdges.size()}, {"cutBoundaryEdgeCount", p.cutBoundaryEdges.size()}});
        array(item, "faces", p.faces.size(), [&s, &p](size_t j) { return scalar(face(s.faces[p.faces[j]].reference)); }, "facesTruncated");
        array(item, "physicalBoundaryEdgeIds", p.physicalBoundaryEdges.size(), [&p](size_t j) { return scalar(p.physicalBoundaryEdges[j] + 1); }, "physicalBoundaryEdgesTruncated");
        array(item, "cutBoundaryEdgeIds", p.cutBoundaryEdges.size(), [&p](size_t j) { return scalar(p.cutBoundaryEdges[j] + 1); }, "cutBoundaryEdgesTruncated");
        return item;
    }, "findingsTruncated");
    return n;
}
Node intersections(const MeshIntersections& r)
{
    const bool enabled = r.phase == IntersectionPhase::complete, complete = enabled && !r.truncated && !r.unavailableSources;
    auto n = scalar({{"status", intersectionStatus(r)}, {"error", r.error},
        {"previousCount", !enabled && r.hasResult ? Json(r.findings.size()) : Json(nullptr)}, {"algorithm", "woby-exact-rational-intersections-v1"},
        {"scope", "per-source; selected parts; world coordinates; no display offset"}, {"topologyMode", topologyModeName(r.mode)},
        {"count", complete ? Json(r.findings.size()) : Json(nullptr)}, {"knownCount", enabled ? r.findings.size() : 0},
        {"affectedFaceCount", complete ? Json(r.affectedFaces) : Json(nullptr)}, {"knownAffectedFaceCount", enabled ? r.affectedFaces : 0},
        {"excludedCollapsedFaces", enabled ? r.excludedCollapsedFaces : 0}, {"unavailableSources", enabled ? r.unavailableSources : 0},
        {"candidateTests", enabled ? r.candidateTests : 0}, {"detectionTruncated", enabled && r.truncated},
        {"pairLimit", r.limits.pairs}, {"candidateLimit", r.limits.candidateTests},
        {"truncationReason", enabled && r.truncated ? Json(r.truncationReason) : Json(nullptr)}});
    array(n, "findings", enabled ? r.findings.size() : 0, [&r](size_t i) {
        const auto& f = r.findings[i]; return scalar({{"source", f.source}, {"provenance", triangleProvenanceName(f.provenance)},
            {"topologyMode", topologyModeName(f.mode)}, {"faces", {face(f.faces[0]), face(f.faces[1])}}});
    }, "findingsTruncated", 100, enabled && r.truncated);
    return n;
}
Node degenerates(const MeshDegenerates& r)
{
    const bool enabled = r.settings.enabled;
    auto n = scalar({{"status", degenerateStatus(r)}, {"algorithm", "woby-degenerate-triangles-v1"},
        {"scope", "per-source generated triangle / transformed part instance; world coordinates; no display offset"},
        {"needleThresholdRatio", r.settings.needleThresholdRatio}, {"capMinAngleDegrees", r.settings.capMinAngleDegrees},
        {"count", enabled && !r.unavailableSources ? Json(r.findings.size()) : Json(nullptr)}, {"knownCount", enabled ? r.findings.size() : 0},
        {"unavailableSources", enabled ? r.unavailableSources : 0},
        {"reasonCounts", {{"collapsed", enabled ? r.collapsedCount : 0}, {"needle", enabled ? r.needleCount : 0}, {"cap", enabled ? r.capCount : 0}}}});
    array(n, "findings", enabled ? r.findings.size() : 0, [&r](size_t i) {
        const auto& f = r.findings[i]; return scalar({{"sourceId", std::to_string(f.fileId)}, {"partId", std::to_string(f.partId)},
            {"source", f.source}, {"provenance", triangleProvenanceName(f.provenance)}, {"triangleId", f.triangleId + 1},
            {"reasons", {{"collapsed", f.reasons.collapsed}, {"needle", f.reasons.needle}, {"cap", f.reasons.cap}}},
            {"edgeRatio", std::isfinite(f.reasons.edgeRatio) ? Json(f.reasons.edgeRatio) : Json(nullptr)},
            {"maximumAngleDegrees", std::isfinite(f.reasons.maximumAngleDegrees) ? Json(f.reasons.maximumAngleDegrees) : Json(nullptr)}});
    }, "findingsTruncated");
    return n;
}
Node duplicates(const DuplicateResult& r)
{
    if (!r.enabled) {
        auto n = scalar({{"status", "disabled"}, {"count", nullptr}, {"knownDuplicateCount", 0}, {"informationalCount", 0}, {"unavailableSources", 0}, {"groupCount", 0}});
        array(n, "findings", 0, {}, "findingsTruncated"); return n;
    }
    auto n = scalar({{"status", duplicateStatus(r)}, {"count", !r.unavailableSources ? Json(r.duplicateCount) : Json(nullptr)},
        {"knownDuplicateCount", r.duplicateCount}, {"informationalCount", r.informationalCount}, {"unavailableSources", r.unavailableSources}, {"groupCount", r.findings.size()}});
    array(n, "findings", r.findings.size(), [&r](size_t i) {
        const auto& f = r.findings[i]; auto item = scalar({{"sourceId", std::to_string(f.fileId)}, {"source", f.source},
            {"provenance", sourceProvenanceName(f.provenance)}, {"representativeId", f.members.front().id + 1}, {"memberCount", f.members.size()}});
        array(item, "members", f.members.size(), [&f](size_t j) { return scalar({{"id", f.members[j].id + 1}, {"reversed", f.members[j].reversed}}); }, "membersTruncated");
        return item;
    }, "findingsTruncated"); return n;
}
Node detectorNode(const MeshComparison& r, ComparisonSide side, const std::string& detector)
{
    const auto it = std::find(diagnosticCategoryKeys.begin(), diagnosticCategoryKeys.end(), detector);
    if (it == diagnosticCategoryKeys.end()) { throw std::invalid_argument("Unknown detector."); }
    const auto index = static_cast<size_t>(it - diagnosticCategoryKeys.begin());
    if (index < backgroundDetectorCount && r.detectors[index].phase != IntersectionPhase::complete) {
        const auto& status = r.detectors[index];
        auto n = scalar({{"status", detectorPhaseName(status.phase)}, {"count", nullptr},
            {"knownCount", status.knownCounts[side == ComparisonSide::a ? 0 : 1]}, {"hasPreviousResult", status.hasResult}, {"error", status.error}});
        array(n, "findings", 0, {}); return n;
    }
    const auto& s = side == ComparisonSide::a ? r.original : r.repaired;
    switch (static_cast<DiagnosticCategory>(index)) {
    case DiagnosticCategory::boundary: return topologyEdges(s.topology, s.topology.boundaries, false);
    case DiagnosticCategory::nonManifold: return topologyEdges(s.topology, s.topology.nonManifoldEdges, false);
    case DiagnosticCategory::winding: return topologyEdges(s.topology, s.topology.windingEdges, true);
    case DiagnosticCategory::duplicatePoints: return duplicates(s.duplicates.points);
    case DiagnosticCategory::duplicateTriangles: return duplicates(s.duplicates.triangles);
    case DiagnosticCategory::degenerateTriangles: return degenerates(s.degenerates);
    case DiagnosticCategory::nonManifoldVertices: return topologyInspection(s.topology, false);
    case DiagnosticCategory::holes: return topologyInspection(s.topology, true);
    case DiagnosticCategory::fins: return fins(s.topology);
    case DiagnosticCategory::selfIntersections: return intersections(s.intersections);
    }
    throw std::invalid_argument("Unknown detector.");
}
Json materialize(const Node& n)
{
    auto result = n.value;
    for (const auto& [name, a] : n.arrays) {
        auto values = Json::array();
        for (size_t i = 0; i < std::min(a.size, a.summaryLimit); ++i) { values.push_back(materialize(a.at(i))); }
        result[name] = std::move(values);
        if (!a.truncated.empty()) { result[a.truncated] = a.incomplete || a.size > a.summaryLimit; }
    }
    return result;
}
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis export canceled."); }
}
bool detectionComplete(const MeshComparison& result, const Json& summary)
{
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        const char* key = side == ComparisonSide::a ? "aToB" : "bToA";
        if (!summary.contains(key) || summary[key].is_null()) { continue; }
        for (const auto* detector : diagnosticCategoryKeys) {
            const auto metadata = detectorNode(result, side, detector).value;
            const auto status = metadata.value("status", "unavailable");
            if (status != "complete" && status != "disabled") { return false; }
        }
    }
    return true;
}
void writeNode(std::ostream& out, const Node& n, std::stop_token stop, std::atomic<size_t>* written)
{
    checkStop(stop);
    if (n.arrays.empty()) { out << n.value.dump(); return; }
    Json fields = n.value;
    for (const auto& [name, a] : n.arrays) {
        if (!a.truncated.empty()) { fields[a.truncated] = a.incomplete; }
    }
    out << '{'; bool comma = false;
    for (const auto& item : fields.items()) {
        if (comma) { out << ','; } comma = true;
        out << Json(item.key()).dump() << ':' << item.value().dump();
    }
    for (const auto& [name, a] : n.arrays) {
        if (comma) { out << ','; } comma = true;
        out << Json(name).dump() << ":[";
        for (size_t i = 0; i < a.size; ++i) {
            if (i) { out << ','; }
            writeNode(out, a.at(i), stop, written);
            if (written) { written->fetch_add(1, std::memory_order_relaxed); }
        }
        out << ']';
    }
    out << '}';
}
} // namespace

nlohmann::json analysisDetectorSummary(const MeshComparison& result, ComparisonSide side, const std::string& detector)
{
    return materialize(detectorNode(result, side, detector));
}
nlohmann::json analysisResultPage(const MeshComparison& result, ComparisonSide side,
    const std::string& detector, const std::string& collection, size_t offset, size_t limit)
{
    if (!limit || limit > 100) { throw std::invalid_argument("limit must be from 1 to 100."); }
    if (collection.empty() || collection[0] != '/') { throw std::invalid_argument("collection must be an array path starting with /."); }
    auto node = detectorNode(result, side, detector);
    const auto metadata = node.value;
    size_t pos = 1;
    Array selected;
    while (true) {
        const size_t slash = collection.find('/', pos);
        const auto key = collection.substr(pos, slash == std::string::npos ? slash : slash - pos);
        const auto it = node.arrays.find(key);
        if (it == node.arrays.end()) { throw std::invalid_argument("Unknown array collection: " + collection); }
        selected = it->second;
        if (slash == std::string::npos) { break; }
        const size_t next = collection.find('/', slash + 1);
        if (next == std::string::npos) { throw std::invalid_argument("collection must end with an array name."); }
        size_t index = 0;
        const auto parsed = std::from_chars(collection.data() + slash + 1, collection.data() + next, index);
        if (parsed.ec != std::errc{} || parsed.ptr != collection.data() + next || index >= selected.size) {
            throw std::invalid_argument("Collection index is outside the current results.");
        }
        node = selected.at(index); pos = next + 1;
    }
    const size_t begin = std::min(offset, selected.size), end = begin + std::min(limit, selected.size - begin);
    auto items = Json::array();
    for (size_t i = begin; i < end; ++i) { items.push_back(materialize(selected.at(i))); }
    return {{"side", side == ComparisonSide::a ? "a" : "b"}, {"detector", detector}, {"collection", collection},
        {"offset", offset}, {"limit", limit}, {"total", selected.size}, {"nextOffset", end < selected.size ? Json(end) : Json(nullptr)},
        {"items", std::move(items)}, {"metadata", metadata}};
}

void writeAnalysisResults(std::ostream& out, const MeshComparison& result, Json summary,
    std::stop_token stop, std::atomic<size_t>* written)
{
    summary["allRetainedResults"] = true;
    summary["detectionComplete"] = detectionComplete(result, summary);
    out << '{'; bool comma = false;
    for (const auto& field : summary.items()) {
        checkStop(stop);
        if (comma) { out << ','; } comma = true;
        out << Json(field.key()).dump() << ':';
        if ((field.key() != "aToB" && field.key() != "bToA") || field.value().is_null()) { out << field.value().dump(); continue; }
        const auto side = field.key() == "aToB" ? ComparisonSide::a : ComparisonSide::b;
        out << '{'; bool innerComma = false;
        for (const auto& item : field.value().items()) {
            if (item.key() == "detectors") { continue; }
            if (innerComma) { out << ','; } innerComma = true;
            out << Json(item.key()).dump() << ':' << item.value().dump();
        }
        if (innerComma) { out << ','; }
        out << "\"detectors\":{\"schemaVersion\":1,\"idBase\":1,\"scope\":\"per-source; selected parts, including unused points when whole file selected\"";
        for (const auto* key : diagnosticCategoryKeys) {
            out << ',' << Json(key).dump() << ':';
            writeNode(out, detectorNode(result, side, key), stop, written);
        }
        out << "}}";
    }
    out << "}\n";
    checkStop(stop);
    if (!out) { throw std::runtime_error("Unable to write analysis export."); }
}

void startAnalysisExport(AnalysisExportRuntime& runtime, const MeshComparison& result,
    Json summary, const std::string& target, const std::filesystem::path& path)
{
    (void)analysisExportStatus(runtime);
    if (runtime.worker.valid()) { throw std::invalid_argument("An export is already running; use analysis export-status or export-cancel."); }
    if (!path.is_absolute() || !std::filesystem::is_directory(path.parent_path())) { throw std::invalid_argument("Export requires an absolute path in an existing directory."); }
    if (std::filesystem::exists(path)) { throw std::invalid_argument("Export destination already exists; choose a new filename."); }
    // Copy only diagnostic data. Meshes, render buffers, and distance samples are not needed by the writer.
    MeshComparison snapshot; snapshot.detectors = result.detectors;
    for (size_t side = 0; side < 2; ++side) {
        const auto& source = side == 0 ? result.original : result.repaired;
        auto& dest = side == 0 ? snapshot.original : snapshot.repaired;
        dest.topology = source.topology; dest.duplicates = source.duplicates;
        dest.degenerates = source.degenerates; dest.intersections = source.intersections;
    }
    runtime.stop = {}; runtime.written = 0; runtime.target = target; runtime.path = path;
    runtime.outcome = {{"state", "running"}, {"target", target}, {"path", pathToUtf8(path)}};
    const bool complete = detectionComplete(snapshot, summary);
    runtime.worker = std::async(std::launch::async, [snapshot = std::move(snapshot), summary = std::move(summary), complete,
        path, stop = runtime.stop.get_token(), written = &runtime.written]() mutable -> Json {
        std::filesystem::path staging;
        bool ownsStaging = false;
        try {
            // Atomically reserve a sibling directory, so cleanup only touches this job's files.
            for (size_t suffix = 0; ; ++suffix) {
                staging = path; staging += ".export-" + std::to_string(suffix);
                if (std::filesystem::create_directory(staging)) { ownsStaging = true; break; }
                checkStop(stop);
            }
            const auto temporary = staging / "results.json";
            { std::ofstream out(temporary, std::ios::binary); out.exceptions(std::ios::badbit | std::ios::failbit);
              writeAnalysisResults(out, snapshot, std::move(summary), stop, written); out.close(); }
            checkStop(stop);
            // Match scene saving: atomic no-clobber publication on the same filesystem.
#ifdef _WIN32
            if (!MoveFileExW(temporary.c_str(), path.c_str(), 0)) {
                throw std::filesystem::filesystem_error("Cannot publish analysis export", path,
                    std::error_code(static_cast<int>(GetLastError()), std::system_category()));
            }
#else
            std::filesystem::create_hard_link(temporary, path);
#endif
            std::error_code ignored; std::filesystem::remove(temporary, ignored); std::filesystem::remove(staging, ignored);
            return {{"state", "complete"}, {"allRetainedResults", true}, {"detectionComplete", complete}};
        } catch (const std::exception& error) {
            if (ownsStaging) {
                std::error_code ignored; std::filesystem::remove(staging / "results.json", ignored); std::filesystem::remove(staging, ignored);
            }
            return {{"state", stop.stop_requested() ? "canceled" : "failed"}, {"error", error.what()}};
        }
    });
}
Json analysisExportStatus(AnalysisExportRuntime& runtime)
{
    if (runtime.worker.valid() && runtime.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        runtime.outcome.update(runtime.worker.get());
    }
    if (runtime.outcome.is_null()) { return {{"state", "idle"}}; }
    auto result = runtime.outcome; result["entriesWritten"] = runtime.written.load(std::memory_order_relaxed); return result;
}
void cancelAnalysisExport(AnalysisExportRuntime& runtime) { runtime.stop.request_stop(); }
void stopAnalysisExport(AnalysisExportRuntime& runtime)
{
    runtime.stop.request_stop();
    if (runtime.worker.valid()) { runtime.worker.wait(); (void)analysisExportStatus(runtime); }
}
} // namespace woby
