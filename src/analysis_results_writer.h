#pragma once

#include "analysis_results.h"
#include <algorithm>
#include <charconv>
#include <concepts>
#include <string_view>

// Private implementation of the full export path. Metadata and paginated
// summaries continue to use the lazy tree in analysis_results.cpp.
namespace woby::analysis_export_detail {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
inline double milliseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
inline void checkStop(std::stop_token stop) {
    if (stop.stop_requested()) { throw std::runtime_error("Analysis export canceled."); }
}
struct Writer {
    std::ostream& stream;
    std::stop_token stop;
    std::atomic<size_t>* written;
    AnalysisExportWriteMetrics* metrics;
    std::string buffer;
    size_t entries = 0;
};
inline void publishProgress(Writer& w) {
    if (w.written) { w.written->store(w.entries, std::memory_order_relaxed); }
}
inline void flush(Writer& w) {
    checkStop(w.stop);
    if (w.buffer.empty()) { return; }
    const auto start = Clock::now();
    w.stream.write(w.buffer.data(), static_cast<std::streamsize>(w.buffer.size()));
    if (!w.stream) { throw std::runtime_error("Unable to write analysis export."); }
    if (w.metrics) {
        w.metrics->streamWriteMs += milliseconds(start);
        w.metrics->bytesWritten += w.buffer.size();
    }
    w.buffer.clear();
    publishProgress(w);
}
inline void append(Writer& w, std::string_view text) {
    constexpr size_t capacity = 64 * 1024;
    while (!text.empty()) {
        const auto count = std::min(capacity - w.buffer.size(), text.size());
        w.buffer.append(text.data(), count); text.remove_prefix(count);
        if (w.buffer.size() == capacity) { flush(w); }
    }
}
inline void value(Writer& w, const Json& v) { append(w, v.dump()); }
inline void value(Writer& w, const std::string& v) { value(w, Json(v)); }
inline void value(Writer& w, const char* v) { value(w, Json(v)); }
inline void value(Writer& w, double v) { value(w, Json(v)); }
inline void value(Writer& w, float v) { value(w, Json(v)); }
inline void value(Writer& w, bool v) { append(w, v ? "true" : "false"); }
template<std::integral T> inline void value(Writer& w, T v) {
    char buffer[32];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), v);
    if (result.ec != std::errc{}) { throw std::runtime_error("Unable to format analysis ID."); }
    append(w, std::string_view(buffer, static_cast<size_t>(result.ptr - buffer)));
}
inline void id(Writer& w, uint64_t v) { append(w, "\""); value(w, v); append(w, "\""); }
struct Object { Writer& writer; bool comma = false; };
inline Object object(Writer& w) { append(w, "{"); return {w}; }
inline void end(Object& o) { append(o.writer, "}"); }
// Keys here are fixed ASCII schema literals, never source/user strings.
inline void key(Object& o, std::string_view name) {
    append(o.writer, o.comma ? ",\"" : "\""); o.comma = true;
    append(o.writer, name); append(o.writer, "\":");
}
template<class T> inline void field(Object& o, std::string_view name, const T& v) {
    key(o, name); value(o.writer, v);
}
inline void idField(Object& o, std::string_view name, uint64_t v) { key(o, name); id(o.writer, v); }
template<class F> inline void array(Writer& w, size_t count, F at) {
    append(w, "[");
    for (size_t i = 0; i < count; ++i) {
        checkStop(w.stop);
        if (i) { append(w, ","); }
        at(i); ++w.entries;
        if (w.entries % 1024 == 0) { publishProgress(w); }
    }
    append(w, "]");
}
template<class F> inline void arrayField(Object& o, std::string_view name, size_t count, F at) {
    key(o, name); array(o.writer, count, at);
}
inline void faceFields(Object& o, const TopologyFaceReference& f) {
    idField(o, "partId", f.partId); idField(o, "sourceId", f.fileId); field(o, "triangleId", f.triangleId + 1);
}
inline void face(Writer& w, const TopologyFaceReference& f) { auto o = object(w); faceFields(o, f); end(o); }
inline void point(Writer& w, const TopologyPointReference& p) {
    auto o = object(w); idField(o, "partId", p.partId); field(o, "pointId", p.pointId + 1); end(o);
}
inline void sourceFields(Object& o, const SourceTopology& s) {
    field(o, "source", s.source); idField(o, "sourceId", s.fileId); field(o, "topologyMode", topologyModeName(s.mode));
}
inline void source(Writer& w, const SourceTopology& s) {
    auto o = object(w); sourceFields(o, s);
    field(o, "provenance", sourceProvenanceName(s.provenance)); field(o, "status", s.available ? "complete" : "unavailable");
    field(o, "excludedCollapsedFaces", s.excludedCollapsedFaces); end(o);
}
inline void edge(Writer& w, const MeshTopology& t, const TopologyEdgeFinding& f) {
    const auto& s = t.sources[f.source]; const auto& e = s.edges[f.edge];
    auto o = object(w); sourceFields(o, s); field(o, "edgeId", f.edge + 1);
    field(o, "provenance", sourceProvenanceName(s.provenance)); field(o, "incidentFaceCount", e.incidentFaces.size());
    field(o, "sameDirection", e.windingConflict); field(o, "orientationContradiction", e.orientationContradiction);
    field(o, "incidentFacesTruncated", false);
    arrayField(o, "incidentFaces", e.incidentFaces.size(), [&](size_t i) {
        const auto& use = e.incidentFaces[i]; auto item = object(w);
        faceFields(item, s.faces[use.face].reference); field(item, "forward", use.forward); end(item);
    });
    arrayField(o, "endpoints", 2, [&](size_t i) {
        const auto& v = s.vertices[e.vertices[i]]; auto item = object(w);
        field(item, "position", originalPosition(v.position, t.coordinateOrigin));
        field(item, "sourcePointCount", v.references.size()); field(item, "sourcePointsTruncated", false);
        arrayField(item, "sourcePoints", v.references.size(), [&](size_t j) { point(w, v.references[j]); }); end(item);
    }); end(o);
}
inline void vertex(Writer& w, const MeshTopology& t, const TopologyVertexFinding& f) {
    const auto& s = t.sources[f.source]; const auto& v = s.vertices[f.vertex];
    auto o = object(w); sourceFields(o, s); field(o, "vertexId", f.vertex + 1);
    field(o, "position", originalPosition(v.position, t.coordinateOrigin));
    field(o, "pointReferenceCount", v.references.size()); field(o, "incidentFaceCount", v.faces.size());
    field(o, "linkComponents", f.linkComponents); field(o, "pointReferencesTruncated", false); field(o, "incidentFacesTruncated", false);
    arrayField(o, "pointReferences", v.references.size(), [&](size_t i) { point(w, v.references[i]); });
    arrayField(o, "incidentFaces", v.faces.size(), [&](size_t i) { face(w, s.faces[v.faces[i]].reference); }); end(o);
}
inline void boundary(Writer& w, const MeshTopology& t, size_t index) {
    const auto& b = t.boundaryRegions[index]; const auto& s = t.sources[b.source];
    auto o = object(w); sourceFields(o, s); field(o, "boundaryId", index + 1); field(o, "componentId", b.component + 1);
    field(o, "kind", boundaryKindName(b.kind)); field(o, "diagonal", b.diagonal); field(o, "componentDiagonal", b.componentDiagonal);
    field(o, "sizeRatio", b.ratioAvailable ? Json(b.sizeRatio) : Json(nullptr));
    field(o, "vertexCount", b.vertices.size()); field(o, "edgeCount", b.edges.size());
    field(o, "verticesTruncated", false); field(o, "edgesTruncated", false);
    arrayField(o, "vertices", b.vertices.size(), [&](size_t i) {
        const auto vi = b.vertices[i]; const auto& v = s.vertices[vi]; auto item = object(w);
        field(item, "vertexId", vi + 1); field(item, "position", originalPosition(v.position, t.coordinateOrigin));
        field(item, "pointReferenceCount", v.references.size()); field(item, "pointReferencesTruncated", false);
        arrayField(item, "pointReferences", v.references.size(), [&](size_t j) { point(w, v.references[j]); }); end(item);
    });
    arrayField(o, "edgeIds", b.edges.size(), [&](size_t i) { value(w, b.edges[i] + 1); });
    arrayField(o, "incidentFacesByEdge", b.edges.size(), [&](size_t i) { face(w, s.faces[s.edges[b.edges[i]].incidentFaces[0].face].reference); }); end(o);
}
inline void fin(Writer& w, const MeshTopology& t, size_t index) {
    const auto& p = t.finPatches[t.fins[index]]; const auto& s = t.sources[p.source];
    auto o = object(w); sourceFields(o, s); field(o, "provenance", sourceProvenanceName(s.provenance));
    field(o, "patchId", p.patch + 1); field(o, "splitComponentCount", p.splitComponentCount);
    field(o, "boundaryKind", finBoundaryKindName(p.boundary)); field(o, "boundaryComponentCount", p.boundaryComponents);
    field(o, "area", p.area); field(o, "denominatorArea", p.denominatorArea); field(o, "areaRatio", p.areaRatio);
    field(o, "faceCount", p.faces.size()); field(o, "physicalBoundaryEdgeCount", p.physicalBoundaryEdges.size());
    field(o, "cutBoundaryEdgeCount", p.cutBoundaryEdges.size()); field(o, "facesTruncated", false);
    field(o, "physicalBoundaryEdgesTruncated", false); field(o, "cutBoundaryEdgesTruncated", false);
    arrayField(o, "faces", p.faces.size(), [&](size_t i) { face(w, s.faces[p.faces[i]].reference); });
    arrayField(o, "physicalBoundaryEdgeIds", p.physicalBoundaryEdges.size(), [&](size_t i) { value(w, p.physicalBoundaryEdges[i] + 1); });
    arrayField(o, "cutBoundaryEdgeIds", p.cutBoundaryEdges.size(), [&](size_t i) { value(w, p.cutBoundaryEdges[i] + 1); }); end(o);
}
inline void duplicate(Writer& w, const DuplicateFinding& f) {
    auto o = object(w); idField(o, "sourceId", f.fileId); field(o, "source", f.source);
    field(o, "provenance", sourceProvenanceName(f.provenance)); field(o, "representativeId", f.members.front().id + 1);
    field(o, "memberCount", f.members.size()); field(o, "membersTruncated", false);
    arrayField(o, "members", f.members.size(), [&](size_t i) {
        auto item = object(w); field(item, "id", f.members[i].id + 1); field(item, "reversed", f.members[i].reversed); end(item);
    }); end(o);
}
inline void degenerate(Writer& w, const DegenerateFinding& f) {
    auto o = object(w); idField(o, "sourceId", f.fileId); idField(o, "partId", f.partId); field(o, "source", f.source);
    field(o, "provenance", sourceProvenanceName(f.provenance)); field(o, "triangleId", f.triangleId + 1);
    // Json's number formatter preserves the existing non-finite -> null policy.
    field(o, "edgeRatio", f.reasons.edgeRatio); field(o, "maximumAngleDegrees", f.reasons.maximumAngleDegrees);
    key(o, "reasons"); auto reasons = object(w);
    field(reasons, "collapsed", f.reasons.collapsed); field(reasons, "needle", f.reasons.needle); field(reasons, "cap", f.reasons.cap);
    end(reasons); end(o);
}
inline void intersection(Writer& w, const IntersectionFinding& f) {
    auto o = object(w); field(o, "source", f.source); field(o, "provenance", sourceProvenanceName(f.provenance));
    field(o, "topologyMode", topologyModeName(f.mode)); key(o, "faces");
    // These two faces were an inline JSON value, not lazy array entries, in the
    // original exporter. Keep entriesWritten's established counting convention.
    append(w, "["); face(w, f.faces[0]); append(w, ","); face(w, f.faces[1]); append(w, "]"); end(o);
}
} // namespace woby::analysis_export_detail
