#include "mesh_intersection_predicates.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cstdint>
#include <limits>

// CDT also instantiates this header in sources with other floating-point
// options. Give this strict implementation its own template symbols.
#define predicates woby_intersection_predicates
#include <predicates.h>
#undef predicates

#if defined(_M_FP_FAST) || defined(__FAST_MATH__)
#error "Exact intersection predicates require strict floating point arithmetic"
#endif
#if defined(WOBY_EXPECT_INTERSECTION_FMA) && !defined(PREDICATES_FAST_FMA)
#error "The intersection FMA contract did not select the FMA implementation"
#endif
static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559
    && std::numeric_limits<double>::radix == 2 && std::numeric_limits<double>::digits == 53);

namespace woby {
namespace {
using Position = std::array<double, 3>;
using Triangle = std::array<Position, 3>;

bool supported(const Triangle& triangle)
{
    // Each nonzero input is a multiple of 2^-252. Degree-three products are
    // multiples of 2^-756; differences are at most 2^201. This conservative
    // interval keeps both expansion and splitter products in normal range.
    // Inspect bits so a subnormal input cannot be mistaken for zero by DAZ.
    constexpr auto lower = std::bit_cast<uint64_t>(0x1p-200);
    constexpr auto upper = std::bit_cast<uint64_t>(0x1p200);
    for (const auto& point : triangle) {
        for (const double value : point) {
            const auto magnitude = std::bit_cast<uint64_t>(value) & 0x7fffffffffffffffULL;
            if (magnitude != 0 && (magnitude < lower || magnitude > upper)) { return false; }
        }
    }
    return true;
}
int sign(double value) { return (value > 0) - (value < 0); }
int orient2(const Position& a, const Position& b, const Position& c, size_t x, size_t y)
{
    return sign(woby_intersection_predicates::adaptive::orient2d(a[x], a[y], b[x], b[y], c[x], c[y]));
}
int orient3(const Position& a, const Position& b, const Position& c, const Position& d)
{
    return sign(woby_intersection_predicates::adaptive::orient3d(a.data(), b.data(), c.data(), d.data()));
}
size_t projection(const Triangle& triangle)
{
    for (size_t axis = 0; axis < 3; ++axis) {
        if (orient2(triangle[0], triangle[1], triangle[2], (axis+1)%3, (axis+2)%3)) { return axis; }
    }
    return 3;
}
bool within(const Position& p, const Position& a, const Position& b, size_t x, size_t y)
{
    return p[x] >= std::min(a[x], b[x]) && p[x] <= std::max(a[x], b[x])
        && p[y] >= std::min(a[y], b[y]) && p[y] <= std::max(a[y], b[y]);
}
bool contained(const Position& p, const Triangle& triangle, size_t axis)
{
    const size_t x = (axis+1)%3, y = (axis+2)%3;
    const int inside = orient2(triangle[0], triangle[1], triangle[2], x, y);
    for (size_t i = 0; i < 3; ++i) {
        const int value = orient2(triangle[i], triangle[(i+1)%3], p, x, y);
        if (value && value != inside) { return false; }
    }
    return true;
}
bool allowed(const Position& p, const Triangle& shared, size_t count)
{
    // The classifier handles two shared vertices before the edge tests.
    return count == 1 && p == shared[0];
}
bool segment2(const Position& a, const Position& b, const Position& c, const Position& d,
    size_t axis, const Triangle& shared, size_t count)
{
    const size_t x = (axis+1)%3, y = (axis+2)%3;
    const int ac = orient2(a,b,c,x,y), ad = orient2(a,b,d,x,y);
    const int ca = orient2(c,d,a,x,y), cb = orient2(c,d,b,x,y);
    if (ac*ad < 0 && ca*cb < 0) { return true; }
    // Every other closed segment intersection contains an original endpoint.
    // These tests also find collinear overlap beyond an allowed common vertex.
    if (!ac && within(c,a,b,x,y) && !allowed(c,shared,count)) { return true; }
    if (!ad && within(d,a,b,x,y) && !allowed(d,shared,count)) { return true; }
    if (!ca && within(a,c,d,x,y) && !allowed(a,shared,count)) { return true; }
    return !cb && within(b,c,d,x,y) && !allowed(b,shared,count);
}
bool segmentTriangle(const Position& a, const Position& b, const Triangle& triangle,
    size_t axis, const Triangle& shared, size_t count)
{
    const int da = orient3(triangle[0],triangle[1],triangle[2],a);
    const int db = orient3(triangle[0],triangle[1],triangle[2],b);
    if (da && da == db) { return false; }
    if (!da && !db) {
        if (contained(a,triangle,axis) && !allowed(a,shared,count)) { return true; }
        if (contained(b,triangle,axis) && !allowed(b,shared,count)) { return true; }
        for (size_t i = 0; i < 3; ++i) {
            if (segment2(a,b,triangle[i],triangle[(i+1)%3],axis,shared,count)) { return true; }
        }
        return false;
    }
    if (!da) { return contained(a,triangle,axis) && !allowed(a,shared,count); }
    if (!db) { return contained(b,triangle,axis) && !allowed(b,shared,count); }
    // Opposite plane signs put the crossing inside the segment. The three
    // tetrahedron signs place that crossing relative to the triangle edges.
    std::array<int, 3> sides{};
    for (size_t i = 0; i < 3; ++i) { sides[i] = orient3(a,b,triangle[i],triangle[(i+1)%3]); }
    return *std::min_element(sides.begin(), sides.end()) >= 0
        || *std::max_element(sides.begin(), sides.end()) <= 0;
}
} // namespace

std::optional<bool> adaptiveTrianglesSelfIntersect(const Triangle& a, const Triangle& b,
    const std::array<size_t, 3>& aIds, const std::array<size_t, 3>& bIds)
{
    // Check on this thread at each call. Strict compilation alone does not
    // establish the runtime rounding mode, and another caller can change it.
    if (std::fegetround() != FE_TONEAREST || !supported(a) || !supported(b)) { return std::nullopt; }
    const size_t ap = projection(a), bp = projection(b);
    if (ap == 3 || bp == 3) { return false; }
    Triangle shared{};
    size_t count = 0, aOther = 0, bOther = 0;
    for (size_t i = 0; i < 3; ++i) {
        bool found = false;
        for (size_t j = 0; j < 3; ++j) {
            if (aIds[i] == bIds[j] && a[i] == b[j]) { shared[count++] = a[i]; found = true; }
        }
        if (!found) { aOther = i; }
    }
    // Noncollapsed triangles have distinct positions, so count is at most 3.
    // Matching coordinates without matching source IDs do not allow contact.
    if (count == 3) { return true; }
    if (count == 2) {
        for (size_t j = 0; j < 3; ++j) {
            bool found = false;
            for (size_t i = 0; i < 3; ++i) { found |= aIds[i] == bIds[j] && a[i] == b[j]; }
            if (!found) { bOther = j; }
        }
        if (orient3(a[0],a[1],a[2],b[bOther])) { return false; }
        const size_t x = (ap+1)%3, y = (ap+2)%3;
        return orient2(shared[0],shared[1],a[aOther],x,y)
            == orient2(shared[0],shared[1],b[bOther],x,y);
    }
    for (size_t i = 0; i < 3; ++i) {
        if (segmentTriangle(a[i],a[(i+1)%3],b,bp,shared,count)
            || segmentTriangle(b[i],b[(i+1)%3],a,ap,shared,count)) { return true; }
    }
    return false;
}
} // namespace woby
