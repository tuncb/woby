#include "mesh_topology.h"
#include "mesh_degenerates.h"

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
void canceled(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
}
void inspectVertexLinks(MeshTopology& result, const SourceTopology& source, std::stop_token stop)
{
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
        std::map<size_t, std::vector<size_t>> link;
        for (const auto& edge : vertex.link) {
            canceled(stop);
            link[edge[0]].push_back(edge[1]); link[edge[1]].push_back(edge[0]);
        }
        size_t ends = 0, components = 0;
        bool degreesValid = true;
        std::set<size_t> seen;
        for (const auto& [seed, neighbors] : link) {
            canceled(stop);
            ends += neighbors.size() == 1;
            degreesValid = degreesValid && (neighbors.size() == 1 || neighbors.size() == 2);
            if (!seen.insert(seed).second) { continue; }
            ++components;
            std::vector<size_t> queue{seed};
            for (size_t head = 0; head < queue.size(); ++head) {
                canceled(stop);
                for (const auto next : link.at(queue[head])) {
                    if (seen.insert(next).second) { queue.push_back(next); }
                }
            }
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
    std::map<std::pair<uint32_t, std::array<float, 16>>, size_t> originalVertices;
    std::map<Point, size_t> exactVertices;
    std::set<std::pair<uint64_t, size_t>> visited;
    std::set<std::tuple<size_t, uint64_t, size_t>> pointReferences;
    for (const auto* part : parts) {
        for (size_t i = part->firstIndex; i < part->firstIndex + part->indexCount; i += 3) {
            canceled(stop);
            if (!visited.emplace(part->partId, i/3).second) { continue; }
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
            if (classifyDegenerateTriangle(points, {}).collapsed) { ++result.excludedCollapsedFaces; continue; }
            TopologyFace face;
            face.reference = {source.fileId, part->partId, i/3};
            for (size_t j = 0; j < 3; ++j) {
                const auto id = data.indices[i+j];
                size_t vertex = 0;
                bool inserted = false;
                if (result.mode == TopologyMode::originalIndex) {
                    const auto entry = originalVertices.emplace(std::make_pair(id, part->transform), result.vertices.size());
                    vertex = entry.first->second; inserted = entry.second;
                } else {
                    const auto entry = exactVertices.emplace(points[j], result.vertices.size());
                    vertex = entry.first->second; inserted = entry.second;
                }
                if (inserted) { TopologyVertex value; value.position = points[j]; result.vertices.push_back(std::move(value)); }
                if (pointReferences.emplace(vertex, part->partId, id).second) { result.vertices[vertex].references.push_back({part->partId, id}); }
                face.vertices[j] = vertex;
            }
            result.faces.push_back(face);
        }
    }
    std::sort(result.faces.begin(), result.faces.end(), [&](const auto& a, const auto& b) {
        canceled(stop);
        return std::tie(a.reference.triangleId, a.reference.partId) < std::tie(b.reference.triangleId, b.reference.partId);
    });
    std::map<std::array<size_t, 2>, std::vector<TopologyEdgeUse>> edges;
    for (size_t f = 0; f < result.faces.size(); ++f) {
        canceled(stop);
        const auto& ids = result.faces[f].vertices;
        for (size_t k = 0; k < 3; ++k) {
            const auto a = ids[k], b = ids[(k+1)%3];
            edges[{std::min(a, b), std::max(a, b)}].push_back({f, a < b});
            result.vertices[a].faces.push_back(f);
            result.vertices[a].link.push_back({ids[(k+1)%3], ids[(k+2)%3]});
        }
    }
    for (auto& [key, uses] : edges) {
        canceled(stop);
        const size_t index = result.edges.size();
        for (const auto vertex : key) {
            result.vertices[vertex].edges.push_back(index);
            if (uses.size() == 1) { result.vertices[vertex].boundaryEdges.push_back(index); }
        }
        for (const auto& use : uses) {
            canceled(stop);
            auto& face = result.faces[use.face];
            for (size_t k = 0; k < 3; ++k) {
                const auto a = face.vertices[k], b = face.vertices[(k+1)%3];
                if (key == std::array<size_t, 2>{std::min(a, b), std::max(a, b)}) { face.edges[k] = index; }
            }
        }
        const bool conflict = uses.size() == 2 && uses[0].forward == uses[1].forward;
        result.edges.push_back({key, std::move(uses), conflict, false});
    }
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
bool sameTopologyInspectionFilters(const TopologyInspectionSettings& a, const TopologyInspectionSettings& b)
{
    return a.holes == b.holes && a.nonManifoldVertices == b.nonManifoldVertices
        && a.holeSizeRatioTolerance == b.holeSizeRatioTolerance;
}
TopologyInspectionSettings normalizedTopologyInspectionSettings(TopologyInspectionSettings settings)
{
    settings.holeSizeRatioTolerance = std::isfinite(settings.holeSizeRatioTolerance)
        ? std::max(0.0f, settings.holeSizeRatioTolerance) : .05f;
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
bool filterTopologyFindings(MeshTopology& topology, TopologyInspectionSettings settings, std::stop_token stop)
{
    canceled(stop);
    settings = normalizedTopologyInspectionSettings(settings);
    const bool changed = topology.inspection.holeSizeRatioTolerance != settings.holeSizeRatioTolerance;
    topology.inspection = settings;
    topology.holes.clear();
    for (size_t i = 0; i < topology.boundaryRegions.size(); ++i) {
        canceled(stop);
        const auto& boundary = topology.boundaryRegions[i];
        if (boundary.kind == BoundaryKind::loop && boundary.ratioAvailable && boundary.sizeRatio <= settings.holeSizeRatioTolerance) {
            topology.holes.push_back(i);
        }
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
        inspectVertexLinks(result, source, stop);
        inspectBoundaries(result, source, stop);
        result.sources.push_back(std::move(source));
    }
    for (const auto& [file, triangle, part] : windingFaces) { canceled(stop); result.windingFaces.push_back({file, part, triangle}); }
    (void)filterTopologyFindings(result, {}, stop);
    return result;
}
} // namespace woby
