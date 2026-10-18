
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
namespace gm_profile {
using Clock=std::chrono::steady_clock;
using Json=nlohmann::json;
struct Counters { uint64_t queries=0, samples=0, nodes=0, angles=0, farNodes=0, fallback=0, inside=0; bool sample=false; };
inline Counters counts;
inline Json report() {return {{"queries",counts.queries},{"sampled_queries",counts.samples},{"sampled_nodes",counts.nodes},{"sampled_angles",counts.angles},{"sampled_far",counts.farNodes},{"exact_fallbacks",counts.fallback},{"inside_queries",counts.inside}};}
struct Scope {
    std::string name; Clock::time_point start=Clock::now();
    explicit Scope(std::string s):name(std::move(s)){std::cerr<<Json({{"event","begin"},{"stage",name}}).dump()<<std::endl;}
    ~Scope(){std::cerr<<Json({{"event","end"},{"stage",name},{"seconds",std::chrono::duration<double>(Clock::now()-start).count()},{"counts",report()}}).dump()<<std::endl;}
};
}
#include "difference_mesh.h"
#include "mesh_intersections.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <manifold/manifold.h>
#include <map>
#include <numeric>
#include <numbers>
#include <random>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace woby
{
namespace
{
using Point = Coordinate;
using Face = std::array<size_t, 3>;
struct Soup {
    std::vector<Point> points;
    std::vector<Face> faces;
};
struct Topology {
    std::vector<std::vector<size_t>> loops;
};
void check(std::stop_token stop)
{
    if (stop.stop_requested()) {
        throw std::runtime_error("Difference mesh canceled.");
    }
}
double distance(const Point &a, const Point &b)
{
    double d = 0;
    for (size_t k = 0; k < 3; ++k) {
        d += (a[k] - b[k]) * (a[k] - b[k]);
    }
    return std::sqrt(d);
}
Soup read(const Mesh &mesh, const Point &origin, size_t &removedCollapsedTriangles, std::stop_token stop)
{
    gm_profile::Scope measured("read");
    Soup s;
    std::map<Point, size_t> unique;
    const auto &g = meshGeometry(mesh);
    if (g.indices.empty() || g.indices.size() % 3) {
        throw std::runtime_error("Input has no valid triangle surface.");
    }
    for (size_t i = 0; i < g.indices.size(); i += 3) {
        check(stop);
        std::array<Point, 3> points;
        for (size_t j = 0; j < 3; ++j) {
            auto &p = points[j];
            p = meshPosition(mesh, g.indices[i + j]);
            for (size_t axis = 0; axis < 3; ++axis) {
                p[axis] += mesh.origin[axis] - origin[axis];
            }
            if (!finiteCoordinate(p)) {
                throw std::runtime_error("Input contains non-finite coordinates.");
            }
        }
        if (exactTriangleCollapsed(points)) {
            ++removedCollapsedTriangles;
            continue;
        }
        // Omitted faces must not leave isolated points in the working solid.
        Face f;
        for (size_t j = 0; j < 3; ++j) {
            const auto &p = points[j];
            const auto [it, added] = unique.emplace(p, s.points.size());
            if (added) {
                s.points.push_back(p);
            }
            f[j] = it->second;
        }
        s.faces.push_back(f);
    }
    if (s.faces.empty()) {
        throw std::runtime_error("No triangles remain after removing " + std::to_string(removedCollapsedTriangles) +
                                 " exact zero-area triangles.");
    }
    return s;
}
Topology validate(const Soup &s, std::stop_token stop)
{
    gm_profile::Scope measured("validate");
    struct Edge {
        size_t a = 0, b = 0, count = 0;
    };
    std::map<std::pair<size_t, size_t>, Edge> edges;
    for (const auto &f : s.faces) {
        check(stop);
        for (size_t j = 0; j < 3; ++j) {
            const auto a = f[j], b = f[(j + 1) % 3];
            auto &e = edges[std::minmax(a, b)];
            if (++e.count == 1) {
                e.a = a;
                e.b = b;
            } else if (e.count > 2 || e.a == a) {
                throw std::runtime_error(
                    "Non-manifold edges or inconsistent triangle winding. Check the input topology.");
            }
        }
    }
    std::map<size_t, size_t> next;
    std::map<size_t, size_t> incoming;
    for (const auto &[key, e] : edges) {
        (void)key;
        if (e.count == 1) {
            if (!next.emplace(e.a, e.b).second || !incoming.emplace(e.b, e.a).second) {
                throw std::runtime_error("Boundary branches at a vertex. Automatic closure is not available.");
            }
        }
    }
    Topology t;
    while (!next.empty()) {
        auto &loop = t.loops.emplace_back();
        const auto start = next.begin()->first;
        auto p = start;
        do {
            check(stop);
            loop.push_back(p);
            const auto it = next.find(p);
            if (it == next.end()) {
                throw std::runtime_error("Boundary is not a closed loop.");
            }
            p = it->second;
            next.erase(it);
        } while (p != start);
    }
    auto source = std::make_shared<SourceAnalysisGeometry>();
    source->source = "Difference input";
    std::map<Point,size_t> uniquePositions;
    std::vector<size_t> geometricIds;
    for (const auto &p : s.points) {
        const auto [it, added] = uniquePositions.emplace(p, source->vertices.size());
        if (added) source->vertices.push_back({p});
        geometricIds.push_back(it->second);
    }
    for (size_t i = 0; i < s.faces.size(); ++i) {
        const auto &f = s.faces[i];
        source->faces.push_back({{1, 1, i + 1}, {geometricIds[f[0]],geometricIds[f[1]],geometricIds[f[2]]}});
    }
    AnalysisGeometry geometry;
    geometry.availableSources = 1;
    geometry.sources.push_back(source);
    const auto intersections = inspectIntersections(geometry, {}, stop, {1, 5000000});
    if (!intersections.findings.empty()) {
        gm_profile::Json record = {{"event", "intersection"}, {"triangles", s.faces.size()}};
        for (const auto &ref : intersections.findings.front().faces) {
            const auto &f = s.faces.at(ref.triangleId-1);
            record["faces"].push_back({{"id",ref.triangleId},{"indices",f},{"points",{s.points[f[0]],s.points[f[1]],s.points[f[2]]}}});
        }
        std::cerr << record.dump() << std::endl;
        throw std::runtime_error("Surface or closure walls self-intersect. Automatic difference creation stopped.");
    }
    if (intersections.truncated) {
        throw std::runtime_error("Intersection validation exceeded its work limit. Use smaller input meshes.");
    }
    return t;
}
double signedVolume(const Soup &s)
{
    double v = 0;
    for (const auto &f : s.faces) {
        const auto &a = s.points[f[0]], &b = s.points[f[1]], &c = s.points[f[2]];
        v += (a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) +
              a[2] * (b[0] * c[1] - b[1] * c[0])) /
             6;
    }
    return v;
}
manifold::Manifold solid(Soup s)
{
    gm_profile::Scope measured("solid");
    if (signedVolume(s) < 0) {
        for (auto &f : s.faces) {
            std::swap(f[1], f[2]);
        }
    }
    manifold::MeshGL64 mesh;
    mesh.numProp = 3;
    for (const auto &p : s.points) {
        mesh.vertProperties.insert(mesh.vertProperties.end(), p.begin(), p.end());
    }
    for (const auto &f : s.faces) {
        for (auto i : f) {
            mesh.triVerts.push_back(static_cast<uint64_t>(i));
        }
    }
    manifold::Manifold result(mesh);
    if (result.Status() != manifold::Manifold::Error::NoError) {
        throw std::runtime_error("Input does not define a valid manifold solid.");
    }
    return result;
}
Mesh publish(const Soup &s, const Point &origin, const std::string &name, std::stop_token stop)
{
    gm_profile::Scope measured("publish");
    Mesh mesh;
    mesh.origin = origin;
    auto &g = editMeshGeometry(mesh);
    if (s.points.size() > UINT32_MAX || s.faces.size() > UINT32_MAX / 3) {
        throw std::runtime_error("Difference mesh exceeds the supported mesh size.");
    }
    for (const auto &p : s.points) {
        check(stop);
        Vertex v;
        v.position = renderPosition(p);
        g.vertices.push_back(v);
        g.precisePositions.push_back(p);
    }
    for (const auto &f : s.faces) {
        for (auto i : f) {
            g.indices.push_back(static_cast<uint32_t>(i));
        }
    }
    if (!g.indices.empty()) {
        g.nodes.push_back({name, 0, static_cast<uint32_t>(g.indices.size())});
        mesh.bounds = calculateBounds(g.vertices);
        generateSmoothNormals(g.vertices, g.indices);
    }
    publishMeshGeometry(mesh);
    return mesh;
}
Soup output(const manifold::Manifold &solidResult, bool mergeCoincident = false)
{
    gm_profile::Scope measured("output");
    if (solidResult.Status() != manifold::Manifold::Error::NoError) {
        throw std::runtime_error("Solid difference failed. Check the input geometry.");
    }
    const auto mesh = solidResult.GetMeshGL64();
    std::cerr << gm_profile::Json({{"event","manifold_output"},{"status",static_cast<int>(solidResult.Status())},{"epsilon",solidResult.GetEpsilon()},{"tolerance",solidResult.GetTolerance()},{"volume",solidResult.Volume()},{"property_merges",mesh.mergeFromVert.size()},{"vertices",mesh.vertProperties.size()/mesh.numProp},{"triangles",mesh.triVerts.size()/3}}).dump() << std::endl;
    Soup s;
    for (size_t i = 0; i < mesh.vertProperties.size(); i += mesh.numProp) {
        s.points.push_back({mesh.vertProperties[i], mesh.vertProperties[i + 1], mesh.vertProperties[i + 2]});
    }
    if (!mergeCoincident) {
        // Reconstructed components can touch at equal positions while remaining topologically separate.
        for (size_t i = 0; i < mesh.triVerts.size(); i += 3) {
            s.faces.push_back({static_cast<size_t>(mesh.triVerts[i]), static_cast<size_t>(mesh.triVerts[i + 1]),
                               static_cast<size_t>(mesh.triVerts[i + 2])});
        }
        return s;
    }
    // Merge property seams before validating/exporting.
    std::map<Point, size_t> unique;
    std::vector<size_t> map;
    std::vector<Point> points;
    for (const auto &p : s.points) {
        const auto [it, added] = unique.emplace(p, points.size());
        if (added) {
            points.push_back(p);
        }
        map.push_back(it->second);
    }
    std::cerr << gm_profile::Json({{"event","output_welding"},{"before",s.points.size()},{"after",points.size()}}).dump() << std::endl;
    s.points = std::move(points);
    for (size_t i = 0; i < mesh.triVerts.size(); i += 3) {
        s.faces.push_back({map.at(mesh.triVerts[i]), map.at(mesh.triVerts[i + 1]), map.at(mesh.triVerts[i + 2])});
    }
    return s;
}
Point subtract(const Point &a, const Point &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
double dot(const Point &a, const Point &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
Point cross(const Point &a, const Point &b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
struct WindingNode {
    Point center{}, areaVector{};
    double radius = 0, area = 0;
    size_t begin = 0, end = 0, left = 0, right = 0;
};
struct WindingTree {
    Soup soup;
    std::vector<WindingNode> nodes;
};
size_t buildWindingNode(WindingTree &tree, size_t begin, size_t end, std::stop_token stop)
{
    check(stop);
    WindingNode node;
    node.begin = begin;
    node.end = end;
    auto low = tree.soup.points[tree.soup.faces[begin][0]], high = low;
    for (size_t i = begin; i < end; ++i) {
        check(stop);
        const auto &f = tree.soup.faces[i];
        const auto normal = cross(subtract(tree.soup.points[f[1]], tree.soup.points[f[0]]),
                                  subtract(tree.soup.points[f[2]], tree.soup.points[f[0]]));
        node.area += .5 * std::sqrt(dot(normal, normal));
        for (size_t k = 0; k < 3; ++k) {
            node.areaVector[k] += .5 * normal[k];
            for (auto index : f) {
                low[k] = std::min(low[k], tree.soup.points[index][k]);
                high[k] = std::max(high[k], tree.soup.points[index][k]);
            }
        }
    }
    for (size_t k = 0; k < 3; ++k) { node.center[k] = .5 * (low[k] + high[k]); }
    node.radius = .5 * distance(low, high);
    const auto index = tree.nodes.size();
    tree.nodes.push_back(node);
    if (end - begin > 12) {
        size_t axis = 0;
        for (size_t k = 1; k < 3; ++k) {
            if (high[k] - low[k] > high[axis] - low[axis]) { axis = k; }
        }
        const auto middle = begin + (end - begin) / 2;
        const auto centroid = [&](const Face &f) {
            return tree.soup.points[f[0]][axis] + tree.soup.points[f[1]][axis] + tree.soup.points[f[2]][axis];
        };
        std::nth_element(tree.soup.faces.begin() + static_cast<ptrdiff_t>(begin),
                         tree.soup.faces.begin() + static_cast<ptrdiff_t>(middle),
                         tree.soup.faces.begin() + static_cast<ptrdiff_t>(end),
                         [&](const Face &a, const Face &b) { return centroid(a) < centroid(b); });
        const auto left = buildWindingNode(tree, begin, middle, stop);
        const auto right = buildWindingNode(tree, middle, end, stop);
        tree.nodes[index].left = left;
        tree.nodes[index].right = right;
    }
    return index;
}
double solidAngle(const Soup &soup, const Face &face, const Point &point)
{
    if (gm_profile::counts.sample) ++gm_profile::counts.angles;
    std::array<Point, 3> directions;
    for (size_t i = 0; i < 3; ++i) {
        directions[i] = subtract(soup.points[face[i]], point);
        const auto length = std::sqrt(dot(directions[i], directions[i]));
        if (length == 0) { return 0; } // The surface has no unique interior at a vertex.
        for (auto &component : directions[i]) { component /= length; }
    }
    const auto &a = directions[0], &b = directions[1], &c = directions[2];
    const auto numerator = dot(a, cross(b, c));
    // atan2(0, negative) is pi. Coplanar triangles must not invent an interior for a sheet.
    if (numerator == 0) { return 0; }
    return 2 * std::atan2(numerator, 1 + dot(a, b) + dot(b, c) + dot(c, a));
}
struct WindingEstimate { double angle = 0, error = 0; };
WindingEstimate winding(const WindingTree &tree, size_t index, const Point &point, bool exact, std::stop_token stop)
{
    check(stop);
    if (gm_profile::counts.sample) ++gm_profile::counts.nodes;
    const auto &node = tree.nodes[index];
    const auto offset = subtract(node.center, point);
    const auto d = std::sqrt(dot(offset, offset));
    if (!exact && node.left && d > 4 * node.radius) {
        // Integral of n dot r/|r|^3. The derivative norm is at most 2/|r|^3.
        // Bound the omitted terms so classification can fall back to exact solid angles.
        if (gm_profile::counts.sample) ++gm_profile::counts.farNodes;
        const auto separation = d - node.radius;
        return {dot(node.areaVector, offset) / (d * d * d),
                2 * node.area * node.radius / (separation * separation * separation)};
    }
    if (node.left) {
        const auto a = winding(tree, node.left, point, exact, stop),
                   b = winding(tree, node.right, point, exact, stop);
        return {a.angle + b.angle, a.error + b.error};
    }
    WindingEstimate result;
    for (size_t i = node.begin; i < node.end; ++i) { result.angle += solidAngle(tree.soup, tree.soup.faces[i], point); }
    return result;
}
bool inside(const WindingTree &tree, const Point &point, std::stop_token stop)
{
    ++gm_profile::counts.queries;
    gm_profile::counts.sample = (gm_profile::counts.queries % 1024 == 1);
    gm_profile::counts.samples += gm_profile::counts.sample;
    if (gm_profile::counts.queries % 1024 == 0) std::cerr << gm_profile::report().dump() << std::endl;
    auto estimate = winding(tree, 0, point, false, stop);
    // Absolute winding accepts a globally reversed mesh while preserving oriented cavities.
    // Exclude the ambiguous half-winding of a point exactly on an open sheet.
    constexpr double threshold = (0.5 + 1e-8) * 4 * std::numbers::pi;
    if (std::abs(estimate.angle) - estimate.error > threshold) { return true; }
    if (std::abs(estimate.angle) + estimate.error < threshold) { return false; }
    ++gm_profile::counts.fallback;
    estimate = winding(tree, 0, point, true, stop);
    return std::abs(estimate.angle) > threshold;
}
DifferenceMesh approximate(Soup a, Soup b, Point origin, double cellSize, std::stop_token stop)
{
    gm_profile::Scope measured("approximate");
    if (!std::isfinite(cellSize) || cellSize <= 0) {
        throw std::invalid_argument("Approximate volume cell size must be finite and positive.");
    }
    auto low = a.points.front(), high = low;
    for (const auto *s : {&a, &b}) {
        for (const auto &p : s->points) {
            for (size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], p[k]);
                high[k] = std::max(high[k], p[k]);
            }
        }
    }
    // Bound the BCC lattice allocation, including the library's power-of-two index padding.
    // Both inputs use this same box and spacing. Coordinates are near zero in cell units.
    constexpr double maxGridPoints = 8'000'000;
    double gridPoints = 2;
    Point size{};
    for (size_t k = 0; k < 3; ++k) {
        size[k] = std::ceil((high[k] - low[k]) / cellSize) + 4;
        if (!std::isfinite(size[k]) || size[k] > maxGridPoints || cellSize <
            std::max(std::abs(low[k]), std::abs(high[k])) * std::numeric_limits<double>::epsilon() * 8) {
            throw std::runtime_error("Cell size is too small for the input extent or coordinate precision. Use larger cells.");
        }
        gridPoints *= std::exp2(std::floor(std::log2(size[k] + 3)) + 1);
        if (gridPoints > maxGridPoints) {
            throw std::runtime_error("Approximate volume exceeds the grid limit (8 million samples). Use larger cells or smaller input groups.");
        }
    }
    std::cerr << gm_profile::Json({{"event","grid"},{"padded_samples",gridPoints},{"size",size},{"cell_size",cellSize}}).dump() << std::endl;
    for (auto *s : {&a, &b}) {
        for (auto &p : s->points) {
            check(stop);
            for (size_t k = 0; k < 3; ++k) { p[k] = (p[k] - low[k]) / cellSize + 2; }
        }
    }
    for (size_t k = 0; k < 3; ++k) { origin[k] += low[k] - 2 * cellSize; }
    const manifold::Box bounds({0, 0, 0}, {size[0], size[1], size[2]});
    const auto reconstruct = [&](Soup soup, const char *name) {
        WindingTree tree{std::move(soup), {}};
        gm_profile::counts = {};
        gm_profile::Scope reconstructed(std::string("reconstruct_") + name);
        { gm_profile::Scope building("build_winding_tree");
        (void)buildWindingNode(tree, 0, tree.soup.faces.size(), stop); }
        // Sequential callbacks keep cancellation exceptions on this worker thread.
        gm_profile::Scope lattice("level_set");
        auto result = manifold::Manifold::LevelSet([&](manifold::vec3 p) {
            const bool isInside = inside(tree, {p.x, p.y, p.z}, stop);
            gm_profile::counts.inside += isInside;
            return isInside ? 1.0 : -1.0;
        }, bounds, 1.0, 0, -1, false);
        check(stop);
        if (result.Status() != manifold::Manifold::Error::NoError || result.IsEmpty()) {
            throw std::runtime_error(std::string("Input ") + name +
                ": No solid volume was found at this cell size. Use smaller cells and check surface orientation. Open sheets need a separate closure rule.");
        }
        return result;
    };
    const auto sa = reconstruct(std::move(a), "A"), sb = reconstruct(std::move(b), "B");
    const std::array<manifold::Manifold, 2> differences{(sa - sb).Simplify(), (sb - sa).Simplify()};
    DifferenceMesh result;
    for (size_t i = 0; i < 2; ++i) {
        check(stop);
        auto soup = output(differences[i], false);
        for (auto &p : soup.points) { for (auto &component : p) { component *= cellSize; } }
        result.volumes[i] = std::abs(differences[i].Volume()) * cellSize * cellSize * cellSize;
        if (!std::isfinite(result.volumes[i])) { throw std::runtime_error("Approximate volume is outside the supported numeric range."); }
        result.parts[i] = publish(soup, origin, i == 0 ? "A_only" : "B_only", stop);
    }
    result.message = "Approximate volume. Missing surfaces are inferred from triangle orientation. Thin features can be lost. Source meshes are unchanged.";
    return result;
}
std::vector<double> arc(const Soup &s, const std::vector<size_t> &loop)
{
    std::vector<double> a{0};
    for (size_t i = 0; i < loop.size(); ++i) {
        a.push_back(a.back() + distance(s.points[loop[i]], s.points[loop[(i + 1) % loop.size()]]));
    }
    if (a.back() == 0) {
        throw std::runtime_error("Boundary has zero length.");
    }
    const auto total = a.back();
    for (auto &v : a) {
        v /= total;
    }
    return a;
}
Point sample(const Soup &s, const std::vector<size_t> &loop, const std::vector<double> &a, double t)
{
    size_t i = static_cast<size_t>(std::upper_bound(a.begin(), a.end(), t) - a.begin() - 1);
    i = std::min(i, loop.size() - 1);
    const auto &p = s.points[loop[i]], &q = s.points[loop[(i + 1) % loop.size()]];
    const double f = (t - a[i]) / (a[i + 1] - a[i]);
    return {p[0] + f * (q[0] - p[0]), p[1] + f * (q[1] - p[1]), p[2] + f * (q[2] - p[2])};
}
DifferenceMesh closeSurfaces(Soup a, Soup b, std::vector<size_t> la, std::vector<size_t> lb, const Point &origin,
                             std::stop_token stop)
{
    // Choose one deterministic anchor, then compare both loop directions using arc length.
    const auto anchor = std::min_element(lb.begin(), lb.end(), [&](size_t x, size_t y) {
        return distance(a.points[la[0]], b.points[x]) < distance(a.points[la[0]], b.points[y]);
    });
    std::rotate(lb.begin(), anchor, lb.end());
    auto reverse = lb;
    std::reverse(reverse.begin() + 1, reverse.end());
    const auto aa = arc(a, la);
    const auto score = [&](const auto &loop) {
        const auto ab = arc(b, loop);
        double result = 0;
        for (int i = 0; i < 64; ++i) {
            check(stop);
            result += distance(sample(a, la, aa, i / 64.0), sample(b, loop, ab, i / 64.0));
        }
        return result;
    };
    const double forwardScore = score(lb), reverseScore = score(reverse);
    if (std::abs(forwardScore - reverseScore) <= std::max(forwardScore, reverseScore) * 1e-8) {
        throw std::runtime_error("Boundary correspondence is ambiguous. Automatic closure stopped.");
    }
    const bool reverseB = reverseScore < forwardScore;
    if (reverseB) {
        lb = std::move(reverse);
    }
    // A and B boundary directions must be opposite in the completed shell.
    if (!reverseB) {
        for (auto &f : b.faces) {
            std::swap(f[1], f[2]);
        }
    }
    Soup shell = a;
    const size_t offset = a.points.size();
    shell.points.insert(shell.points.end(), b.points.begin(), b.points.end());
    for (auto f : b.faces) {
        for (auto &i : f) {
            i += offset;
        }
        shell.faces.push_back(f);
    }
    const size_t surfaceFaces = shell.faces.size();
    const auto ab = arc(b, lb);
    size_t i = 0, j = 0;
    while (i < la.size() || j < lb.size()) {
        check(stop);
        const auto av = la[i % la.size()], bv = lb[j % lb.size()] + offset;
        if (j == lb.size() || (i < la.size() && aa[i + 1] <= ab[j + 1])) {
            shell.faces.push_back({la[(i + 1) % la.size()], av, bv});
            ++i;
        } else {
            shell.faces.push_back({av, bv, lb[(j + 1) % lb.size()] + offset});
            ++j;
        }
    }
    const auto topology = validate(shell, stop);
    if (!topology.loops.empty()) {
        throw std::runtime_error("Automatic closure did not form a closed volume.");
    }
    const auto closed = solid(shell);
    const double volume = closed.Volume();
    if (!(volume > 0)) {
        throw std::runtime_error("The surface pair does not enclose a positive volume.");
    }
    if (signedVolume(shell) < 0) {
        for (auto &f : shell.faces) {
            std::swap(f[1], f[2]);
        }
    }
    DifferenceMesh result;
    result.constructed = true;
    result.volumes[0] = volume;
    result.closureTriangles = shell.faces.size() - surfaceFaces;
    Soup walls;
    walls.points = shell.points;
    walls.faces.assign(shell.faces.begin() + static_cast<std::ptrdiff_t>(surfaceFaces), shell.faces.end());
    shell.faces.resize(surfaceFaces);
    result.parts[0] = publish(shell, origin, "Gap_surfaces", stop);
    result.parts[2] = publish(walls, origin, "Generated_closure_walls", stop);
    result.message = "Constructed gap volume. Generated closure walls are an assumption.";
    return result;
}
} // namespace
DifferenceMesh buildDifferenceMesh(const Mesh &first, const Mesh &second, std::stop_token stop)
{
    return buildDifferenceMesh(first, second, DifferenceSettings{}, stop);
}
bool sameDifferenceConstruction(const DifferenceSettings &a, const DifferenceSettings &b)
{
    return a.method == b.method && (a.method == DifferenceMethod::direct || a.cellSize == b.cellSize);
}
const char *differenceMethodName(DifferenceMethod method)
{
    return method == DifferenceMethod::approximate ? "approximate" : "direct";
}
DifferenceMethod parseDifferenceMethod(const std::string &name)
{
    if (name == "direct") { return DifferenceMethod::direct; }
    if (name == "approximate") { return DifferenceMethod::approximate; }
    throw std::invalid_argument("Difference method must be direct or approximate.");
}
DifferenceMesh buildDifferenceMesh(const Mesh &first, const Mesh &second, const DifferenceSettings &settings,
                                   std::stop_token stop)
{
    if (settings.method != DifferenceMethod::direct && settings.method != DifferenceMethod::approximate) {
        throw std::invalid_argument("Unknown difference method.");
    }
    const auto origin = first.origin;
    Soup a, b;
    Topology ta, tb;
    std::array<size_t, 2> removed{};
    try {
        a = read(first, origin, removed[0], stop);
        if (settings.method == DifferenceMethod::direct) { ta = validate(a, stop); }
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Input A: ") + e.what());
    }
    try {
        b = read(second, origin, removed[1], stop);
        if (settings.method == DifferenceMethod::direct) { tb = validate(b, stop); }
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Input B: ") + e.what());
    }
    DifferenceMesh result;
    if (settings.method == DifferenceMethod::approximate) {
        result = approximate(std::move(a), std::move(b), origin, settings.cellSize, stop);
    } else if (!ta.loops.empty() || !tb.loops.empty()) {
        if (ta.loops.size() != 1 || tb.loops.size() != 1) {
            throw std::runtime_error(
                "Automatic closure needs one outer boundary on each input, without holes. Found A: " +
                std::to_string(ta.loops.size()) + ", B: " + std::to_string(tb.loops.size()) + " loops.");
        }
        result = closeSurfaces(std::move(a), std::move(b), ta.loops[0], tb.loops[0], origin, stop);
    } else {
        check(stop);
        const auto sa = solid(std::move(a)), sb = solid(std::move(b));
        const std::array<manifold::Manifold, 2> differences{(sa - sb).Simplify(), (sb - sa).Simplify()};
        for (size_t i = 0; i < 2; ++i) {
            check(stop);
            auto soup = output(differences[i]);
            if (!soup.faces.empty()) {
                if (!validate(soup, stop).loops.empty()) {
                    throw std::runtime_error("Boolean output is not closed.");
                }
            }
            result.volumes[i] = std::abs(differences[i].Volume());
            result.parts[i] = publish(soup, origin, i == 0 ? "A_only" : "B_only", stop);
        }
        result.message = result.volumes[0] + result.volumes[1] == 0 ? "No geometric difference."
                                                                    : "A only: removed from A. B only: added in B.";
    }
    check(stop);
    result.construction.method = settings.method;
    result.construction.cellSize = settings.cellSize;
    result.removedCollapsedTriangles = removed;
    if (removed[0] || removed[1]) {
        result.message += " Removed exact zero-area input triangles: A: " + std::to_string(removed[0]) +
                          ", B: " + std::to_string(removed[1]) + ". Source meshes are unchanged.";
    }
    return result;
}
void exportDifferenceMesh(const DifferenceMesh &result, const std::filesystem::path &path, std::stop_token stop)
{
    if (!path.is_absolute() || path.extension() != ".obj") {
        throw std::runtime_error("Difference export requires an absolute .obj path.");
    }
    if (std::filesystem::exists(path)) {
        throw std::runtime_error("Export file already exists.");
    }
    std::filesystem::path temporaryDirectory;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto candidate =
            path.parent_path() / (".woby-difference-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            temporaryDirectory = std::move(candidate);
            break;
        }
    }
    if (temporaryDirectory.empty()) {
        throw std::runtime_error("Cannot create temporary difference export.");
    }
    const auto temporaryPath = temporaryDirectory / "mesh.obj";
    std::ofstream file;
    try {
        file.open(temporaryPath);
        if (!file) {
            throw std::runtime_error("Cannot create difference export.");
        }
        file << std::setprecision(17) << "# Woby difference mesh\n# " << result.message << '\n';
        size_t offset = 1;
        for (size_t part = 0; part < result.parts.size(); ++part) {
            const auto &m = result.parts[part];
            const auto &g = meshGeometry(m);
            if (g.indices.empty()) {
                continue;
            }
            file << "g "
                 << (part == 2            ? "Generated_closure_walls"
                     : result.constructed ? "Gap_surfaces"
                     : part == 0          ? "A_only"
                                          : "B_only")
                 << '\n';
            for (size_t i = 0; i < g.vertices.size(); ++i) {
                check(stop);
                const auto p = originalPosition(meshPosition(m, i), m.origin);
                file << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
            }
            for (size_t i = 0; i < g.indices.size(); i += 3) {
                check(stop);
                file << "f " << offset + g.indices[i] << ' ' << offset + g.indices[i + 1] << ' '
                     << offset + g.indices[i + 2] << '\n';
            }
            offset += g.vertices.size();
        }
        file.close();
        if (!file) {
            throw std::runtime_error("Could not finish difference export.");
        }
        check(stop);
#ifdef _WIN32
        if (!MoveFileExW(temporaryPath.c_str(), path.c_str(), 0)) {
            throw std::filesystem::filesystem_error(
                "Cannot install difference export", path,
                std::error_code(static_cast<int>(GetLastError()), std::system_category()));
        }
#else
        if (::link(temporaryPath.c_str(), path.c_str()) != 0) {
            throw std::filesystem::filesystem_error("Cannot install difference export", path,
                                                    std::error_code(errno, std::generic_category()));
        }
#endif
    } catch (...) {
        file.close();
        std::error_code error;
        std::filesystem::remove(temporaryPath, error);
        std::filesystem::remove(temporaryDirectory, error);
        throw;
    }
    std::error_code error;
    std::filesystem::remove(temporaryPath, error);
    std::filesystem::remove(temporaryDirectory, error);
}
} // namespace woby
