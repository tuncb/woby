#include "mesh_intersections.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <tuple>

namespace woby {
namespace {
using Exact = boost::multiprecision::cpp_rational;
using Point = std::array<Exact, 3>;
using Triangle = std::array<Point, 3>;
using Position = std::array<double, 3>;
void canceled(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
}
Point subtract(const Point& a, const Point& b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
Point cross(const Point& a, const Point& b)
{
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
Exact dot(const Point& a, const Point& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
Triangle exact(const std::array<Position, 3>& points)
{
    Triangle result;
    for (size_t i = 0; i < 3; ++i) {
        for (size_t k = 0; k < 3; ++k) {
            if (!std::isfinite(points[i][k])) { throw std::invalid_argument("Non-finite intersection coordinate."); }
            result[i][k] = points[i][k];
        }
    }
    return result;
}
Point normal(const Triangle& t) { return cross(subtract(t[1], t[0]), subtract(t[2], t[0])); }
bool zero(const Point& p) { return p[0] == 0 && p[1] == 0 && p[2] == 0; }
Point interpolate(const Point& a, const Point& b, const Exact& t)
{
    return {a[0]+t*(b[0]-a[0]), a[1]+t*(b[1]-a[1]), a[2]+t*(b[2]-a[2])};
}
// Clip a coplanar segment against the three inward triangle half-planes.
// Keeping exact endpoints also distinguishes a valid shared feature from overlap.
std::vector<Point> edgeIntersection(const Point& a, const Point& b, const Triangle& triangle, const Point& n)
{
    const Exact da = dot(n, subtract(a, triangle[0])), db = dot(n, subtract(b, triangle[0]));
    if ((da > 0 && db > 0) || (da < 0 && db < 0)) { return {}; }
    Exact lo = 0, hi = 1;
    if (da != 0 || db != 0) { lo = hi = da/(da-db); }
    for (size_t i = 0; i < 3; ++i) {
        const auto edge = subtract(triangle[(i+1)%3], triangle[i]);
        const Exact x = dot(n, cross(edge, subtract(a, triangle[i])));
        const Exact y = dot(n, cross(edge, subtract(b, triangle[i])));
        if (x < 0 && y < 0) { return {}; }
        if (x < 0) { const Exact t = x/(x-y); if (t > lo) { lo = t; } }
        if (y < 0) { const Exact t = x/(x-y); if (t < hi) { hi = t; } }
        if (lo > hi) { return {}; }
    }
    return {interpolate(a, b, lo), interpolate(a, b, hi)};
}
bool onSharedFeature(const Point& p, const std::vector<Point>& shared)
{
    if (shared.size() == 1) { return p == shared[0]; }
    if (shared.size() != 2) { return false; }
    if (!zero(cross(subtract(p, shared[0]), subtract(shared[1], shared[0])))) { return false; }
    for (size_t k = 0; k < 3; ++k) {
        if (p[k] < std::min(shared[0][k], shared[1][k]) || p[k] > std::max(shared[0][k], shared[1][k])) { return false; }
    }
    return true;
}
struct Box { Position minimum{}, maximum{}; };
struct Node { Box box; size_t begin = 0, end = 0, left = 0, right = 0; };
struct Tree { std::vector<Box> boxes; std::vector<size_t> order; std::vector<Node> nodes; };
bool overlaps(const Box& a, const Box& b)
{
    for (size_t k = 0; k < 3; ++k) { if (a.maximum[k] < b.minimum[k] || b.maximum[k] < a.minimum[k]) { return false; } }
    return true;
}
size_t buildNode(Tree& tree, size_t begin, size_t end, std::stop_token stop)
{
    canceled(stop);
    Node node; node.begin = begin; node.end = end; node.box = tree.boxes[tree.order[begin]];
    for (size_t i = begin+1; i < end; ++i) {
        canceled(stop);
        const auto& b = tree.boxes[tree.order[i]];
        for (size_t k = 0; k < 3; ++k) {
            node.box.minimum[k] = std::min(node.box.minimum[k], b.minimum[k]);
            node.box.maximum[k] = std::max(node.box.maximum[k], b.maximum[k]);
        }
    }
    const size_t index = tree.nodes.size(); tree.nodes.push_back(node);
    if (end-begin > 8) {
        size_t axis = 0;
        for (size_t k = 1; k < 3; ++k) {
            if (node.box.maximum[k]-node.box.minimum[k] > node.box.maximum[axis]-node.box.minimum[axis]) { axis = k; }
        }
        const size_t middle = begin+(end-begin)/2;
        std::nth_element(tree.order.begin()+static_cast<ptrdiff_t>(begin), tree.order.begin()+static_cast<ptrdiff_t>(middle),
            tree.order.begin()+static_cast<ptrdiff_t>(end), [&](size_t a, size_t b) {
                canceled(stop);
                const double x = tree.boxes[a].minimum[axis]+tree.boxes[a].maximum[axis];
                const double y = tree.boxes[b].minimum[axis]+tree.boxes[b].maximum[axis];
                return x == y ? a < b : x < y;
            });
        const auto left = buildNode(tree, begin, middle, stop), right = buildNode(tree, middle, end, stop);
        tree.nodes[index].left = left; tree.nodes[index].right = right;
    }
    return index;
}
std::array<Position, 3> positions(const SourceTopology& source, size_t face)
{
    const auto& ids = source.faces[face].vertices;
    return {source.vertices[ids[0]].position, source.vertices[ids[1]].position, source.vertices[ids[2]].position};
}
}

bool exactTriangleCollapsed(const std::array<Position, 3>& points)
{
    // Outward-rounded intervals cheaply prove the usual nonzero determinant.
    // Ambiguous, underflowing and overflowing cases fall back to exact rationals.
    for (const auto& p : points) { for (const auto v : p) {
        if (!std::isfinite(v)) { throw std::invalid_argument("Non-finite intersection coordinate."); }
    } }
    constexpr double infinity = std::numeric_limits<double>::infinity();
    using Interval = std::array<double, 2>;
    const auto difference = [](double a, double b) -> Interval {
        return {std::nextafter(a-b, -infinity), std::nextafter(a-b, infinity)};
    };
    const auto product = [](const Interval& a, const Interval& b) -> Interval {
        const std::array<double, 4> products = {a[0]*b[0], a[0]*b[1], a[1]*b[0], a[1]*b[1]};
        for (const auto v : products) { if (!std::isfinite(v)) { return {-infinity, infinity}; } }
        const auto [lo, hi] = std::minmax_element(products.begin(), products.end());
        return {std::nextafter(*lo, -infinity), std::nextafter(*hi, infinity)};
    };
    for (size_t k = 0; k < 3; ++k) {
        const size_t j = (k+1)%3;
        const auto a = product(difference(points[1][k],points[0][k]), difference(points[2][j],points[0][j]));
        const auto b = product(difference(points[1][j],points[0][j]), difference(points[2][k],points[0][k]));
        if (a[0] > b[1] || a[1] < b[0]) { return false; }
    }
    return zero(normal(exact(points)));
}
bool trianglesSelfIntersect(const std::array<Position, 3>& aPoints, const std::array<Position, 3>& bPoints,
    const std::array<size_t, 3>& aIds, const std::array<size_t, 3>& bIds)
{
    const auto a = exact(aPoints), b = exact(bPoints);
    const auto an = normal(a), bn = normal(b);
    if (zero(an) || zero(bn)) { return false; }
    std::vector<Point> shared;
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j) { if (aIds[i] == bIds[j] && a[i] == b[j]) { shared.push_back(a[i]); } }
    }
    // Non-coplanar planes containing the same edge meet only along that edge.
    if (shared.size() == 2 && !zero(cross(an,bn))) { return false; }
    for (size_t i = 0; i < 3; ++i) {
        for (const auto& p : edgeIntersection(a[i], a[(i+1)%3], b, bn)) { if (!onSharedFeature(p, shared)) { return true; } }
        for (const auto& p : edgeIntersection(b[i], b[(i+1)%3], a, an)) { if (!onSharedFeature(p, shared)) { return true; } }
    }
    return false;
}
const char* intersectionStatus(const MeshIntersections& result)
{
    switch (result.phase) {
    case IntersectionPhase::notChecked: return "not_checked";
    case IntersectionPhase::queued: return "queued";
    case IntersectionPhase::running: return "running";
    case IntersectionPhase::outdated: return "out_of_date";
    case IntersectionPhase::canceled: return "canceled";
    case IntersectionPhase::failed: return "failed";
    case IntersectionPhase::complete: break;
    }
    if (result.truncated) { return "partial"; }
    if (result.unavailableSources) { return result.availableSources ? "partial" : "unavailable"; }
    return "complete";
}
MeshIntersections inspectIntersections(const MeshTopology& topology, IntersectionSettings settings,
    std::stop_token stop, IntersectionLimits limits)
{
    MeshIntersections result; result.settings = settings; result.mode = topology.mode;
    canceled(stop);
    result.phase = IntersectionPhase::complete; result.hasResult = true;
    result.availableSources = topology.availableSources; result.unavailableSources = topology.unavailableSources;
    result.excludedCollapsedFaces = topology.excludedCollapsedFaces;
    std::set<std::tuple<uint64_t, size_t, uint64_t>> affected;
    for (const auto& source : topology.sources) {
        canceled(stop);
        if (!source.available || source.faces.empty()) { continue; }
        Tree tree;
        for (size_t f = 0; f < source.faces.size(); ++f) {
            canceled(stop);
            const auto p = positions(source, f);
            Box box{p[0], p[0]};
            for (const auto& v : p) { for (size_t k = 0; k < 3; ++k) { box.minimum[k] = std::min(box.minimum[k], v[k]); box.maximum[k] = std::max(box.maximum[k], v[k]); } }
            tree.boxes.push_back(box); tree.order.push_back(f);
        }
        buildNode(tree, 0, tree.order.size(), stop);
        for (size_t a = 0; a < source.faces.size() && !result.truncated; ++a) {
            std::vector<size_t> queue{0};
            while (!queue.empty() && !result.truncated) {
                canceled(stop);
                const auto& node = tree.nodes[queue.back()]; queue.pop_back();
                if (!overlaps(tree.boxes[a], node.box)) { continue; }
                if (node.left) { queue.push_back(node.right); queue.push_back(node.left); continue; }
                for (size_t i = node.begin; i < node.end; ++i) {
                    canceled(stop);
                    const size_t b = tree.order[i];
                    if (b <= a || !overlaps(tree.boxes[a], tree.boxes[b])) { continue; }
                    if (result.candidateTests >= limits.candidateTests) { result.truncated = true; break; }
                    ++result.candidateTests;
                    const auto ap = positions(source, a), bp = positions(source, b);
                    if (!trianglesSelfIntersect(ap, bp, source.faces[a].vertices, source.faces[b].vertices)) { continue; }
                    if (result.findings.size() >= limits.pairs) { result.truncated = true; break; }
                    IntersectionFinding finding;
                    finding.faces = {source.faces[a].reference, source.faces[b].reference};
                    finding.source = source.source; finding.provenance = source.provenance; finding.mode = source.mode;
                    for (size_t j = 0; j < 3; ++j) { for (size_t k = 0; k < 3; ++k) {
                        finding.geometry[j][k] = static_cast<float>(ap[j][k]); finding.geometry[j+3][k] = static_cast<float>(bp[j][k]);
                    } }
                    for (const auto& ref : finding.faces) { affected.emplace(ref.fileId, ref.triangleId, ref.partId); }
                    result.findings.push_back(std::move(finding));
                }
            }
        }
        if (result.truncated) { break; }
    }
    std::sort(result.findings.begin(), result.findings.end(), [&](const auto& a, const auto& b) {
        canceled(stop);
        const auto key = [](const auto& f) { return std::tuple{f.faces[0].fileId, f.faces[0].triangleId, f.faces[0].partId, f.faces[1].triangleId, f.faces[1].partId}; };
        return key(a) < key(b);
    });
    result.affectedFaces = affected.size();
    return result;
}
} // namespace woby
