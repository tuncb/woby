#include "mesh_comparison.h"
#include "comparison_settings.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace woby
{
namespace
{
using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;
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

std::vector<Triangle> meshTriangles(const Mesh &mesh)
{
    if (mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        throw std::runtime_error("Comparison needs nonempty triangular meshes.");
    if (mesh.indices.size() / 3 > comparisonTriangleLimit)
        throw std::runtime_error("Prototype comparison supports up to 50,000 triangles per file.");
    std::vector<Triangle> triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (size_t i = 0; i < mesh.indices.size(); i += 3)
    {
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

struct DistanceNode
{
    Point minimum{}, maximum{};
    size_t begin = 0, end = 0, left = 0, right = 0;
};
struct DistanceTree
{
    std::stop_token stop;
    std::vector<Triangle> triangles;
    std::vector<size_t> order;
    std::vector<DistanceNode> nodes;
};
size_t buildNode(DistanceTree &tree, size_t begin, size_t end)
{
    if (tree.stop.stop_requested())
    {
        throw std::runtime_error("Comparison canceled.");
    }
    DistanceNode node;
    node.minimum.fill(std::numeric_limits<double>::infinity());
    node.maximum.fill(-std::numeric_limits<double>::infinity());
    node.begin = begin;
    node.end = end;
    for (size_t i = begin; i < end; ++i)
        for (const auto &p : tree.triangles[tree.order[i]])
            for (size_t axis = 0; axis < 3; ++axis)
            {
                node.minimum[axis] = std::min(node.minimum[axis], p[axis]);
                node.maximum[axis] = std::max(node.maximum[axis], p[axis]);
            }
    const size_t index = tree.nodes.size();
    tree.nodes.push_back(node);
    if (end - begin > 8)
    {
        size_t axis = 0;
        for (size_t k = 1; k < 3; ++k)
            if (node.maximum[k] - node.minimum[k] > node.maximum[axis] - node.minimum[axis])
                axis = k;
        const size_t middle = begin + (end - begin) / 2;
        std::nth_element(tree.order.begin() + static_cast<std::ptrdiff_t>(begin),
                         tree.order.begin() + static_cast<std::ptrdiff_t>(middle),
                         tree.order.begin() + static_cast<std::ptrdiff_t>(end), [&](size_t a, size_t b) {
                             const auto &ta = tree.triangles[a];
                             const auto &tb = tree.triangles[b];
                             const double ca = ta[0][axis] + ta[1][axis] + ta[2][axis],
                                          cb = tb[0][axis] + tb[1][axis] + tb[2][axis];
                             return ca == cb ? a < b : ca < cb;
                         });
        const size_t left = buildNode(tree, begin, middle), right = buildNode(tree, middle, end);
        tree.nodes[index].left = left;
        tree.nodes[index].right = right;
    }
    return index;
}
DistanceTree buildTree(const Mesh &mesh, std::stop_token stop)
{
    DistanceTree tree;
    tree.stop = stop;
    tree.triangles = meshTriangles(mesh);
    tree.order.resize(tree.triangles.size());
    std::iota(tree.order.begin(), tree.order.end(), size_t{0});
    tree.nodes.reserve(tree.order.size() * 2);
    buildNode(tree, 0, tree.order.size());
    return tree;
}
double boxSquared(const Point &p, const DistanceNode &node)
{
    double result = 0;
    for (size_t k = 0; k < 3; ++k)
    {
        const double d = p[k] - std::clamp(p[k], node.minimum[k], node.maximum[k]);
        result += d * d;
    }
    return result;
}
void nearest(const DistanceTree &tree, size_t index, const Point &p, double &best)
{
    if (tree.stop.stop_requested())
    {
        throw std::runtime_error("Comparison canceled.");
    }
    if (best == 0)
    {
        return;
    }
    const auto &node = tree.nodes[index];
    if (boxSquared(p, node) > best)
        return;
    if (node.left == 0)
    {
        for (size_t i = node.begin; i < node.end; ++i)
            best = std::min(best, triangleSquared(p, tree.triangles[tree.order[i]]));
    }
    else
    {
        size_t first = node.left, second = node.right;
        if (boxSquared(p, tree.nodes[first]) > boxSquared(p, tree.nodes[second]))
            std::swap(first, second);
        nearest(tree, first, p, best);
        nearest(tree, second, p, best);
    }
}

SurfaceComparison compareSurface(const Mesh &mesh, const DistanceTree &source, const DistanceTree &target)
{
    SurfaceComparison result;
    result.source = mesh;
    result.diagnostics = inspectMesh(mesh);
    result.sampled.vertices.reserve(source.triangles.size() * 12);
    result.sampled.indices.reserve(source.triangles.size() * 12);
    double weightedDistance = 0, totalArea = 0;
    for (const auto &t : source.triangles)
    {
        const Point ab = mul(add(t[0], t[1]), .5), bc = mul(add(t[1], t[2]), .5), ca = mul(add(t[2], t[0]), .5);
        const std::array<Triangle, 4> parts = {Triangle{t[0], ab, ca}, Triangle{ab, t[1], bc}, Triangle{ca, bc, t[2]},
                                               Triangle{ab, bc, ca}};
        for (const auto &part : parts)
        {
            const Point center = mul(add(add(part[0], part[1]), part[2]), 1.0 / 3.0);
            double squared = std::numeric_limits<double>::infinity();
            nearest(target, 0, center, squared);
            const double distance = std::sqrt(squared), weight = area(part);
            if (distance > std::numeric_limits<float>::max())
                throw std::runtime_error("Comparison distance exceeds the supported display range.");
            result.distances.push_back(distance);
            result.sampleAreas.push_back(weight);
            result.maximum = std::max(result.maximum, distance);
            weightedDistance += distance * weight;
            totalArea += weight;
            const auto normal = cross(sub(part[1], part[0]), sub(part[2], part[0]));
            const double normalLength = std::sqrt(lengthSquared(normal));
            for (const auto &p : part)
            {
                Vertex vertex;
                for (size_t k = 0; k < 3; ++k)
                {
                    vertex.position[k] = static_cast<float>(p[k]);
                    vertex.normal[k] = normalLength > 0 ? static_cast<float>(normal[k] / normalLength) : 0.0f;
                }
                vertex.texcoord[0] = static_cast<float>(distance);
                result.sampled.indices.push_back(static_cast<uint32_t>(result.sampled.vertices.size()));
                result.sampled.vertices.push_back(vertex);
            }
        }
    }
    result.sampled.bounds = calculateBounds(result.sampled.vertices);
    result.sampled.nodes.push_back({"Comparison", 0, static_cast<uint32_t>(result.sampled.indices.size())});
    if (totalArea > 0)
    {
        result.mean = weightedDistance / totalArea;
        std::vector<size_t> sorted(result.distances.size());
        std::iota(sorted.begin(), sorted.end(), size_t{0});
        std::stable_sort(sorted.begin(), sorted.end(),
                         [&](size_t a, size_t b) { return result.distances[a] < result.distances[b]; });
        double cumulative = 0;
        for (const size_t index : sorted)
        {
            cumulative += result.sampleAreas[index];
            if (cumulative >= totalArea * .95)
            {
                result.percentile95 = result.distances[index];
                break;
            }
        }
    }
    return result;
}
} // namespace

ComparisonSettings normalizedComparisonSettings(ComparisonSettings settings, size_t fileCount)
{
    if (settings.originalFile < 0 || static_cast<size_t>(settings.originalFile) >= fileCount)
        settings.originalFile = -1;
    if (settings.repairedFile < 0 || static_cast<size_t>(settings.repairedFile) >= fileCount)
        settings.repairedFile = -1;
    if (settings.originalFile < 0 || settings.repairedFile < 0 || settings.originalFile == settings.repairedFile)
        settings.enabled = false;
    if (settings.mode != ComparisonMode::distance && settings.mode != ComparisonMode::original &&
        settings.mode != ComparisonMode::repaired && settings.mode != ComparisonMode::overlay)
        settings.mode = ComparisonMode::distance;
    settings.tolerance = std::isfinite(settings.tolerance) ? std::clamp(settings.tolerance, 0.0f, 1e12f) : .05f;
    settings.colorRange = std::isfinite(settings.colorRange) ? std::clamp(settings.colorRange, 1e-6f, 1e12f) : .5f;
    settings.colorRange = std::max(settings.colorRange, settings.tolerance);
    return settings;
}

double pointTriangleDistance(const std::array<float, 3> &p, const std::array<float, 3> &a,
                             const std::array<float, 3> &b, const std::array<float, 3> &c)
{
    if (!finitePosition(p) || !finitePosition(a) || !finitePosition(b) || !finitePosition(c))
        throw std::runtime_error("Distance requires finite coordinates.");
    return std::sqrt(triangleSquared(toPoint(p), {toPoint(a), toPoint(b), toPoint(c)}));
}

MeshDiagnostics inspectMesh(const Mesh &mesh)
{
    const auto triangles = meshTriangles(mesh);
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

MeshComparison compareMeshes(const Mesh &original, const Mesh &repaired, std::stop_token stop)
{
    if (stop.stop_requested())
    {
        throw std::runtime_error("Comparison canceled.");
    }
    const auto originalTree = buildTree(original, stop), repairedTree = buildTree(repaired, stop);
    return {compareSurface(original, originalTree, repairedTree), compareSurface(repaired, repairedTree, originalTree)};
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
