#include "difference_mesh.h"
#include "mesh_intersections.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <manifold/manifold.h>
#include <map>
#include <numeric>
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
    for (const auto &p : s.points) {
        source->vertices.push_back({p});
    }
    for (size_t i = 0; i < s.faces.size(); ++i) {
        source->faces.push_back({{1, 1, i + 1}, s.faces[i]});
    }
    AnalysisGeometry geometry;
    geometry.availableSources = 1;
    geometry.sources.push_back(source);
    const auto intersections = inspectIntersections(geometry, {}, stop, {1, 5000000});
    if (!intersections.findings.empty()) {
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
Soup output(const manifold::Manifold &solidResult)
{
    if (solidResult.Status() != manifold::Manifold::Error::NoError) {
        throw std::runtime_error("Solid difference failed. Check the input geometry.");
    }
    const auto mesh = solidResult.GetMeshGL64();
    Soup s;
    for (size_t i = 0; i < mesh.vertProperties.size(); i += mesh.numProp) {
        s.points.push_back({mesh.vertProperties[i], mesh.vertProperties[i + 1], mesh.vertProperties[i + 2]});
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
    s.points = std::move(points);
    for (size_t i = 0; i < mesh.triVerts.size(); i += 3) {
        s.faces.push_back({map.at(mesh.triVerts[i]), map.at(mesh.triVerts[i + 1]), map.at(mesh.triVerts[i + 2])});
    }
    return s;
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
    const auto origin = first.origin;
    Soup a, b;
    Topology ta, tb;
    std::array<size_t, 2> removed{};
    try {
        a = read(first, origin, removed[0], stop);
        ta = validate(a, stop);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Input A: ") + e.what());
    }
    try {
        b = read(second, origin, removed[1], stop);
        tb = validate(b, stop);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Input B: ") + e.what());
    }
    DifferenceMesh result;
    if (!ta.loops.empty() || !tb.loops.empty()) {
        if (ta.loops.size() != 1 || tb.loops.size() != 1) {
            throw std::runtime_error(
                "Automatic closure needs one outer boundary on each input, without holes. Found A: " +
                std::to_string(ta.loops.size()) + ", B: " + std::to_string(tb.loops.size()) + " loops.");
        }
        result = closeSurfaces(std::move(a), std::move(b), ta.loops[0], tb.loops[0], origin, stop);
    } else {
        check(stop);
        const auto sa = solid(std::move(a)), sb = solid(std::move(b));
        const std::array<manifold::Manifold, 2> differences{sa - sb, sb - sa};
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
