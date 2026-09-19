#include "mesh_comparison.h"
#include "comparison_settings.h"
#include "parallel_work.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

namespace woby
{
namespace
{
using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;
void checkCanceled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error("Analysis canceled.");
}

template <typename T>
std::vector<T> copyWithCancellation(const std::vector<T>& values, std::stop_token stop)
{
    std::vector<T> result;
    checkCanceled(stop);
    result.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i % 256 == 0) { checkCanceled(stop); }
        result.push_back(values[i]);
    }
    return result;
}

MeshDiagnostics inspectTriangles(const std::vector<Triangle>& triangles, std::stop_token stop);
Point toPoint(const std::array<float, 3> &p)
{
    return {p[0], p[1], p[2]};
}
Point sub(const Point &a, const Point &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Point add(const Point &a, const Point &b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Point mul(const Point &p, double s)
{
    return {p[0] * s, p[1] * s, p[2] * s};
}
double dot(const Point &a, const Point &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Point cross(const Point &a, const Point &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double lengthSquared(const Point &p)
{
    return dot(p, p);
}
double area(const Triangle &t)
{
    return .5 * std::sqrt(lengthSquared(cross(sub(t[1], t[0]), sub(t[2], t[0]))));
}
bool degenerate(const Triangle &t)
{
    const double edge =
        std::max({lengthSquared(sub(t[1], t[0])), lengthSquared(sub(t[2], t[0])), lengthSquared(sub(t[2], t[1]))});
    return lengthSquared(cross(sub(t[1], t[0]), sub(t[2], t[0]))) <= edge * edge * 1e-24;
}
double segmentSquared(const Point &p, const Point &a, const Point &b)
{
    const auto ab = sub(b, a);
    const double denominator = lengthSquared(ab);
    const double t = denominator > 0 ? std::clamp(dot(sub(p, a), ab) / denominator, 0.0, 1.0) : 0.0;
    return lengthSquared(sub(p, add(a, mul(ab, t))));
}
double triangleSquared(const Point &p, const Triangle &t)
{
    const auto &a = t[0];
    const auto &b = t[1];
    const auto &c = t[2];
    if (degenerate(t))
        return std::min({segmentSquared(p, a, b), segmentSquared(p, b, c), segmentSquared(p, c, a)});
    const auto ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
        return lengthSquared(ap);
    const auto bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
        return lengthSquared(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return lengthSquared(sub(p, add(a, mul(ab, d1 / (d1 - d3)))));
    const auto cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
        return lengthSquared(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return lengthSquared(sub(p, add(a, mul(ac, d2 / (d2 - d6)))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
        return segmentSquared(p, b, c);
    const double denominator = 1 / (va + vb + vc);
    return lengthSquared(sub(p, add(a, add(mul(ab, vb * denominator), mul(ac, vc * denominator)))));
}

std::vector<Triangle> meshTriangles(const Mesh &mesh, std::stop_token stop)
{
    checkCanceled(stop);
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        throw std::runtime_error("Analysis needs nonempty triangular meshes.");
    std::vector<Triangle> triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (size_t i = 0; i < mesh.indices.size(); i += 3)
    {
        if (i % 768 == 0) { checkCanceled(stop); }
        Triangle t;
        for (size_t k = 0; k < 3; ++k)
        {
            const auto index = mesh.indices[i + k];
            if (index >= mesh.vertices.size() || !finitePosition(mesh.vertices[index].position))
                throw std::runtime_error("Mesh contains an invalid vertex or triangle index.");
            t[k] = toPoint(mesh.vertices[index].position);
        }
        triangles.push_back(t);
    }
    return triangles;
}

struct DistanceBounds { Point minimum{}, maximum{}; };
// Four siblings in structure-of-arrays form: contiguous double-precision bounds
// allow two SSE2 lane pairs without narrowing large/near-coincident geometry.
struct alignas(64) DistanceNode {
    std::array<std::array<double, 4>, 3> minimum{}, maximum{};
    std::array<uint32_t, 4> child{}, count{};
    uint32_t childCount = 0;
    std::array<uint32_t, 7> padding{};
};
static_assert(sizeof(DistanceNode) == 256);
struct DistanceTree
{
    std::stop_token stop;
    std::vector<Triangle> triangles;
    std::vector<uint32_t> order;
    std::vector<DistanceNode> nodes;
};
DistanceBounds distanceBounds(const DistanceTree& tree, size_t begin, size_t end)
{
    DistanceBounds bounds;
    bounds.minimum.fill(std::numeric_limits<double>::infinity());
    bounds.maximum.fill(-std::numeric_limits<double>::infinity());
    for (size_t i = begin; i < end; ++i)
    {
        if (i % 256 == 0) { checkCanceled(tree.stop); }
        for (const auto &p : tree.triangles[tree.order[i]])
            for (size_t axis = 0; axis < 3; ++axis)
            {
                bounds.minimum[axis] = std::min(bounds.minimum[axis], p[axis]);
                bounds.maximum[axis] = std::max(bounds.maximum[axis], p[axis]);
            }
    }
    return bounds;
}

uint32_t buildNode(DistanceTree& tree, size_t begin, size_t end, const DistanceBounds& bounds)
{
    checkCanceled(tree.stop);
    struct Range { size_t begin = 0, end = 0; DistanceBounds bounds; };
    std::array<Range, 4> ranges{};
    ranges[0] = {begin, end, bounds};
    uint32_t rangeCount = 1;
    while (rangeCount < 4) {
        uint32_t largest = 0;
        for (uint32_t i = 1; i < rangeCount; ++i) {
            if (ranges[i].end - ranges[i].begin > ranges[largest].end - ranges[largest].begin) { largest = i; }
        }
        const auto range = ranges[largest];
        if (range.end - range.begin <= 8) { break; }
        size_t axis = 0;
        for (size_t k = 1; k < 3; ++k) {
            if (range.bounds.maximum[k] - range.bounds.minimum[k] > range.bounds.maximum[axis] - range.bounds.minimum[axis]) { axis = k; }
        }
        const size_t middle = range.begin + (range.end - range.begin) / 2;
        std::nth_element(tree.order.begin() + static_cast<std::ptrdiff_t>(range.begin),
            tree.order.begin() + static_cast<std::ptrdiff_t>(middle),
            tree.order.begin() + static_cast<std::ptrdiff_t>(range.end), [&](uint32_t a, uint32_t b) {
                checkCanceled(tree.stop);
                const auto& ta = tree.triangles[a];
                const auto& tb = tree.triangles[b];
                const double ca = ta[0][axis] + ta[1][axis] + ta[2][axis];
                const double cb = tb[0][axis] + tb[1][axis] + tb[2][axis];
                return ca == cb ? a < b : ca < cb;
            });
        ranges[largest] = {range.begin, middle, distanceBounds(tree, range.begin, middle)};
        ranges[rangeCount++] = {middle, range.end, distanceBounds(tree, middle, range.end)};
    }
    const auto index = static_cast<uint32_t>(tree.nodes.size());
    tree.nodes.emplace_back();
    tree.nodes[index].childCount = rangeCount;
    for (uint32_t i = 0; i < rangeCount; ++i) {
        const auto& range = ranges[i];
        for (size_t k = 0; k < 3; ++k) {
            tree.nodes[index].minimum[k][i] = range.bounds.minimum[k];
            tree.nodes[index].maximum[k][i] = range.bounds.maximum[k];
        }
        if (range.end - range.begin <= 8) {
            tree.nodes[index].child[i] = static_cast<uint32_t>(range.begin);
            tree.nodes[index].count[i] = static_cast<uint32_t>(range.end - range.begin);
        } else {
            const auto child = buildNode(tree, range.begin, range.end, range.bounds);
            tree.nodes[index].child[i] = child;
        }
    }
    return index;
}
DistanceTree buildTree(const Mesh &mesh, std::stop_token stop)
{
    validateComparisonMeshSize(mesh.vertices.size(), mesh.indices.size() / 3);
    DistanceTree tree;
    tree.stop = stop;
    tree.triangles = meshTriangles(mesh, stop);
    tree.order.reserve(tree.triangles.size());
    for (size_t i = 0; i < tree.triangles.size(); ++i)
    {
        if (i % 256 == 0) { checkCanceled(stop); }
        tree.order.push_back(static_cast<uint32_t>(i));
    }
    tree.nodes.reserve(tree.order.size() / 16 + 1);
    buildNode(tree, 0, tree.order.size(), distanceBounds(tree, 0, tree.order.size()));
    // Apply the new-to-old permutation in place, then release it. Every leaf
    // streams contiguous triangles, without a second triangle copy or indirection.
    for (size_t i = 0; i < tree.order.size(); ++i) {
        if (i % 256 == 0) { checkCanceled(stop); }
        if (tree.order[i] == i) { continue; }
        const auto first = tree.triangles[i];
        size_t current = i;
        while (tree.order[current] != i) {
            checkCanceled(stop);
            const auto next = tree.order[current];
            tree.triangles[current] = tree.triangles[next];
            tree.order[current] = static_cast<uint32_t>(current);
            current = next;
        }
        tree.triangles[current] = first;
        tree.order[current] = static_cast<uint32_t>(current);
    }
    std::vector<uint32_t>().swap(tree.order);
    return tree;
}
std::array<double, 4> boxSquared(const Point& p, const DistanceNode& node)
{
    std::array<double, 4> result{};
#if defined(__SSE2__) || defined(_M_X64)
    for (size_t lane = 0; lane < 4; lane += 2) {
        auto sum = _mm_setzero_pd();
        for (size_t k = 0; k < 3; ++k) {
            const auto point = _mm_set1_pd(p[k]);
            const auto low = _mm_loadu_pd(node.minimum[k].data() + lane);
            const auto high = _mm_loadu_pd(node.maximum[k].data() + lane);
            const auto delta = _mm_sub_pd(point, _mm_min_pd(_mm_max_pd(point, low), high));
            sum = _mm_add_pd(sum, _mm_mul_pd(delta, delta));
        }
        _mm_storeu_pd(result.data() + lane, sum);
    }
#else
    for (size_t lane = 0; lane < node.childCount; ++lane) {
        for (size_t k = 0; k < 3; ++k) {
            const double d = p[k] - std::clamp(p[k], node.minimum[k][lane], node.maximum[k][lane]);
            result[lane] += d * d;
        }
    }
#endif
    return result;
}

template <typename T>
void resizeWithCancellation(std::vector<T>& values, size_t count, std::stop_token stop)
{
    checkCanceled(stop);
    values.reserve(count);
    constexpr size_t batch = std::max(size_t{1}, size_t{65536} / sizeof(T));
    while (values.size() < count) {
        checkCanceled(stop);
        values.resize(values.size() + std::min(batch, count - values.size()));
    }
}
void nearest(const DistanceTree& tree, const Point& p, double& best, uint32_t& hint)
{
    checkCanceled(tree.stop);
    // The previous sample's closest face is an exact upper bound for this
    // query. Neighboring centroids usually share a nearby target surface.
    best = triangleSquared(p, tree.triangles[hint]);
    struct Visit { double distance = 0; uint32_t child = 0, count = 0; };
    // Median splits and 32-bit primitive counts bound depth to 32. At most
    // three deferred siblings per level, plus the current entry, fit here.
    std::array<Visit, 100> stack{};
    size_t pending = 1, visits = 0;
    while (pending && best != 0) {
        if (++visits % 256 == 0) { checkCanceled(tree.stop); }
        const auto visit = stack[--pending];
        if (visit.distance > best) { continue; }
        if (visit.count) {
            for (size_t i = visit.child; i < size_t(visit.child) + visit.count; ++i) {
                const auto distance = triangleSquared(p, tree.triangles[i]);
                if (distance < best) { best = distance; hint = static_cast<uint32_t>(i); }
            }
            continue;
        }
        const auto& node = tree.nodes[visit.child];
        const auto distances = boxSquared(p, node);
        std::array<Visit, 4> children{};
        size_t count = 0;
        for (uint32_t lane = 0; lane < node.childCount; ++lane) {
            if (distances[lane] > best) { continue; }
            const Visit child{distances[lane], node.child[lane], node.count[lane]};
            size_t position = count++;
            while (position && children[position - 1].distance < child.distance) {
                children[position] = children[position - 1];
                --position;
            }
            children[position] = child;
        }
        // Push far-to-near so the closest child tightens the bound first.
        for (size_t i = 0; i < count; ++i) { stack[pending++] = children[i]; }
    }
}

std::vector<DiagnosticEdge> duplicateBounds(const DuplicateResult& result, std::stop_token stop)
{
    std::vector<DiagnosticEdge> bounds;
    for (const auto& finding : result.findings) {
        checkCanceled(stop);
        DiagnosticEdge edge{finding.geometry.front(), finding.geometry.front()};
        for (const auto& point : finding.geometry) {
            checkCanceled(stop);
            for (size_t k = 0; k < 3; ++k) { edge.a[k] = std::min(edge.a[k], point[k]); edge.b[k] = std::max(edge.b[k], point[k]); }
        }
        bounds.push_back(edge);
    }
    return bounds;
}

void inspectSurfaceDegenerates(SurfaceComparison& surface, const Mesh& mesh, DegenerateSettings settings, std::stop_token stop)
{
    const std::vector<DuplicateSource> empty;
    surface.degenerates = inspectDegenerates(mesh.duplicateInput ? mesh.duplicateInput->sources : empty, settings, stop);
    if (settings.enabled && !mesh.duplicateInput) { surface.degenerates.unavailableSources = 1; }
    for (const auto& finding : surface.degenerates.findings) {
        checkCanceled(stop);
        DiagnosticEdge bounds{finding.geometry[0], finding.geometry[0]};
        for (const auto& p : finding.geometry) {
            for (size_t k = 0; k < 3; ++k) { bounds.a[k] = std::min(bounds.a[k], p[k]); bounds.b[k] = std::max(bounds.b[k], p[k]); }
        }
        surface.degenerateBounds.push_back(bounds);
    }
}

void prepareTopologyInspectionGeometry(SurfaceComparison& surface, std::stop_token stop, bool holesOnly = false)
{
    const auto point = [](const auto& p) { return std::array<float, 3>{static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])}; };
    if (!holesOnly) { surface.nonManifoldVertexBounds.clear(); surface.nonManifoldVertexMarkers.clear(); }
    surface.holeBounds.clear(); surface.holeEdges.clear();
    if (!holesOnly) {
        for (const auto& finding : surface.topology.nonManifoldVertices) {
            checkCanceled(stop);
            const auto& source = surface.topology.sources[finding.source];
            const auto& vertex = source.vertices[finding.vertex];
            const auto p = point(vertex.position);
            auto minimum = p, maximum = p;
            for (const auto f : vertex.faces) {
                checkCanceled(stop);
                for (const auto v : source.faces[f].vertices) {
                    const auto q = point(source.vertices[v].position);
                    for (size_t k = 0; k < 3; ++k) { minimum[k] = std::min(minimum[k], q[k]); maximum[k] = std::max(maximum[k], q[k]); }
                }
            }
            surface.nonManifoldVertexBounds.push_back({minimum, maximum});
            double extent = 0;
            for (size_t k = 0; k < 3; ++k) { extent = std::max(extent, static_cast<double>(maximum[k])-minimum[k]); }
            const float radius = static_cast<float>(extent * .025);
            for (size_t k = 0; k < 3; ++k) {
                auto a = p, b = p;
                constexpr double limit = std::numeric_limits<float>::max();
                a[k] = static_cast<float>(std::clamp(static_cast<double>(p[k])-radius, -limit, limit));
                b[k] = static_cast<float>(std::clamp(static_cast<double>(p[k])+radius, -limit, limit));
                surface.nonManifoldVertexMarkers.push_back({a, b});
            }
        }
    }
    for (const auto index : surface.topology.holes) {
        checkCanceled(stop);
        const auto& finding = surface.topology.boundaryRegions[index];
        const auto& source = surface.topology.sources[finding.source];
        surface.holeBounds.push_back({point(finding.minimum), point(finding.maximum)});
        for (const auto e : finding.edges) {
            checkCanceled(stop);
            const auto& edge = source.edges[e];
            surface.holeEdges.push_back({point(source.vertices[edge.vertices[0]].position), point(source.vertices[edge.vertices[1]].position)});
        }
    }
}

void inspectSurfaceTopology(SurfaceComparison& surface, const Mesh& mesh, TopologyMode mode, std::stop_token stop)
{
    const std::vector<DuplicateSource> empty;
    surface.topology = buildMeshTopology(mesh.duplicateInput ? mesh.duplicateInput->sources : empty, mode, stop);
    if (!mesh.duplicateInput) { surface.topology.unavailableSources = 1; }
    const auto geometry = [&](const std::vector<TopologyEdgeFinding>& findings) {
        std::vector<DiagnosticEdge> result;
        for (const auto& finding : findings) {
            checkCanceled(stop);
            const auto& source = surface.topology.sources[finding.source];
            const auto& edge = source.edges[finding.edge];
            DiagnosticEdge value;
            for (size_t k = 0; k < 3; ++k) {
                value.a[k] = static_cast<float>(source.vertices[edge.vertices[0]].position[k]);
                value.b[k] = static_cast<float>(source.vertices[edge.vertices[1]].position[k]);
            }
            result.push_back(value);
        }
        return result;
    };
    surface.topologyBoundaries = geometry(surface.topology.boundaries);
    surface.topologyNonManifold = geometry(surface.topology.nonManifoldEdges);
    surface.topologyWinding = geometry(surface.topology.windingEdges);
    prepareTopologyInspectionGeometry(surface, stop);
}

SurfaceComparison copySurface(const Mesh& mesh, std::stop_token stop, DegenerateSettings degenerates = {})
{
    checkCanceled(stop);
    SurfaceComparison result;
    if (mesh.duplicateInput) { result.duplicates = inspectDuplicates(*mesh.duplicateInput, stop); }
    else {
        result.duplicates.points.unavailableSources = 1;
        result.duplicates.triangles.unavailableSources = 1;
    }
    inspectSurfaceDegenerates(result, mesh, degenerates, stop);
    result.duplicatePointBounds = duplicateBounds(result.duplicates.points, stop);
    result.duplicateTriangleBounds = duplicateBounds(result.duplicates.triangles, stop);
    result.source.vertices = copyWithCancellation(mesh.vertices, stop);
    result.source.indices = copyWithCancellation(mesh.indices, stop);
    result.source.nodes = copyWithCancellation(mesh.nodes, stop);
    result.source.bounds = mesh.bounds;
    result.quality = inspectSurfaceMeshQuality(mesh, stop);
    return result;
}

SurfaceComparison compareSurface(const Mesh &mesh, const DistanceTree &source, const DistanceTree &target, bool distancesOnly = false, DegenerateSettings degenerates = {})
{
    const auto stop = source.stop;
    auto result = distancesOnly ? SurfaceComparison{} : copySurface(mesh, stop, degenerates);
    if (!distancesOnly) { result.diagnostics = inspectMesh(mesh, stop); }
    const size_t triangleCount = mesh.indices.size() / 3;
    resizeWithCancellation(result.sampled.vertices, triangleCount * 12, stop);
    resizeWithCancellation(result.sampled.indices, triangleCount * 12, stop);
    resizeWithCancellation(result.distances, triangleCount * 4, stop);
    resizeWithCancellation(result.sampleAreas, triangleCount * 4, stop);
    // Each worker owns disjoint output ranges. The source order remains the
    // saved triangle order even though target BVH leaves are spatially packed.
    parallelAnalysisBatches(triangleCount, 128, stop, [&](size_t begin, size_t end) {
        uint32_t hint = 0;
        for (size_t face = begin; face < end; ++face) {
            checkCanceled(stop);
            const Triangle t = {toPoint(mesh.vertices[mesh.indices[face * 3]].position),
                toPoint(mesh.vertices[mesh.indices[face * 3 + 1]].position),
                toPoint(mesh.vertices[mesh.indices[face * 3 + 2]].position)};
            const Point ab = mul(add(t[0], t[1]), .5), bc = mul(add(t[1], t[2]), .5), ca = mul(add(t[2], t[0]), .5);
            const std::array<Triangle, 4> parts = {Triangle{t[0], ab, ca}, Triangle{ab, t[1], bc}, Triangle{ca, bc, t[2]},
                                                   Triangle{ab, bc, ca}};
            for (size_t sample = 0; sample < parts.size(); ++sample)
            {
                const auto& part = parts[sample];
                const size_t sampleIndex = face * 4 + sample;
                const Point center = mul(add(add(part[0], part[1]), part[2]), 1.0 / 3.0);
                double squared = std::numeric_limits<double>::infinity();
                nearest(target, center, squared, hint);
                const double distance = std::sqrt(squared), weight = area(part);
                if (distance > std::numeric_limits<float>::max())
                    throw std::runtime_error("Analysis distance exceeds the supported display range.");
                result.distances[sampleIndex] = distance;
                result.sampleAreas[sampleIndex] = weight;
                const auto normal = cross(sub(part[1], part[0]), sub(part[2], part[0]));
                const double normalLength = std::sqrt(lengthSquared(normal));
                for (size_t corner = 0; corner < part.size(); ++corner)
                {
                    const auto& p = part[corner];
                    Vertex vertex;
                    for (size_t k = 0; k < 3; ++k)
                    {
                        vertex.position[k] = static_cast<float>(p[k]);
                        vertex.normal[k] = normalLength > 0 ? static_cast<float>(normal[k] / normalLength) : 0.0f;
                    }
                    vertex.texcoord[0] = static_cast<float>(distance);
                    const auto index = sampleIndex * 3 + corner;
                    result.sampled.indices[index] = static_cast<uint32_t>(index);
                    result.sampled.vertices[index] = vertex;
                }
            }
        }
    });
    // Fixed-order reductions keep weighted statistics reproducible regardless
    // of worker scheduling, available CPU capacity, and cancellation timing.
    double weightedDistance = 0, totalArea = 0;
    for (size_t i = 0; i < result.distances.size(); ++i) {
        if (i % 256 == 0) { checkCanceled(stop); }
        result.maximum = std::max(result.maximum, result.distances[i]);
        weightedDistance += result.distances[i] * result.sampleAreas[i];
        totalArea += result.sampleAreas[i];
    }
    // Compute bounds with cancellation between small batches of vertices.
    auto& bounds = result.sampled.bounds;
    bounds.min = result.sampled.vertices.front().position;
    bounds.max = bounds.min;
    for (size_t i = 0; i < result.sampled.vertices.size(); ++i)
    {
        if (i % 256 == 0) { checkCanceled(stop); }
        for (size_t k = 0; k < 3; ++k)
        {
            bounds.min[k] = std::min(bounds.min[k], result.sampled.vertices[i].position[k]);
            bounds.max[k] = std::max(bounds.max[k], result.sampled.vertices[i].position[k]);
        }
    }
    for (size_t k = 0; k < 3; ++k) { bounds.center[k] = (bounds.min[k] + bounds.max[k]) * .5f; }
    float radiusSquared = 0;
    for (size_t i = 0; i < result.sampled.vertices.size(); ++i)
    {
        if (i % 256 == 0) { checkCanceled(stop); }
        float squared = 0;
        for (size_t k = 0; k < 3; ++k)
        {
            const float offset = result.sampled.vertices[i].position[k] - bounds.center[k];
            squared += offset * offset;
        }
        radiusSquared = std::max(radiusSquared, squared);
    }
    bounds.radius = std::max(std::sqrt(radiusSquared), .001f);
    result.sampled.nodes.push_back({"Analysis", 0, static_cast<uint32_t>(result.sampled.indices.size())});
    if (totalArea > 0)
    {
        result.mean = weightedDistance / totalArea;
        std::vector<size_t> sorted;
        sorted.reserve(result.distances.size());
        for (size_t i = 0; i < result.distances.size(); ++i)
        {
            if (i % 256 == 0) { checkCanceled(stop); }
            sorted.push_back(i);
        }
        // Break ties by sample index to retain stable ordering without the
        // temporary allocation and unchecked merge passes of stable_sort.
        std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
            checkCanceled(stop);
            return result.distances[a] == result.distances[b] ? a < b : result.distances[a] < result.distances[b];
        });
        double cumulative = 0;
        for (const size_t index : sorted)
        {
            checkCanceled(stop);
            cumulative += result.sampleAreas[index];
            if (cumulative >= totalArea * .95)
            {
                result.percentile95 = result.distances[index];
                break;
            }
        }
    }
    checkCanceled(stop);
    return result;
}
} // namespace

ComparisonSettings normalizedComparisonSettings(ComparisonSettings settings)
{
    if (settings.diagnosticSide != ComparisonSide::a && settings.diagnosticSide != ComparisonSide::b) {
        settings.diagnosticSide = ComparisonSide::a;
    }
    if (settings.diagnosticCategory != DiagnosticCategory::boundary &&
        settings.diagnosticCategory != DiagnosticCategory::nonManifold &&
        settings.diagnosticCategory != DiagnosticCategory::winding &&
        settings.diagnosticCategory != DiagnosticCategory::duplicatePoints &&
        settings.diagnosticCategory != DiagnosticCategory::duplicateTriangles &&
        settings.diagnosticCategory != DiagnosticCategory::selfIntersections &&
        settings.diagnosticCategory != DiagnosticCategory::degenerateTriangles &&
        settings.diagnosticCategory != DiagnosticCategory::nonManifoldVertices && settings.diagnosticCategory != DiagnosticCategory::holes) {
        settings.diagnosticCategory = DiagnosticCategory::boundary;
    }
    if (settings.mode != ComparisonMode::distance && settings.mode != ComparisonMode::original &&
        settings.mode != ComparisonMode::repaired && settings.mode != ComparisonMode::overlay &&
        settings.mode != ComparisonMode::surfaceQuality)
        settings.mode = ComparisonMode::distance;
    settings.tolerance = std::isfinite(settings.tolerance) ? std::clamp(settings.tolerance, 0.0f, 1e12f) : .05f;
    settings.colorRange = std::isfinite(settings.colorRange) ? std::clamp(settings.colorRange, 1e-6f, 1e12f) : .5f;
    settings.colorRange = std::max(settings.colorRange, settings.tolerance);
    settings.quality = normalizedSurfaceQualitySettings(settings.quality);
    settings.degenerates = normalizedDegenerateSettings(settings.degenerates);
    settings.topologyInspection = normalizedTopologyInspectionSettings(settings.topologyInspection);
    settings.topologyMode = normalizedTopologyMode(settings.topologyMode);
    return settings;
}

const std::vector<DiagnosticEdge>& comparisonDiagnosticEdges(
    const MeshComparison& result, ComparisonSide side, DiagnosticCategory category)
{
    const auto& surface = side == ComparisonSide::a ? result.original : result.repaired;
    const auto& diagnostics = surface.diagnostics;
    // Legacy callers may supply geometry-only results. Application snapshots retain source records.
    if (!surface.topology.sources.empty()) {
        if (category == DiagnosticCategory::boundary) { return surface.topologyBoundaries; }
        if (category == DiagnosticCategory::nonManifold) { return surface.topologyNonManifold; }
        if (category == DiagnosticCategory::winding) { return surface.topologyWinding; }
    }
    switch (category) {
    case DiagnosticCategory::nonManifoldVertices: return surface.nonManifoldVertexBounds;
    case DiagnosticCategory::holes: return surface.holeBounds;
    case DiagnosticCategory::boundary: return diagnostics.boundaryEdges;
    case DiagnosticCategory::nonManifold: return diagnostics.nonManifoldEdges;
    case DiagnosticCategory::winding: return diagnostics.inconsistentWindingEdges;
    case DiagnosticCategory::duplicatePoints: return side == ComparisonSide::a ? result.original.duplicatePointBounds : result.repaired.duplicatePointBounds;
    case DiagnosticCategory::selfIntersections: return side == ComparisonSide::a ? result.original.intersectionBounds : result.repaired.intersectionBounds;
    case DiagnosticCategory::degenerateTriangles: return side == ComparisonSide::a ? result.original.degenerateBounds : result.repaired.degenerateBounds;
    case DiagnosticCategory::duplicateTriangles: return side == ComparisonSide::a ? result.original.duplicateTriangleBounds : result.repaired.duplicateTriangleBounds;
    }
    static const std::vector<DiagnosticEdge> empty;
    return empty;
}

const DuplicateResult& comparisonDuplicates(const MeshComparison& result, ComparisonSide side, DiagnosticCategory category)
{
    const auto& duplicates = side == ComparisonSide::a ? result.original.duplicates : result.repaired.duplicates;
    return category == DiagnosticCategory::duplicatePoints ? duplicates.points : duplicates.triangles;
}

double pointTriangleDistance(const std::array<float, 3> &p, const std::array<float, 3> &a,
                             const std::array<float, 3> &b, const std::array<float, 3> &c)
{
    if (!finitePosition(p) || !finitePosition(a) || !finitePosition(b) || !finitePosition(c))
        throw std::runtime_error("Distance requires finite coordinates.");
    return std::sqrt(triangleSquared(toPoint(p), {toPoint(a), toPoint(b), toPoint(c)}));
}

namespace
{
MeshDiagnostics inspectTriangles(const std::vector<Triangle>& triangles, std::stop_token stop)
{
    checkCanceled(stop);
    MeshDiagnostics result;
    // Match geometric positions, so duplicated OBJ/STL seam vertices do not
    // masquerade as open edges. Nearby, unequal positions remain separate.
    std::map<Point, size_t> vertexIds;
    std::vector<Point> positions;
    struct EdgeUse
    {
        size_t count = 0;
        int orientation = 0;
    };
    std::map<std::pair<size_t, size_t>, EdgeUse> edges;
    std::map<std::array<size_t, 3>, size_t> faces;
    for (const auto &t : triangles)
    {
        checkCanceled(stop);
        if (degenerate(t))
        {
            ++result.degenerateTriangles;
            continue;
        }
        std::array<size_t, 3> ids;
        for (size_t k = 0; k < 3; ++k)
        {
            const auto [it, inserted] = vertexIds.emplace(t[k], positions.size());
            if (inserted)
                positions.push_back(t[k]);
            ids[k] = it->second;
        }
        auto canonical = ids;
        std::sort(canonical.begin(), canonical.end());
        if (++faces[canonical] > 1)
            ++result.duplicateTriangles;
        for (size_t k = 0; k < 3; ++k)
        {
            const size_t a = ids[k], b = ids[(k + 1) % 3];
            auto &edge = edges[std::minmax(a, b)];
            ++edge.count;
            edge.orientation += a < b ? 1 : -1;
        }
    }
    for (const auto &[indices, use] : edges)
    {
        checkCanceled(stop);
        DiagnosticEdge edge;
        for (size_t k = 0; k < 3; ++k)
        {
            edge.a[k] = static_cast<float>(positions[indices.first][k]);
            edge.b[k] = static_cast<float>(positions[indices.second][k]);
        }
        if (use.count == 1)
            result.boundaryEdges.push_back(edge);
        if (use.count > 2)
            result.nonManifoldEdges.push_back(edge);
        if (use.count == 2 && use.orientation != 0)
            result.inconsistentWindingEdges.push_back(edge);
    }
    return result;
}
} // namespace

uint32_t comparisonBufferBytes(size_t count, size_t elementBytes)
{
    if (elementBytes == 0 || count > std::numeric_limits<uint32_t>::max() / elementBytes)
        throw std::runtime_error("Analysis exceeds the supported 32-bit buffer size. Reduce the selected geometry.");
    return static_cast<uint32_t>(count * elementBytes);
}

void validateComparisonMeshSize(size_t vertexCount, size_t triangleCount)
{
    (void)comparisonBufferBytes(vertexCount, sizeof(Vertex));
    (void)comparisonBufferBytes(triangleCount, 12 * sizeof(Vertex));
    (void)comparisonBufferBytes(triangleCount, 12 * sizeof(uint32_t));
    (void)comparisonBufferBytes(triangleCount, 6 * sizeof(uint32_t));
}

MeshDiagnostics inspectMesh(const Mesh &mesh, std::stop_token stop)
{
    return inspectTriangles(meshTriangles(mesh, stop), stop);
}

static void completeComparisonDetectors(MeshComparison& result, uint32_t stages)
{
    for (size_t i = 0; i < result.detectors.size(); ++i) {
        const auto category = static_cast<DiagnosticCategory>(i);
        if (!(stages & comparisonDiagnosticStage(category))) { continue; }
        auto& status = result.detectors[i];
        status.phase = IntersectionPhase::complete; status.hasResult = true;
        for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
            const auto& surface = side == ComparisonSide::a ? result.original : result.repaired;
            status.knownCounts[side == ComparisonSide::a ? 0 : 1] = category == DiagnosticCategory::duplicatePoints
                ? surface.duplicates.points.duplicateCount : category == DiagnosticCategory::duplicateTriangles
                ? surface.duplicates.triangles.duplicateCount : category == DiagnosticCategory::winding
                ? surface.topology.windingFaces.size() : comparisonDiagnosticEdges(result, side, category).size();
        }
    }
}

MeshComparison compareMeshes(const Mesh &original, const Mesh &repaired, std::stop_token stop, DegenerateSettings degenerates, TopologyMode topologyMode)
{
    checkCanceled(stop);
    validateComparisonMeshSize(original.vertices.size(), original.indices.size() / 3);
    validateComparisonMeshSize(repaired.vertices.size(), repaired.indices.size() / 3);
    if (original.vertices.empty() && original.indices.empty()) {
        MeshComparison result;
        result.repaired = copySurface(repaired, stop, degenerates);
        result.repaired.diagnostics = inspectMesh(repaired, stop);
        inspectSurfaceTopology(result.repaired, repaired, topologyMode, stop);
        result.qualityDistributions = surfaceQualityDistributions(result.original.quality, result.repaired.quality, stop);
        completeComparisonDetectors(result, comparisonDetectors);
        return result;
    }
    if (repaired.vertices.empty() && repaired.indices.empty()) {
        MeshComparison result;
        result.original = copySurface(original, stop, degenerates);
        result.original.diagnostics = inspectMesh(original, stop);
        inspectSurfaceTopology(result.original, original, topologyMode, stop);
        result.qualityDistributions = surfaceQualityDistributions(result.original.quality, result.repaired.quality, stop);
        completeComparisonDetectors(result, comparisonDetectors);
        return result;
    }
    const auto originalTree = buildTree(original, stop), repairedTree = buildTree(repaired, stop);
    MeshComparison result;
    result.original = compareSurface(original, originalTree, repairedTree, false, degenerates);
    result.repaired = compareSurface(repaired, repairedTree, originalTree, false, degenerates);
    inspectSurfaceTopology(result.original, original, topologyMode, stop);
    inspectSurfaceTopology(result.repaired, repaired, topologyMode, stop);
    result.qualityDistributions = surfaceQualityDistributions(result.original.quality, result.repaired.quality, stop);
    completeComparisonDetectors(result, comparisonDetectors);
    return result;
}

bool diagnosticAutoUpdate(const ComparisonSettings& settings, DiagnosticCategory category)
{
    switch (category) {
    case DiagnosticCategory::boundary: return settings.autoUpdateBoundaries;
    case DiagnosticCategory::nonManifold: return settings.autoUpdateNonManifold;
    case DiagnosticCategory::winding: return settings.autoUpdateWinding;
    case DiagnosticCategory::duplicatePoints: return settings.duplicates.points;
    case DiagnosticCategory::duplicateTriangles: return settings.duplicates.triangles;
    case DiagnosticCategory::degenerateTriangles: return settings.degenerates.enabled;
    case DiagnosticCategory::nonManifoldVertices: return settings.topologyInspection.nonManifoldVertices;
    case DiagnosticCategory::holes: return settings.topologyInspection.holes;
    case DiagnosticCategory::selfIntersections: return settings.intersections.autoUpdate;
    }
    return false;
}

void setDiagnosticAutoUpdate(ComparisonSettings& settings, DiagnosticCategory category, bool automatic)
{
    switch (category) {
    case DiagnosticCategory::boundary: settings.autoUpdateBoundaries = automatic; break;
    case DiagnosticCategory::nonManifold: settings.autoUpdateNonManifold = automatic; break;
    case DiagnosticCategory::winding: settings.autoUpdateWinding = automatic; break;
    case DiagnosticCategory::duplicatePoints: settings.duplicates.points = automatic; break;
    case DiagnosticCategory::duplicateTriangles: settings.duplicates.triangles = automatic; break;
    case DiagnosticCategory::degenerateTriangles: settings.degenerates.enabled = automatic; break;
    case DiagnosticCategory::nonManifoldVertices: settings.topologyInspection.nonManifoldVertices = automatic; break;
    case DiagnosticCategory::holes: settings.topologyInspection.holes = automatic; break;
    case DiagnosticCategory::selfIntersections: settings.intersections.autoUpdate = automatic; break;
    }
}

const char* detectorPhaseName(IntersectionPhase phase)
{
    switch (phase) {
    case IntersectionPhase::notChecked: return "not_checked";
    case IntersectionPhase::queued: return "queued";
    case IntersectionPhase::running: return "running";
    case IntersectionPhase::complete: return "complete";
    case IntersectionPhase::outdated: return "out_of_date";
    case IntersectionPhase::canceled: return "canceled";
    case IntersectionPhase::failed: return "failed";
    }
    return "not_checked";
}

DetectorStatus comparisonDetectorStatus(const MeshComparison& result, DiagnosticCategory category)
{
    if (category != DiagnosticCategory::selfIntersections) { return result.detectors.at(static_cast<size_t>(category)); }
    const auto& inspection = result.original.intersections;
    return {inspection.phase, inspection.hasResult,
        {inspection.findings.size(), result.repaired.intersections.findings.size()}, inspection.error};
}

void invalidateComparisonDetectors(MeshComparison& result, uint32_t stages)
{
    for (size_t i = 0; i < result.detectors.size(); ++i) {
        if (!(comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)) & stages)) { continue; }
        auto& status = result.detectors[i];
        status.phase = status.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
        status.error.clear();
    }
}

uint32_t comparisonDiagnosticStage(DiagnosticCategory category)
{
    switch (category) {
    case DiagnosticCategory::duplicatePoints: return comparisonDuplicatePoints;
    case DiagnosticCategory::duplicateTriangles: return comparisonDuplicateTriangles;
    case DiagnosticCategory::degenerateTriangles: return comparisonDegenerates;
    case DiagnosticCategory::selfIntersections: return comparisonIntersections;
    case DiagnosticCategory::boundary:
    case DiagnosticCategory::nonManifold:
    case DiagnosticCategory::winding:
    case DiagnosticCategory::nonManifoldVertices:
    case DiagnosticCategory::holes: return comparisonTopology;
    }
    return comparisonTopology;
}
uint32_t nextComparisonStage(uint32_t missing)
{
    // Publish inexpensive findings before distance/quality and expensive checks.
    for (const auto stage : {comparisonSource, comparisonTopology, comparisonDuplicatePoints,
        comparisonDuplicateTriangles, comparisonDegenerates, comparisonQuality, comparisonDistance}) {
        if (missing & stage) { return stage; }
    }
    return 0;
}

uint32_t requestedComparisonStages(const ComparisonSettings& settings, bool bothInputs, bool fullResults)
{
    uint32_t stages = comparisonSource;
    for (size_t i = 0; i < diagnosticCategoryCount; ++i) {
        const auto category = static_cast<DiagnosticCategory>(i);
        if (diagnosticAutoUpdate(settings, category)) { stages |= comparisonDiagnosticStage(category); }
    }
    if (fullResults || settings.mode == ComparisonMode::surfaceQuality) { stages |= comparisonQuality; }
    if (bothInputs && (fullResults || settings.mode == ComparisonMode::distance)) { stages |= comparisonDistance; }
    return stages;
}

bool resetComparisonCache(ComparisonCacheStatus& cache, uint64_t signature)
{
    if (cache.signature == signature) { return false; }
    cache = {signature, 0};
    return true;
}

bool resetComparisonDegenerateCache(ComparisonCacheStatus& cache, DegenerateSettings settings)
{
    settings = normalizedDegenerateSettings(settings);
    const bool changed = !sameDegenerateThresholds(cache.degenerates, settings);
    cache.degenerates = settings;
    if (changed) { cache.completed &= ~comparisonDegenerates; }
    return changed;
}

bool resetComparisonTopologyCache(ComparisonCacheStatus& cache, TopologyMode mode)
{
    mode = normalizedTopologyMode(mode);
    if (cache.topologyMode == mode) { return false; }
    cache.topologyMode = mode;
    cache.completed &= ~(comparisonTopology | comparisonIntersections);
    return true;
}

MeshComparison computeComparisonStages(const Mesh& original, const Mesh& repaired, uint32_t stages, std::stop_token stop, DegenerateSettings degenerates, TopologyMode topologyMode)
{
    checkCanceled(stop);
    MeshComparison result;
    if ((stages & comparisonDistance) && !original.indices.empty() && !repaired.indices.empty()) {
        const auto a = buildTree(original, stop), b = buildTree(repaired, stop);
        result.original = compareSurface(original, a, b, true);
        result.repaired = compareSurface(repaired, b, a, true);
    }
    const auto inspect = [&](const Mesh& mesh, SurfaceComparison& surface) {
        checkCanceled(stop);
        if (mesh.vertices.empty() && mesh.indices.empty()) {
            if (stages & comparisonIntersections) {
                surface.intersections.phase = IntersectionPhase::complete;
                surface.intersections.hasResult = true; surface.intersections.mode = topologyMode;
            }
            return;
        }
        validateComparisonMeshSize(mesh.vertices.size(), mesh.indices.size() / 3);
        if (stages & comparisonSource) {
            surface.source.vertices = copyWithCancellation(mesh.vertices, stop);
            surface.source.indices = copyWithCancellation(mesh.indices, stop);
            surface.source.nodes = copyWithCancellation(mesh.nodes, stop);
            surface.source.bounds = mesh.bounds;
        }
        if (stages & comparisonTopology) {
            surface.diagnostics = inspectMesh(mesh, stop);
            inspectSurfaceTopology(surface, mesh, topologyMode, stop);
        }
        if (stages & comparisonIntersections) {
            if (!(stages & comparisonTopology)) { inspectSurfaceTopology(surface, mesh, topologyMode, stop); }
            surface.intersections = inspectIntersections(surface.topology, {true, true}, stop);
            for (const auto& finding : surface.intersections.findings) {
                checkCanceled(stop);
                DiagnosticEdge bounds{finding.geometry[0], finding.geometry[0]};
                for (const auto& p : finding.geometry) {
                    for (size_t k = 0; k < 3; ++k) { bounds.a[k] = std::min(bounds.a[k], p[k]); bounds.b[k] = std::max(bounds.b[k], p[k]); }
                }
                surface.intersectionBounds.push_back(bounds);
                for (size_t i = 0; i < 6; i += 3) {
                    for (size_t k = 0; k < 3; ++k) { surface.intersectionEdges.push_back({finding.geometry[i+k], finding.geometry[i+(k+1)%3]}); }
                }
            }
        }
        if (stages & comparisonDegenerates) { degenerates.enabled = true; inspectSurfaceDegenerates(surface, mesh, degenerates, stop); }
        if (stages & comparisonQuality) { surface.quality = inspectSurfaceMeshQuality(mesh, stop); }
        if (stages & (comparisonDuplicatePoints | comparisonDuplicateTriangles)) {
            DuplicateInput input;
            if (mesh.duplicateInput) { input = *mesh.duplicateInput; }
            input.settings.points = (stages & comparisonDuplicatePoints) != 0;
            input.settings.triangles = (stages & comparisonDuplicateTriangles) != 0;
            if (mesh.duplicateInput) { surface.duplicates = inspectDuplicates(input, stop); }
            else {
                surface.duplicates.points.unavailableSources = 1;
                surface.duplicates.triangles.unavailableSources = 1;
            }
            if (stages & comparisonDuplicatePoints) { surface.duplicatePointBounds = duplicateBounds(surface.duplicates.points, stop); }
            if (stages & comparisonDuplicateTriangles) { surface.duplicateTriangleBounds = duplicateBounds(surface.duplicates.triangles, stop); }
        }
    };
    inspect(original, result.original);
    inspect(repaired, result.repaired);
    completeComparisonDetectors(result, stages);
    if (stages & comparisonQuality) {
        result.qualityDistributions = surfaceQualityDistributions(result.original.quality, result.repaired.quality, stop);
    }
    checkCanceled(stop);
    return result;
}

bool applyComparisonStages(MeshComparison& result, ComparisonCacheStatus& cache, MeshComparison update,
    uint64_t signature, uint32_t stages)
{
    if (!signature || signature != cache.signature) { return false; }
    if (stages & comparisonTopology) {
        for (const auto* surface : {&update.original, &update.repaired}) {
            if ((surface->topology.availableSources || surface->topology.unavailableSources)
                && surface->topology.mode != cache.topologyMode) { stages &= ~comparisonTopology; break; }
        }
        if (!stages) { return false; }
    }
    if (stages & comparisonDegenerates) {
        for (const auto* surface : {&update.original, &update.repaired}) {
            if ((surface->degenerates.availableSources || surface->degenerates.unavailableSources)
                && !sameDegenerateThresholds(surface->degenerates.settings, cache.degenerates)) { stages &= ~comparisonDegenerates; break; }
        }
        if (!stages) { return false; }
    }
    if (stages & comparisonIntersections) {
        for (const auto* surface : {&update.original, &update.repaired}) {
            if ((surface->intersections.availableSources || surface->intersections.unavailableSources)
                && surface->intersections.mode != cache.topologyMode) { stages &= ~comparisonIntersections; break; }
        }
        if (!stages) { return false; }
    }
    const auto merge = [&](SurfaceComparison& target, SurfaceComparison& source) {
        if (stages & comparisonIntersections) {
            target.intersections = std::move(source.intersections);
            target.intersectionBounds = std::move(source.intersectionBounds);
            target.intersectionEdges = std::move(source.intersectionEdges);
        }
        if (stages & comparisonSource) { target.source = std::move(source.source); }
        if (stages & comparisonTopology) {
            target.diagnostics = std::move(source.diagnostics);
            target.topology = std::move(source.topology);
            target.topologyBoundaries = std::move(source.topologyBoundaries);
            target.topologyNonManifold = std::move(source.topologyNonManifold);
            target.topologyWinding = std::move(source.topologyWinding);
            target.nonManifoldVertexBounds = std::move(source.nonManifoldVertexBounds);
            target.nonManifoldVertexMarkers = std::move(source.nonManifoldVertexMarkers);
            target.holeBounds = std::move(source.holeBounds);
            target.holeEdges = std::move(source.holeEdges);
        }
        if (stages & comparisonQuality) { target.quality = std::move(source.quality); }
        if (stages & comparisonDegenerates) {
            target.degenerates = std::move(source.degenerates);
            target.degenerateBounds = std::move(source.degenerateBounds);
        }
        if (stages & comparisonDuplicatePoints) {
            target.duplicates.points = std::move(source.duplicates.points);
            target.duplicatePointBounds = std::move(source.duplicatePointBounds);
        }
        if (stages & comparisonDuplicateTriangles) {
            target.duplicates.triangles = std::move(source.duplicates.triangles);
            target.duplicateTriangleBounds = std::move(source.duplicateTriangleBounds);
        }
        if (stages & comparisonDistance) {
            target.sampled = std::move(source.sampled);
            target.distances = std::move(source.distances);
            target.sampleAreas = std::move(source.sampleAreas);
            target.maximum = source.maximum;
            target.mean = source.mean;
            target.percentile95 = source.percentile95;
        }
    };
    merge(result.original, update.original);
    merge(result.repaired, update.repaired);
    for (size_t i = 0; i < result.detectors.size(); ++i) {
        if (stages & comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i))) { result.detectors[i] = std::move(update.detectors[i]); }
    }
    if (stages & comparisonQuality) { result.qualityDistributions = std::move(update.qualityDistributions); }
    cache.completed |= stages;
    return true;
}

void setComparisonIntersectionSettings(MeshComparison& result, IntersectionSettings settings)
{
    result.original.intersections.settings = result.repaired.intersections.settings = settings;
}

void setComparisonDegenerateSettings(MeshComparison& result, DegenerateSettings settings)
{
    settings = normalizedDegenerateSettings(settings);
    for (auto* surface : {&result.original, &result.repaired}) {
        surface->degenerates.settings.enabled = settings.enabled;
        surface->degenerates.settings.show = settings.show;
        // Disabled responses still describe the current configured thresholds.
        // The cache separately invalidates findings when these thresholds change.
        if (!settings.enabled) { surface->degenerates.settings = settings; }
    }
}

bool setComparisonTopologyInspectionSettings(MeshComparison& result, TopologyInspectionSettings settings)
{
    settings = normalizedTopologyInspectionSettings(settings);
    bool geometryChanged = false;
    for (auto* surface : {&result.original, &result.repaired}) {
        if (surface->topology.inspection.holeSizeRatioTolerance == settings.holeSizeRatioTolerance) {
            surface->topology.inspection = settings;
            continue;
        }
        if (filterTopologyFindings(surface->topology, settings)) {
            prepareTopologyInspectionGeometry(*surface, {}, true);
            geometryChanged = true;
        }
    }
    return geometryChanged;
}

void setComparisonDuplicateEnabled(MeshComparison& result, const DuplicateSettings& settings)
{
    for (auto* surface : {&result.original, &result.repaired}) {
        surface->duplicates.points.enabled = settings.points;
        surface->duplicates.triangles.enabled = settings.triangles;
    }
}

double surfacePercentAboveTolerance(const SurfaceComparison &surface, double tolerance)
{
    if (!std::isfinite(tolerance) || tolerance < 0)
        throw std::runtime_error("Tolerance must be finite and nonnegative.");
    double total = 0, above = 0;
    for (size_t i = 0; i < surface.distances.size(); ++i)
    {
        const double weight = surface.sampleAreas.at(i);
        total += weight;
        if (surface.distances[i] > tolerance)
            above += weight;
    }
    return total > 0 ? 100 * above / total : 0;
}
} // namespace woby
