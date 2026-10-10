#include "uv_quality.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace woby {
namespace {
int orientation(const UvPoint& a, const UvPoint& b, const UvPoint& c)
{
    const double ax = a[0]-c[0], ay = a[1]-c[1], bx = b[0]-c[0], by = b[1]-c[1];
    const double p = ax*by, q = ay*bx, determinant = p-q;
    if (std::isnormal(determinant) && std::abs(determinant) > (std::abs(p)+std::abs(q))*1e-14) {
        return determinant > 0 ? 1 : -1;
    }
    using Exact = boost::multiprecision::cpp_rational;
    const Exact exact = (Exact(a[0])-Exact(c[0]))*(Exact(b[1])-Exact(c[1]))
        -(Exact(a[1])-Exact(c[1]))*(Exact(b[0])-Exact(c[0]));
    return exact > 0 ? 1 : exact < 0 ? -1 : 0;
}
bool interiorsOverlap(const UvTriangleQuality& a, const UvTriangleQuality& b)
{
    // Strict separating-axis test. Shared edges/vertices have zero intersection
    // area and are excluded; containment and coincident faces are included.
    const auto separated = [](const auto& first, const auto& second) {
        for (size_t k = 0; k < 3; ++k) {
            bool inside = false;
            for (const auto& p : second.uv) {
                inside |= orientation(first.uv[k],first.uv[(k+1)%3],p) == first.orientation;
            }
            if (!inside) { return true; }
        }
        return false;
    };
    return !separated(a,b) && !separated(b,a);
}

struct Box { size_t triangle; UvPoint min, max; };
void canceled(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV overlap analysis canceled."); }
}
void truncate(UvOverlapResult& result, UvOverlapTruncation reason)
{
    result.truncated = true; result.truncation = reason;
}
size_t automaticWorkLimit(size_t count, size_t candidates)
{
    // Bound index queries and updates separately from exact tests. Saturate instead of wrapping.
    constexpr auto maximum = std::numeric_limits<size_t>::max();
    if (count > maximum/64) { return maximum; }
    const auto base = count*64;
    return candidates > (maximum-base)/4 ? maximum : base+candidates*4;
}
constexpr size_t noInterval = std::numeric_limits<size_t>::max();
struct Interval {
    size_t rank = 0, left = noInterval, right = noInterval;
    int height = 1;
    double maximum = 0;
};
enum class SweepStop { work };
struct Sweep {
    const std::vector<Box>& boxes;
    const std::vector<UvTriangleQuality>& triangles;
    UvOverlapResult& result;
    std::stop_token stop;
    size_t workLimit;
    std::vector<Interval> nodes;
};
void charge(Sweep& s)
{
    canceled(s.stop);
    if (s.result.searchWork == s.workLimit) { throw SweepStop::work; }
    ++s.result.searchWork;
}
int height(const Sweep& s, size_t n) { return n == noInterval ? 0 : s.nodes[n].height; }
void refresh(Sweep& s, size_t n)
{
    auto& node = s.nodes[n];
    node.height = 1+std::max(height(s,node.left),height(s,node.right));
    node.maximum = s.boxes[node.rank].max[1];
    for (const size_t child : {node.left,node.right}) {
        if (child != noInterval) { node.maximum = std::max(node.maximum,s.nodes[child].maximum); }
    }
}
size_t rotateLeft(Sweep& s, size_t n)
{
    const size_t top = s.nodes[n].right;
    s.nodes[n].right = s.nodes[top].left; s.nodes[top].left = n;
    refresh(s,n); refresh(s,top); return top;
}
size_t rotateRight(Sweep& s, size_t n)
{
    const size_t top = s.nodes[n].left;
    s.nodes[n].left = s.nodes[top].right; s.nodes[top].right = n;
    refresh(s,n); refresh(s,top); return top;
}
size_t balance(Sweep& s, size_t n)
{
    refresh(s,n);
    const auto left = s.nodes[n].left, right = s.nodes[n].right;
    if (height(s,left)-height(s,right) > 1) {
        if (height(s,s.nodes[left].left) < height(s,s.nodes[left].right)) { s.nodes[n].left = rotateLeft(s,left); }
        return rotateRight(s,n);
    }
    if (height(s,right)-height(s,left) > 1) {
        if (height(s,s.nodes[right].right) < height(s,s.nodes[right].left)) { s.nodes[n].right = rotateRight(s,right); }
        return rotateLeft(s,n);
    }
    return n;
}
bool before(const Sweep& s, size_t a, size_t b)
{
    const double x = s.boxes[s.nodes[a].rank].min[1], y = s.boxes[s.nodes[b].rank].min[1];
    return x == y ? s.nodes[a].rank < s.nodes[b].rank : x < y;
}
size_t insert(Sweep& s, size_t root, size_t n)
{
    charge(s);
    if (root == noInterval) { return n; }
    if (before(s,n,root)) { s.nodes[root].left = insert(s,s.nodes[root].left,n); }
    else { s.nodes[root].right = insert(s,s.nodes[root].right,n); }
    return balance(s,root);
}
size_t detachFirst(Sweep& s, size_t root, size_t& first)
{
    charge(s);
    if (s.nodes[root].left == noInterval) { first = root; return s.nodes[root].right; }
    s.nodes[root].left = detachFirst(s,s.nodes[root].left,first); return balance(s,root);
}
size_t erase(Sweep& s, size_t root, size_t n)
{
    charge(s);
    if (root == n) {
        const auto left = s.nodes[root].left, right = s.nodes[root].right;
        if (left == noInterval) { return right; }
        if (right == noInterval) { return left; }
        size_t next = noInterval;
        const auto remainder = detachFirst(s,right,next);
        // Move links, not interval values: the expiry heap refers to stable node slots.
        s.nodes[next].left = left; s.nodes[next].right = remainder;
        return balance(s,next);
    }
    if (before(s,n,root)) { s.nodes[root].left = erase(s,s.nodes[root].left,n); }
    else { s.nodes[root].right = erase(s,s.nodes[root].right,n); }
    return balance(s,root);
}
bool sweepQuery(Sweep& s, size_t root, size_t rank)
{
    if (root == noInterval) { return true; }
    charge(s);
    const auto& node = s.nodes[root]; const auto& current = s.boxes[rank];
    if (node.maximum <= current.min[1]) { return true; }
    if (!sweepQuery(s,node.left,rank)) { return false; }
    const auto& previous = s.boxes[node.rank];
    if (previous.min[1] >= current.max[1]) { return true; }
    if (previous.max[1] > current.min[1]) {
        if (s.result.candidates == s.result.limits.candidates) { truncate(s.result,UvOverlapTruncation::candidates); return false; }
        ++s.result.candidates;
        const auto& a = s.triangles[previous.triangle]; const auto& b = s.triangles[current.triangle];
        if (interiorsOverlap(a,b)) {
            if (s.result.pairs.size() == s.result.limits.pairs) { truncate(s.result,UvOverlapTruncation::pairs); return false; }
            const bool cross = a.patch != b.patch;
            s.result.pairs.push_back({previous.triangle,current.triangle,cross});
            const uint8_t flag = cross ? uint8_t{2} : uint8_t{1};
            s.result.flags[previous.triangle] |= flag; s.result.flags[current.triangle] |= flag;
            if (cross) { ++s.result.crossPatchPairs; }
        }
    }
    return sweepQuery(s,node.right,rank);
}
}

const char* uvOverlapTruncationKey(UvOverlapTruncation reason)
{
    switch (reason) {
    case UvOverlapTruncation::none: return "none";
    case UvOverlapTruncation::candidates: return "candidates";
    case UvOverlapTruncation::pairs: return "pairs";
    case UvOverlapTruncation::searchWork: return "search_work";
    }
    return "none";
}
void inspectUvOverlaps(UvQuality& quality, std::stop_token stop, UvOverlapLimits limits)
{
    quality.distributions[static_cast<size_t>(UvQualityMetric::overlap)].reset();
    quality.overlap.reset(); quality.overlapLimits = limits;
    if (!quality.settings.uvOverlapEnabled) { updateUvQualityStatistics(quality,stop); return; }
    auto result = std::make_shared<UvOverlapResult>();
    result->measurements = quality.measurements; result->scope = quality.settings.uvOverlapScope; result->limits = limits;
    const auto& triangles = uvMeasurements(quality).triangles;
    result->flags.resize(triangles.size());
    std::vector<Box> boxes;
    for (size_t i=0; i<triangles.size(); ++i) {
        canceled(stop);
        const auto& t = triangles[i]; if (!validUvTriangle(t)) { continue; }
        Box box{i,t.uv[0],t.uv[0]};
        for (const auto& p : t.uv) { for (size_t k=0; k<2; ++k) {
            box.min[k] = std::min(box.min[k],p[k]); box.max[k] = std::max(box.max[k],p[k]);
        } }
        boxes.push_back(box);
    }
    const bool perPatch = quality.settings.uvOverlapScope == UvOverlapScope::perPatch;
    size_t comparisons = 0;
    std::sort(boxes.begin(),boxes.end(),[&](const auto& a, const auto& b) {
        if ((comparisons++ & 1023) == 0) { canceled(stop); }
        const auto pa = triangles[a.triangle].patch, pb = triangles[b.triangle].patch;
        if (perPatch && pa != pb) { return pa < pb; }
        return a.min[0] == b.min[0] ? a.triangle < b.triangle : a.min[0] < b.min[0];
    });
    result->searchWorkLimit = limits.searchWork.value_or(automaticWorkLimit(boxes.size(),limits.candidates));
    Sweep sweep{boxes,triangles,*result,stop,result->searchWorkLimit,{}};
    std::vector<size_t> expired, free;
    const auto expiresLater = [&](size_t a, size_t b) {
        const auto x = boxes[sweep.nodes[a].rank].max[0], y = boxes[sweep.nodes[b].rank].max[0];
        return x == y ? sweep.nodes[a].rank > sweep.nodes[b].rank : x > y;
    };
    size_t root = noInterval;
    try {
        for (size_t i=0; i<boxes.size(); ++i) {
            canceled(stop);
            if (perPatch && i && triangles[boxes[i-1].triangle].patch != triangles[boxes[i].triangle].patch) {
                sweep.nodes.clear(); expired.clear(); free.clear(); root = noInterval;
            }
            while (!expired.empty() && boxes[sweep.nodes[expired.front()].rank].max[0] <= boxes[i].min[0]) {
                const size_t n = expired.front();
                std::pop_heap(expired.begin(),expired.end(),expiresLater); expired.pop_back();
                root = erase(sweep,root,n); free.push_back(n);
            }
            // Query in V order before insertion, so each pair is emitted once.
            if (!sweepQuery(sweep,root,i)) { break; }
            size_t n = sweep.nodes.size();
            if (free.empty()) { sweep.nodes.emplace_back(); } else { n = free.back(); free.pop_back(); }
            sweep.nodes[n] = {i,noInterval,noInterval,1,boxes[i].max[1]};
            root = insert(sweep,root,n);
            expired.push_back(n); std::push_heap(expired.begin(),expired.end(),expiresLater);
        }
    } catch (SweepStop) { truncate(*result,UvOverlapTruncation::searchWork); }
    canceled(stop);
    for (const auto flags : result->flags) { if (flags) { ++result->overlappingTriangles; } }
    quality.overlapCache = quality.overlap = std::move(result);
    updateUvQualityStatistics(quality,stop);
}
} // namespace woby
