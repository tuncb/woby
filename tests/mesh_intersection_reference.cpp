#include "mesh_intersection_reference.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

// Frozen rational clipping classifier from dcee7da. It deliberately bypasses
// all floating-point filters and does not call the production classifier.
namespace intersection_reference {
namespace {
using Exact = boost::multiprecision::cpp_rational;
using Point = std::array<Exact, 3>;
using Triangle = std::array<Point, 3>;
using Position = std::array<double, 3>;
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
}
bool intersects(const std::array<Position, 3>& aPoints, const std::array<Position, 3>& bPoints,
    const std::array<size_t, 3>& aIds, const std::array<size_t, 3>& bIds)
{
    for (const auto* points : {&aPoints, &bPoints}) {
        for (const auto& point : *points) { for (const auto coordinate : point) {
            if (!std::isfinite(coordinate)) { throw std::invalid_argument("Non-finite intersection coordinate."); }
        } }
    }
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
} // namespace intersection_reference
