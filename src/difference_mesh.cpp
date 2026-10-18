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
void check(std::stop_token stop)
{
    if (stop.stop_requested()) {
        throw std::runtime_error("Difference mesh canceled.");
    }
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
void checkClosedTopology(const Soup &s, std::stop_token stop)
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
    for (const auto &[key, e] : edges) {
        (void)key;
        if (e.count != 2) { throw std::runtime_error("Boolean output is not closed."); }
    }
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
    if (mesh.mergeFromVert.empty()) {
        for (size_t i = 0; i < mesh.triVerts.size(); i += 3) {
            s.faces.push_back({static_cast<size_t>(mesh.triVerts[i]), static_cast<size_t>(mesh.triVerts[i + 1]),
                               static_cast<size_t>(mesh.triVerts[i + 2])});
        }
        return s;
    }
    // Recover property seams from Manifold's explicit topology. Equal positions
    // alone can belong to separate components and must not join their edges.
    std::vector<size_t> parents(s.points.size());
    std::iota(parents.begin(), parents.end(), size_t{0});
    const auto root = [&](size_t index) {
        while (parents.at(index) != index) {
            parents[index] = parents.at(parents[index]);
            index = parents[index];
        }
        return index;
    };
    for (size_t i = 0; i < mesh.mergeFromVert.size(); ++i) {
        parents[root(mesh.mergeFromVert[i])] = root(mesh.mergeToVert.at(i));
    }
    std::vector<size_t> map(s.points.size());
    std::vector<size_t> compact(s.points.size(), std::numeric_limits<size_t>::max());
    std::vector<Point> points;
    for (size_t i = 0; i < s.points.size(); ++i) {
        const auto representative = root(i);
        if (compact[representative] == std::numeric_limits<size_t>::max()) {
            compact[representative] = points.size();
            points.push_back(s.points[representative]);
        }
        map[i] = compact[representative];
    }
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
DifferenceMesh approximateMesh(Soup a, Soup b, Point origin, std::stop_token stop)
{
    auto low = a.points.front(), high = low;
    for (const auto *s : {&a, &b}) {
        for (const auto &p : s->points) {
            check(stop);
            for (size_t k = 0; k < 3; ++k) {
                low[k] = std::min(low[k], p[k]);
                high[k] = std::max(high[k], p[k]);
            }
        }
    }
    Point center{};
    for (size_t k = 0; k < 3; ++k) {
        center[k] = std::midpoint(low[k], high[k]);
    }
    // Work near zero to reduce cancellation. This changes neither source mesh.
    for (auto *s : {&a, &b}) {
        for (auto &p : s->points) {
            check(stop);
            p = subtract(p, center);
            if (!finiteCoordinate(p)) { throw std::runtime_error("Input extent exceeds the supported numeric range."); }
        }
    }
    if (!finiteCoordinate(origin)) { throw std::runtime_error("Input origin exceeds the supported numeric range."); }
    const auto makeSolid = [&](Soup soup, const char *name) {
        try {
            // Manifold checks closed, oriented topology. Exact geometric intersection
            // checks are deliberately excluded from this approximate method.
            auto result = solid(std::move(soup));
            if (result.IsEmpty() || !std::isfinite(result.Volume()) || result.Volume() <= 0) {
                throw std::runtime_error("No positive solid volume was found.");
            }
            return result;
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string("Input ") + name + ": " + e.what() +
                " Difference mesh requires closed solids.");
        }
    };
    check(stop);
    const auto sa = makeSolid(std::move(a), "A");
    check(stop);
    const auto sb = makeSolid(std::move(b), "B");
    const std::array<double, 2> inputVolumes{sa.Volume(), sb.Volume()};
    // A coarse consistency guard, not an accuracy estimate or a geometric tolerance.
    const double volumeSlack = std::max(inputVolumes[0], inputVolumes[1]) * 1e-7;
    DifferenceMesh result;
    for (size_t i = 0; i < 2; ++i) {
        check(stop);
        try {
            const auto difference = i == 0 ? sa - sb : sb - sa;
            auto soup = output(difference);
            for (auto &p : soup.points) {
                check(stop);
                // Display and picking use the comparison's common input frame.
                // Keep the local calculation origin out of the published mesh.
                for (size_t k = 0; k < 3; ++k) { p[k] += center[k]; }
                if (!finiteCoordinate(p)) { throw std::runtime_error("Output contains non-finite coordinates."); }
            }
            checkClosedTopology(soup, stop);
            const auto volume = difference.Volume();
            if (!std::isfinite(volume) || volume < 0 || volume > inputVolumes[i] + volumeSlack) {
                throw std::runtime_error("Approximate mesh produced an inconsistent volume.");
            }
            result.volumes[i] = volume;
            result.parts[i] = publish(soup, origin, i == 0 ? "A_only" : "B_only", stop);
        } catch (const std::exception &e) {
            throw std::runtime_error(std::string(i == 0 ? "Output A only: " : "Output B only: ") + e.what());
        }
    }
    if (std::abs((result.volumes[0] - result.volumes[1]) - (inputVolumes[0] - inputVolumes[1])) > volumeSlack) {
        throw std::runtime_error("Approximate mesh volumes do not agree with the input volume change.");
    }
    result.message = "Approximate mesh. Surface intersections are accepted without an exact check. "
                     "Volumes are estimates and assume valid closed solids. Source meshes are unchanged.";
    return result;
}
} // namespace
DifferenceMesh buildDifferenceMesh(const Mesh &first, const Mesh &second, std::stop_token stop)
{
    const auto origin = first.origin;
    Soup a, b;
    std::array<size_t, 2> removed{};
    try {
        a = read(first, origin, removed[0], stop);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Input A: ") + e.what());
    }
    try {
        b = read(second, origin, removed[1], stop);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Input B: ") + e.what());
    }
    auto result = approximateMesh(std::move(a), std::move(b), origin, stop);
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
            file << "g " << (part == 0 ? "A_only" : "B_only") << '\n';
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
