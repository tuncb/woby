#include "mesh_topology.h"
#include "mesh_intersections.h"
#include "analysis_index.h"
#include "parallel_work.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace woby {
namespace {
using Point = std::array<double, 3>;
void canceled(const std::stop_token& stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
}
void inspectVertexLinks(MeshTopology& result, const SourceTopology& source, std::stop_token stop)
{
    // Reuse a sparse scratch graph across vertices. A vertex link normally has
    // only a handful of nodes; constructing map/set nodes for each was costly.
    std::vector<size_t> degree(source.vertices.size()), parent(source.vertices.size()), nodes;
    const auto root = [&](size_t v) {
        while (parent[v] != v) { parent[v] = parent[parent[v]]; v = parent[v]; }
        return v;
    };
    for (size_t v = 0; v < source.vertices.size(); ++v) {
        canceled(stop);
        const auto& vertex = source.vertices[v];
        bool excluded = false;
        for (const auto e : vertex.edges) {
            canceled(stop);
            if (source.edges[e].incidentFaces.size() > 2) { excluded = true; break; }
        }
        if (excluded) { ++result.excludedNonManifoldEdgeVertices; continue; }
        if (vertex.faces.empty()) { continue; }
        nodes.clear();
        size_t components = 0;
        for (const auto& edge : vertex.link) {
            canceled(stop);
            for (const auto node : edge) {
                if (degree[node]++ == 0) { parent[node] = node; nodes.push_back(node); ++components; }
            }
            const auto a = root(edge[0]), b = root(edge[1]);
            if (a != b) { parent[a] = b; --components; }
        }
        size_t ends = 0;
        bool degreesValid = true;
        for (const auto node : nodes) {
            canceled(stop);
            ends += degree[node] == 1;
            degreesValid &= degree[node] == 1 || degree[node] == 2;
            degree[node] = 0;
        }
        const bool boundary = !vertex.boundaryEdges.empty();
        const bool valid = degreesValid && components == 1 && ends == (boundary ? 2u : 0u)
            && (!boundary || vertex.boundaryEdges.size() == 2);
        if (!valid) { result.nonManifoldVertices.push_back({result.sources.size(), v, components}); }
    }
}
void inspectBoundaries(MeshTopology& result, const SourceTopology& source, std::stop_token stop)
{
    std::vector<std::vector<size_t>> componentEdges(source.components.size());
    for (size_t e = 0; e < source.edges.size(); ++e) {
        canceled(stop);
        const auto& edge = source.edges[e];
        if (edge.incidentFaces.size() == 1) {
            componentEdges[source.faces[edge.incidentFaces[0].face].component].push_back(e);
        }
    }
    for (size_t component = 0; component < componentEdges.size(); ++component) {
        canceled(stop);
        if (componentEdges[component].empty()) { continue; }
        Point minimum{}, maximum{};
        bool first = true;
        for (const auto f : source.components[component]) {
            canceled(stop);
            for (const auto v : source.faces[f].vertices) {
                const auto& p = source.vertices[v].position;
                if (first) { minimum = maximum = p; first = false; }
                for (size_t k = 0; k < 3; ++k) { minimum[k] = std::min(minimum[k], p[k]); maximum[k] = std::max(maximum[k], p[k]); }
            }
        }
        const double diagonal = std::hypot(maximum[0]-minimum[0], maximum[1]-minimum[1], maximum[2]-minimum[2]);
        std::map<size_t, std::vector<size_t>> adjacency;
        for (const auto e : componentEdges[component]) {
            canceled(stop);
            for (const auto v : source.edges[e].vertices) { adjacency[v].push_back(e); }
        }
        std::set<size_t> seen;
        for (const auto& [seed, incident] : adjacency) {
            canceled(stop); (void)incident;
            if (!seen.insert(seed).second) { continue; }
            TopologyBoundary finding;
            finding.source = result.sources.size(); finding.component = component;
            finding.vertices.push_back(seed);
            finding.minimum = finding.maximum = source.vertices[seed].position;
            std::set<size_t> edges;
            size_t ends = 0;
            for (size_t head = 0; head < finding.vertices.size(); ++head) {
                canceled(stop);
                const auto v = finding.vertices[head];
                const auto& p = source.vertices[v].position;
                for (size_t k = 0; k < 3; ++k) { finding.minimum[k] = std::min(finding.minimum[k], p[k]); finding.maximum[k] = std::max(finding.maximum[k], p[k]); }
                const auto& uses = adjacency.at(v);
                ends += uses.size() == 1;
                if (uses.size() > 2) { finding.kind = BoundaryKind::branched; }
                for (const auto e : uses) {
                    edges.insert(e);
                    for (const auto next : source.edges[e].vertices) {
                        if (seen.insert(next).second) { finding.vertices.push_back(next); }
                    }
                }
            }
            finding.edges.assign(edges.begin(), edges.end());
            if (finding.kind != BoundaryKind::branched && ends) { finding.kind = BoundaryKind::open; }
            if (finding.kind == BoundaryKind::loop) {
                // Canonical start and direction; every vertex has degree two here.
                finding.vertices.clear(); finding.edges.clear();
                size_t vertex = seed, previous = std::numeric_limits<size_t>::max();
                do {
                    canceled(stop);
                    finding.vertices.push_back(vertex);
                    const auto& uses = adjacency.at(vertex);
                    const size_t edge = uses[0] == previous ? uses[1] : uses[0];
                    finding.edges.push_back(edge);
                    const auto& endpoints = source.edges[edge].vertices;
                    vertex = endpoints[0] == vertex ? endpoints[1] : endpoints[0];
                    previous = edge;
                } while (vertex != seed);
            }
            finding.diagonal = std::hypot(finding.maximum[0]-finding.minimum[0], finding.maximum[1]-finding.minimum[1], finding.maximum[2]-finding.minimum[2]);
            finding.componentDiagonal = diagonal;
            finding.ratioAvailable = diagonal > 0 && std::isfinite(diagonal);
            if (finding.ratioAvailable) { finding.sizeRatio = finding.diagonal / diagonal; }
            result.boundaryRegions.push_back(std::move(finding));
        }
    }
}
void inspectFinPatches(MeshTopology& result, const SourceTopology& source, std::stop_token stop)
{
    // Physical boundaries determine the classification. A cut edge is retained as
    // provenance, but must not close the open rim of an attached fin artificially.
    const size_t first = result.finPatches.size();
    std::vector<bool> seen(source.faces.size());
    const bool manifold = std::none_of(source.edges.begin(), source.edges.end(), [](const auto& edge) {
        return edge.incidentFaces.size() > 2;
    });
    std::vector<std::vector<size_t>> componentFaces, componentBoundaries;
    if (manifold) {
        componentFaces.resize(source.components.size()); componentBoundaries.resize(source.components.size());
        for (size_t i = 0; i < source.components.size(); ++i) { componentFaces[i].reserve(source.components[i].size()); }
        for (size_t f = 0; f < source.faces.size(); ++f) {
            canceled(stop); componentFaces[source.faces[f].component].push_back(f);
        }
        for (size_t e = 0; e < source.edges.size(); ++e) {
            canceled(stop);
            if (source.edges[e].incidentFaces.size() == 1) {
                componentBoundaries[source.faces[source.edges[e].incidentFaces[0].face].component].push_back(e);
            }
        }
    }
    double denominator = 0;
    bool areasAvailable = true;
    for (size_t seed = 0; seed < source.faces.size(); ++seed) {
        canceled(stop);
        if (seen[seed]) { continue; }
        TopologyFinPatch patch;
        patch.source = result.sources.size(); patch.patch = result.finPatches.size() - first;
        patch.faces.push_back(seed); seen[seed] = true;
        std::set<size_t> physical, cuts;
        if (manifold) {
            const auto component = source.faces[seed].component;
            patch.faces = source.components[component];
            physical.insert(componentBoundaries[component].begin(), componentBoundaries[component].end());
            for (const auto f : patch.faces) { seen[f] = true; }
        }
        for (size_t head = 0; head < patch.faces.size(); ++head) {
            canceled(stop);
            const auto& face = source.faces[patch.faces[head]];
            const auto& a = source.vertices[face.vertices[0]].position;
            const auto& b = source.vertices[face.vertices[1]].position;
            const auto& c = source.vertices[face.vertices[2]].position;
            Point u{}, v{};
            for (size_t k = 0; k < 3; ++k) { u[k] = b[k]-a[k]; v[k] = c[k]-a[k]; }
            patch.area += .5 * std::hypot(u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]);
            if (manifold) { continue; } // The edge-connected component is already the fin patch.
            for (const auto e : face.edges) {
                canceled(stop);
                const auto& uses = source.edges[e].incidentFaces;
                if (uses.size() == 1) { physical.insert(e); }
                else if (uses.size() > 2) { cuts.insert(e); }
                else {
                    for (const auto& use : uses) {
                        if (!seen[use.face]) { seen[use.face] = true; patch.faces.push_back(use.face); }
                    }
                }
            }
        }
        if (manifold) { patch.faces = std::move(componentFaces[source.faces[seed].component]); }
        else { std::sort(patch.faces.begin(), patch.faces.end(), [&](size_t a, size_t b) { canceled(stop); return a < b; }); }
        patch.physicalBoundaryEdges.assign(physical.begin(), physical.end());
        patch.cutBoundaryEdges.assign(cuts.begin(), cuts.end());
        std::map<size_t, std::vector<size_t>> graph;
        for (const auto e : physical) {
            canceled(stop);
            const auto& v = source.edges[e].vertices;
            graph[v[0]].push_back(v[1]); graph[v[1]].push_back(v[0]);
        }
        bool open = false, branched = false;
        std::set<size_t> visited;
        for (const auto& [vertex, neighbors] : graph) {
            canceled(stop);
            open |= neighbors.size() == 1; branched |= neighbors.size() > 2;
            if (!visited.insert(vertex).second) { continue; }
            ++patch.boundaryComponents;
            std::vector<size_t> queue{vertex};
            for (size_t head = 0; head < queue.size(); ++head) {
                canceled(stop);
                for (const auto next : graph.at(queue[head])) {
                    if (visited.insert(next).second) { queue.push_back(next); }
                }
            }
        }
        if (!physical.empty()) {
            // Exact topology may retain triangles whose area underflows double.
            // Keep other detectors usable, but never claim a clean fin result.
            areasAvailable &= std::isfinite(patch.area) && patch.area > 0;
            patch.boundary = branched ? FinBoundaryKind::branched : open ? FinBoundaryKind::open
                : patch.boundaryComponents == 1 ? FinBoundaryKind::simpleLoop : FinBoundaryKind::multipleLoops;
            denominator = std::max(denominator, patch.area);
        }
        result.finPatches.push_back(std::move(patch));
    }
    const size_t count = result.finPatches.size() - first;
    if (count >= 2 && !areasAvailable) { ++result.unavailableFinAreaSources; }
    for (size_t i = first; i < result.finPatches.size(); ++i) {
        canceled(stop);
        auto& patch = result.finPatches[i];
        patch.splitComponentCount = count; patch.denominatorArea = denominator;
        if (denominator > 0) { patch.areaRatio = patch.area / denominator; }
        patch.candidate = areasAvailable && count >= 2 && patch.boundary != FinBoundaryKind::none
            && patch.boundary != FinBoundaryKind::simpleLoop;
    }
}
SourceTopology buildSourceTopology(const DuplicateSource& source, TopologyMode mode, std::stop_token stop)
{
    SourceTopology result;
    result.fileId = source.fileId;
    result.source = source.name;
    result.mode = mode;
    if (!source.data) { result.available = false; return result; }
    const auto& data = *source.data;
    result.provenance = data.provenance;
    if (mode == TopologyMode::automatic) {
        result.mode = data.provenance == SourceProvenance::stlCorners ? TopologyMode::exactPosition : TopologyMode::originalIndex;
    }
    if (data.indices.size() % 3) { throw std::invalid_argument("Invalid source triangle indices."); }
    for (const auto& point : data.points) {
        canceled(stop);
        for (const auto value : point) { if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite source coordinate."); } }
    }
    for (const auto index : data.indices) {
        canceled(stop);
        if (index >= data.points.size()) { throw std::invalid_argument("Invalid source point index."); }
    }
    std::vector<const SourcePartInstance*> parts;
    for (const auto& part : source.parts) {
        canceled(stop);
        if (part.firstIndex % 3 || part.indexCount % 3 || part.firstIndex > data.indices.size()
            || part.indexCount > data.indices.size() - part.firstIndex) { throw std::invalid_argument("Invalid source part range."); }
        for (const auto value : part.transform) { if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite source transform."); } }
        parts.push_back(&part);
    }
    if (result.mode == TopologyMode::originalIndex && data.provenance == SourceProvenance::stlCorners) {
        result.available = false;
        return result;
    }
    std::sort(parts.begin(), parts.end(), [&](const auto* a, const auto* b) {
        canceled(stop);
        return std::tie(a->partId, a->firstIndex, a->indexCount, a->transform)
            < std::tie(b->partId, b->firstIndex, b->indexCount, b->transform);
    });
    AnalysisIndex<size_t, 2> originalVertices;
    AnalysisIndex<double, 3> exactVertices;
    AnalysisIndex<float, 16> transforms;
    AnalysisIndex<uint64_t, 3> largeReferenceSets;
    const bool uniformTransform = !parts.empty() && std::all_of(parts.begin(), parts.end(), [&](const auto* part) {
        return part->transform == parts.front()->transform;
    });
    const size_t missingVertex = std::numeric_limits<size_t>::max();
    std::vector<size_t> originalIds;
    if (result.mode == TopologyMode::originalIndex && uniformTransform) { originalIds.assign(data.points.size(), missingVertex); }
    else if (result.mode == TopologyMode::originalIndex) { reserveAnalysisIndex(originalVertices, data.points.size()); }
    else { reserveAnalysisIndex(exactVertices, data.points.size()); }
    std::vector<size_t> visited(data.indices.size() / 3, 0);
    size_t generation = 0;
    uint64_t previousPart = 0;
    result.vertices.reserve(data.points.size());
    result.faces.reserve(data.indices.size() / 3);
    for (const auto* part : parts) {
        if (generation == 0 || previousPart != part->partId) { ++generation; previousPart = part->partId; }
        const auto transform = analysisIndex(transforms, part->transform);
        for (size_t i = part->firstIndex; i < part->firstIndex + part->indexCount; i += 3) {
            canceled(stop);
            if (visited[i/3] == generation) { continue; }
            visited[i/3] = generation;
            std::array<Point, 3> points{};
            for (size_t j = 0; j < 3; ++j) {
                const auto& p = data.points[data.indices[i+j]];
                const auto& m = part->transform;
                for (size_t k = 0; k < 3; ++k) {
                    const double value = m[k]*p[0] + m[k+4]*p[1] + m[k+8]*p[2] + m[k+12];
                    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                        throw std::invalid_argument("Topology requires finite renderable world coordinates.");
                    }
                    points[j][k] = value;
                }
            }
            // Needle/cap thresholds must never change connectivity.
            if (exactTriangleCollapsed(points)) { ++result.excludedCollapsedFaces; continue; }
            TopologyFace face;
            face.reference = {source.fileId, part->partId, i/3};
            for (size_t j = 0; j < 3; ++j) {
                const auto id = data.indices[i+j];
                size_t vertex = 0;
                if (result.mode == TopologyMode::originalIndex) {
                    if (uniformTransform) {
                        if (originalIds[id] == missingVertex) { originalIds[id] = result.vertices.size(); }
                        vertex = originalIds[id];
                    } else { vertex = analysisIndex(originalVertices, std::array<size_t, 2>{id, transform}); }
                } else {
                    vertex = analysisIndex(exactVertices, points[j]);
                }
                if (vertex == result.vertices.size()) { TopologyVertex value; value.position = points[j]; result.vertices.push_back(std::move(value)); }
                auto& references = result.vertices[vertex].references;
                const auto matches = [&](const auto& ref) { return ref.partId == part->partId && ref.pointId == id; };
                if (references.empty() || !matches(references.back())) {
                    bool found;
                    if (references.size() < 8) { found = std::any_of(references.begin(), references.end(), matches); }
                    else {
                        // Exact-position welding can merge arbitrarily many
                        // source IDs. Avoid a quadratic scan at those vertices.
                        if (references.size() == 8) {
                            for (const auto& ref : references) {
                                (void)analysisIndex(largeReferenceSets, std::array<uint64_t,3>{vertex, ref.partId, ref.pointId});
                            }
                        }
                        const auto count = largeReferenceSets.keys.size();
                        found = analysisIndex(largeReferenceSets, std::array<uint64_t,3>{vertex, part->partId, id}) < count;
                    }
                    if (!found) { references.push_back({part->partId, id}); }
                }
                face.vertices[j] = vertex;
            }
            result.faces.push_back(face);
        }
    }
    const auto faceOrder = [&](const auto& a, const auto& b) {
        canceled(stop);
        return std::tie(a.reference.triangleId, a.reference.partId) < std::tie(b.reference.triangleId, b.reference.partId);
    };
    if (!std::is_sorted(result.faces.begin(), result.faces.end(), faceOrder)) {
        std::sort(result.faces.begin(), result.faces.end(), faceOrder);
    }
    auto incidence = std::make_shared<TopologyIncidence>();
    const size_t corners = result.faces.size() * 3;
    incidence->vertexFaces.resize(corners);
    incidence->vertexLinks.resize(corners);
    incidence->edgeUses.resize(corners);
    std::vector<size_t> degrees(result.vertices.size()), starts(result.vertices.size() + 1);
    for (const auto& face : result.faces) {
        canceled(stop);
        for (size_t k = 0; k < 3; ++k) {
            ++degrees[face.vertices[k]];
            ++starts[std::min(face.vertices[k], face.vertices[(k+1)%3]) + 1];
        }
    }
    size_t offset = 0;
    for (size_t v = 0; v < result.vertices.size(); ++v) {
        auto& vertex = result.vertices[v];
        vertex.faces = std::span(incidence->vertexFaces).subspan(offset, degrees[v]);
        vertex.link = std::span(incidence->vertexLinks).subspan(offset, degrees[v]);
        offset += degrees[v];
        starts[v+1] += starts[v];
    }
    // Counting-sort by the smaller endpoint. Each small vertex bucket is then
    // sorted by the other endpoint and face, retaining the old canonical order.
    std::vector<std::array<size_t, 2>> entries(corners);
    auto cursor = starts;
    std::fill(degrees.begin(), degrees.end(), 0);
    for (size_t f = 0; f < result.faces.size(); ++f) {
        canceled(stop);
        const auto& ids = result.faces[f].vertices;
        for (size_t k = 0; k < 3; ++k) {
            const auto a = ids[k], b = ids[(k+1)%3];
            entries[cursor[std::min(a,b)]++] = {std::max(a,b), f*3+k};
            const auto position = static_cast<size_t>(result.vertices[a].faces.data() - incidence->vertexFaces.data()) + degrees[a]++;
            incidence->vertexFaces[position] = f;
            incidence->vertexLinks[position] = {b, ids[(k+2)%3]};
        }
    }
    std::fill(degrees.begin(), degrees.end(), 0);
    std::vector<size_t> boundaryDegrees(result.vertices.size());
    result.edges.reserve(corners);
    for (size_t a = 0; a < result.vertices.size(); ++a) {
        canceled(stop);
        std::sort(entries.begin() + static_cast<ptrdiff_t>(starts[a]), entries.begin() + static_cast<ptrdiff_t>(starts[a+1]));
        for (size_t begin = starts[a]; begin < starts[a+1];) {
            const auto b = entries[begin][0];
            size_t end = begin + 1;
            while (end < starts[a+1] && entries[end][0] == b) { ++end; }
            const auto index = result.edges.size();
            for (size_t i = begin; i < end; ++i) {
                canceled(stop);
                const auto corner = entries[i][1];
                auto& face = result.faces[corner/3];
                incidence->edgeUses[i] = {corner/3, face.vertices[corner%3] == a};
                face.edges[corner%3] = index;
            }
            const auto uses = std::span<const TopologyEdgeUse>(incidence->edgeUses).subspan(begin, end-begin);
            ++degrees[a]; ++degrees[b];
            if (uses.size() == 1) { ++boundaryDegrees[a]; ++boundaryDegrees[b]; }
            result.edges.push_back({{a,b}, uses, uses.size() == 2 && uses[0].forward == uses[1].forward, false});
            begin = end;
        }
    }
    incidence->vertexEdges.resize(result.edges.size() * 2);
    size_t boundaryCount = 0;
    for (const auto count : boundaryDegrees) { boundaryCount += count; }
    incidence->boundaryEdges.resize(boundaryCount);
    offset = 0; size_t boundaryOffset = 0;
    for (size_t v = 0; v < result.vertices.size(); ++v) {
        result.vertices[v].edges = std::span(incidence->vertexEdges).subspan(offset, degrees[v]);
        result.vertices[v].boundaryEdges = std::span(incidence->boundaryEdges).subspan(boundaryOffset, boundaryDegrees[v]);
        offset += degrees[v]; boundaryOffset += boundaryDegrees[v];
    }
    std::fill(degrees.begin(), degrees.end(), 0);
    std::fill(boundaryDegrees.begin(), boundaryDegrees.end(), 0);
    for (size_t e = 0; e < result.edges.size(); ++e) {
        canceled(stop);
        for (const auto v : result.edges[e].vertices) {
            const auto position = static_cast<size_t>(result.vertices[v].edges.data() - incidence->vertexEdges.data()) + degrees[v]++;
            incidence->vertexEdges[position] = e;
            if (result.edges[e].incidentFaces.size() == 1) {
                const auto boundaryPosition = static_cast<size_t>(result.vertices[v].boundaryEdges.data() - incidence->boundaryEdges.data()) + boundaryDegrees[v]++;
                incidence->boundaryEdges[boundaryPosition] = e;
            }
        }
    }
    result.incidence = std::move(incidence);
    // Edge-connected components include non-manifold edges, without an O(n^2)
    // neighbor clique when many faces meet at one edge.
    std::vector<bool> seenFaces(result.faces.size()), seenEdges(result.edges.size());
    for (size_t seed = 0; seed < result.faces.size(); ++seed) {
        canceled(stop);
        if (seenFaces[seed]) { continue; }
        std::vector<size_t> component{seed};
        seenFaces[seed] = true;
        for (size_t head = 0; head < component.size(); ++head) {
            canceled(stop);
            auto& face = result.faces[component[head]];
            face.component = result.components.size();
            for (const auto edge : face.edges) {
                if (seenEdges[edge]) { continue; }
                seenEdges[edge] = true;
                for (const auto& use : result.edges[edge].incidentFaces) {
                    canceled(stop);
                    if (!seenFaces[use.face]) { seenFaces[use.face] = true; component.push_back(use.face); }
                }
            }
        }
        result.components.push_back(std::move(component));
    }
    // Relative flip constraints on manifold edges reveal non-orientable regions.
    // Contradiction edges are deterministic witnesses, not repair instructions.
    if (std::none_of(result.edges.begin(), result.edges.end(), [](const auto& edge) { return edge.windingConflict; })) {
        return result; // Every relative constraint is zero, so all faces agree.
    }
    std::vector<int> flips(result.faces.size(), -1);
    for (size_t seed = 0; seed < result.faces.size(); ++seed) {
        canceled(stop);
        if (flips[seed] != -1) { continue; }
        std::vector<size_t> queue{seed};
        flips[seed] = 0;
        for (size_t head = 0; head < queue.size(); ++head) {
            canceled(stop);
            const auto face = queue[head];
            for (const auto index : result.faces[face].edges) {
                auto& edge = result.edges[index];
                if (edge.incidentFaces.size() != 2) { continue; }
                const auto other = edge.incidentFaces[0].face == face ? edge.incidentFaces[1].face : edge.incidentFaces[0].face;
                const int expected = flips[face] ^ static_cast<int>(edge.windingConflict);
                if (flips[other] == -1) { flips[other] = expected; queue.push_back(other); }
                else if (flips[other] != expected) { edge.orientationContradiction = true; }
            }
        }
    }
    return result;
}
}
TopologyMode normalizedTopologyMode(TopologyMode mode)
{
    return mode == TopologyMode::originalIndex || mode == TopologyMode::exactPosition ? mode : TopologyMode::automatic;
}
const char* topologyModeName(TopologyMode mode)
{
    switch (mode) {
    case TopologyMode::automatic: return "automatic";
    case TopologyMode::originalIndex: return "original_index";
    case TopologyMode::exactPosition: return "exact_position";
    }
    return "automatic";
}
TopologyMode parseTopologyMode(const std::string& name)
{
    if (name == "automatic") { return TopologyMode::automatic; }
    if (name == "original_index") { return TopologyMode::originalIndex; }
    if (name == "exact_position") { return TopologyMode::exactPosition; }
    throw std::invalid_argument("Unknown topology mode. Use automatic, original_index, or exact_position.");
}
const char* topologyStatus(const MeshTopology& topology)
{
    if (topology.unavailableSources) { return topology.availableSources ? "partial" : "unavailable"; }
    return "complete";
}
const char* finStatus(const MeshTopology& topology)
{
    if (topology.unavailableSources || topology.unavailableFinAreaSources) {
        return topology.availableSources > topology.unavailableFinAreaSources ? "partial" : "unavailable";
    }
    return "complete";
}
bool sameTopologyInspectionFilters(const TopologyInspectionSettings& a, const TopologyInspectionSettings& b)
{
    return a.holes == b.holes && a.nonManifoldVertices == b.nonManifoldVertices
        && a.fins == b.fins && a.finMaxAreaRatio == b.finMaxAreaRatio
        && a.holeSizeRatioTolerance == b.holeSizeRatioTolerance;
}
TopologyInspectionSettings normalizedTopologyInspectionSettings(TopologyInspectionSettings settings)
{
    settings.holeSizeRatioTolerance = std::isfinite(settings.holeSizeRatioTolerance)
        ? std::max(0.0f, settings.holeSizeRatioTolerance) : .05f;
    settings.finMaxAreaRatio = std::isfinite(settings.finMaxAreaRatio) ? std::max(0.0f, settings.finMaxAreaRatio) : 1.0f;
    return settings;
}
const char* boundaryKindName(BoundaryKind kind)
{
    switch (kind) {
    case BoundaryKind::loop: return "loop";
    case BoundaryKind::open: return "open";
    case BoundaryKind::branched: return "branched";
    }
    return "branched";
}
const char* finBoundaryKindName(FinBoundaryKind kind)
{
    switch (kind) {
    case FinBoundaryKind::none: return "none";
    case FinBoundaryKind::simpleLoop: return "simple_loop";
    case FinBoundaryKind::multipleLoops: return "multiple_loops";
    case FinBoundaryKind::open: return "open";
    case FinBoundaryKind::branched: return "branched";
    }
    return "none";
}
bool filterTopologyFindings(MeshTopology& topology, TopologyInspectionSettings settings, std::stop_token stop)
{
    canceled(stop);
    settings = normalizedTopologyInspectionSettings(settings);
    const bool changed = topology.inspection.holeSizeRatioTolerance != settings.holeSizeRatioTolerance
        || topology.inspection.finMaxAreaRatio != settings.finMaxAreaRatio;
    topology.inspection = settings;
    topology.holes.clear();
    for (size_t i = 0; i < topology.boundaryRegions.size(); ++i) {
        canceled(stop);
        const auto& boundary = topology.boundaryRegions[i];
        if (boundary.kind == BoundaryKind::loop && boundary.ratioAvailable && boundary.sizeRatio <= settings.holeSizeRatioTolerance) {
            topology.holes.push_back(i);
        }
    }
    topology.fins.clear();
    for (size_t i = 0; i < topology.finPatches.size(); ++i) {
        canceled(stop);
        const auto& patch = topology.finPatches[i];
        if (patch.candidate && patch.areaRatio <= settings.finMaxAreaRatio) { topology.fins.push_back(i); }
    }
    return changed;
}
MeshTopology buildMeshTopology(const std::vector<DuplicateSource>& sources, TopologyMode mode, std::stop_token stop)
{
    canceled(stop);
    MeshTopology result;
    result.mode = normalizedTopologyMode(mode);
    std::vector<const DuplicateSource*> sorted;
    for (const auto& source : sources) { canceled(stop); sorted.push_back(&source); }
    std::sort(sorted.begin(), sorted.end(), [&](const auto* a, const auto* b) { canceled(stop); return a->fileId < b->fileId; });
    std::set<std::tuple<uint64_t, size_t, uint64_t>> windingFaces;
    for (const auto* input : sorted) {
        canceled(stop);
        auto source = buildSourceTopology(*input, result.mode, stop);
        if (source.available) { ++result.availableSources; } else { ++result.unavailableSources; }
        result.excludedCollapsedFaces += source.excludedCollapsedFaces;
        for (size_t i = 0; i < source.edges.size(); ++i) {
            canceled(stop);
            const auto& edge = source.edges[i];
            const TopologyEdgeFinding finding{result.sources.size(), i};
            if (edge.incidentFaces.size() == 1) { result.boundaries.push_back(finding); }
            if (edge.incidentFaces.size() > 2) { result.nonManifoldEdges.push_back(finding); }
            if (edge.windingConflict || edge.orientationContradiction) {
                result.windingEdges.push_back(finding);
                for (const auto& use : edge.incidentFaces) {
                    const auto& ref = source.faces[use.face].reference;
                    windingFaces.emplace(ref.fileId, ref.triangleId, ref.partId);
                }
            }
            result.orientationContradictions += edge.orientationContradiction;
        }
        parallelAnalysisBatches(3, 1, stop, [&](size_t begin, size_t) {
            if (begin == 0) { inspectVertexLinks(result, source, stop); }
            if (begin == 1) { inspectBoundaries(result, source, stop); }
            if (begin == 2) { inspectFinPatches(result, source, stop); }
        }, source.faces.size() >= 4096 ? 3 : 4096);
        result.sources.push_back(std::move(source));
    }
    for (const auto& [file, triangle, part] : windingFaces) { canceled(stop); result.windingFaces.push_back({file, part, triangle}); }
    (void)filterTopologyFindings(result, {}, stop);
    return result;
}
} // namespace woby
