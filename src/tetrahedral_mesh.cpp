#include "tetrahedral_mesh.h"
#include "analysis_index.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace woby {
namespace {
constexpr std::array<std::array<uint32_t, 6>, 4> faces{{
    {0,1,3,4,8,7}, {1,2,3,5,9,8}, {2,0,3,6,7,9}, {0,2,1,6,5,4}}};
struct BoundaryFace {
    std::array<uint32_t, 6> nodes{};
    uint32_t group = 0, count = 3;
    bool reverse = false, internal = false, parity = false;
};
double signedVolume(const Coordinate& a, const Coordinate& b, const Coordinate& c, const Coordinate& d)
{
    Coordinate u{}, v{}, w{};
    for (size_t i = 0; i < 3; ++i) { u[i] = b[i]-a[i]; v[i] = c[i]-a[i]; w[i] = d[i]-a[i]; }
    return (u[1]*v[2]-u[2]*v[1])*w[0] + (u[2]*v[0]-u[0]*v[2])*w[1] + (u[0]*v[1]-u[1]*v[0])*w[2];
}
std::array<uint32_t, 6> faceKey(const BoundaryFace& face)
{
    std::array<uint32_t, 6> key{face.nodes[0],face.nodes[1],face.nodes[2],UINT32_MAX,UINT32_MAX,UINT32_MAX};
    std::sort(key.begin(), key.begin()+3);
    if (face.count == 6) {
        // Associate each mid-edge node with its sorted corner pair.
        for (size_t edge = 0; edge < 3; ++edge) {
            auto a = face.nodes[edge], b = face.nodes[(edge+1)%3];
            if (a > b) { std::swap(a,b); }
            const size_t slot = a == key[0] ? (b == key[1] ? 3u : 4u) : 5u;
            key[slot] = face.nodes[3+edge];
        }
    }
    return key;
}
}

void appendTetrahedralBoundary(MeshGeometry& geometry, const ModelLoadProgressCallback& progress)
{
    AnalysisIndex<uint32_t, 6> keys;
    std::vector<BoundaryFace> boundary;
    // Bound allocation and worst-case output before expanding the cells.
    constexpr uint64_t maxBytes = 4ull * 1024 * 1024 * 1024;
    if (geometry.tetrahedra.size() > maxBytes / (4 * (sizeof(BoundaryFace) + 64))) {
        throw std::runtime_error("Tetrahedral boundary exceeds supported size.");
    }
    for (size_t i = 0; i < geometry.tetrahedra.size(); ++i) {
        if (i % 4096 == 0) { reportModelLoadProgress(progress, ModelLoadStage::triangulating, i, geometry.tetrahedra.size()); }
        const auto& cell = geometry.tetrahedra[i];
        if ((cell.nodeCount != 4 && cell.nodeCount != 10) || cell.groupIndex >= geometry.nodes.size()) {
            throw std::runtime_error("Tetrahedra require 4 or 10 nodes and a valid group.");
        }
        for (size_t n = 0; n < cell.nodeCount; ++n) {
            if (cell.nodes[n] >= geometry.precisePositions.size()) { throw std::runtime_error("Tetrahedral node is outside the vertex buffer."); }
            for (size_t p = 0; p < n; ++p) {
                if (cell.nodes[n] == cell.nodes[p]) { throw std::runtime_error("Tetrahedral nodes must be distinct."); }
            }
            if (!finiteCoordinate(geometry.precisePositions[cell.nodes[n]])) { throw std::runtime_error("Invalid tetrahedral position."); }
        }
        const auto volume = signedVolume(geometry.precisePositions[cell.nodes[0]], geometry.precisePositions[cell.nodes[1]],
            geometry.precisePositions[cell.nodes[2]], geometry.precisePositions[cell.nodes[3]]);
        if (!std::isfinite(volume) || volume == 0) { throw std::runtime_error("Tetrahedral corner volume must be nonzero and finite."); }
        for (const auto& local : faces) {
            BoundaryFace face;
            face.group = cell.groupIndex; face.count = cell.nodeCount == 10 ? 6u : 3u; face.reverse = volume < 0;
            for (size_t n = 0; n < face.count; ++n) { face.nodes[n] = cell.nodes[local[n]]; }
            const auto& n = face.nodes;
            face.parity = ((n[0] > n[1]) ^ (n[0] > n[2]) ^ (n[1] > n[2])) ^ face.reverse;
            const auto id = analysisIndex(keys, faceKey(face));
            if (id == boundary.size()) { boundary.push_back(face); }
            else {
                auto& previous = boundary[id];
                if (previous.internal || previous.parity == face.parity) {
                    throw std::runtime_error("Tetrahedra have duplicate, overlapping or non-manifold faces.");
                }
                previous.internal = true;
            }
        }
    }
    std::vector<uint64_t> counts(geometry.nodes.size(), 0);
    for (const auto& face : boundary) { if (!face.internal) { counts[face.group] += face.count == 6 ? 12u : 3u; } }
    uint64_t total = geometry.indices.size();
    for (size_t i = 0; i < counts.size(); ++i) {
        if (!counts[i]) { continue; }
        auto& node = geometry.nodes[i];
        if (node.indexCount || node.lineIndexCount || node.pointIndexCount) { throw std::runtime_error("Tetrahedra require separate groups."); }
        if (total + counts[i] > UINT32_MAX || (total + counts[i]) * sizeof(uint32_t) > maxBytes) {
            throw std::runtime_error("Tetrahedral triangle buffer exceeds supported size.");
        }
        node.indexOffset = static_cast<uint32_t>(total); node.indexCount = static_cast<uint32_t>(counts[i]); total += counts[i];
    }
    geometry.indices.resize(static_cast<size_t>(total));
    std::vector<uint32_t> next;
    for (const auto& node : geometry.nodes) { next.push_back(node.indexOffset); }
    constexpr std::array<std::array<size_t, 3>, 4> triangles{{{0,3,5},{3,1,4},{5,4,2},{3,4,5}}};
    for (size_t i = 0; i < boundary.size(); ++i) {
        if (i % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::triangulating, i, boundary.size()); }
        const auto& face = boundary[i];
        if (face.internal) { continue; }
        const auto emit = [&](size_t a, size_t b, size_t c) {
            if (face.reverse) { std::swap(b,c); }
            auto& offset = next[face.group];
            geometry.indices[offset++] = face.nodes[a]; geometry.indices[offset++] = face.nodes[b]; geometry.indices[offset++] = face.nodes[c];
        };
        if (face.count == 3) { emit(0,1,2); }
        else { for (const auto& triangle : triangles) { emit(triangle[0],triangle[1],triangle[2]); } }
    }
}
}
