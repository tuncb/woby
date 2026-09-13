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
        result.sources.push_back(std::move(source));
    }
    for (const auto& [file, triangle, part] : windingFaces) { canceled(stop); result.windingFaces.push_back({file, part, triangle}); }
    return result;
}
} // namespace woby
