#include "prototype.h"
#include <chrono>
#include <numeric>

// Keep the old scan and predicate as an independent benchmark reference.
#define inspectUvOverlaps inspectUvOverlapsReference
#include "scan_reference.inc"
#undef inspectUvOverlaps

namespace woby {
namespace {
struct Box { size_t triangle; UvPoint min, max; };
struct Node { UvPoint min, max; size_t begin, end, right, maxRank; };
bool intersects(const Box& a, const auto& b)
{
    return a.min[0] < b.max[0] && b.min[0] < a.max[0]
        && a.min[1] < b.max[1] && b.min[1] < a.max[1];
}
void canceled(std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV overlap analysis canceled."); }
}
size_t build(std::vector<Node>& nodes, std::vector<size_t>& order, const std::vector<Box>& boxes,
    size_t begin, size_t end, std::stop_token stop)
{
    canceled(stop);
    Node node{boxes[order[begin]].min,boxes[order[begin]].max,begin,end,0,0};
    UvPoint lo{std::numeric_limits<double>::max(),std::numeric_limits<double>::max()};
    UvPoint hi{std::numeric_limits<double>::lowest(),std::numeric_limits<double>::lowest()};
    for (size_t i=begin; i<end; ++i) {
        if ((i & 1023) == 0) { canceled(stop); }
        const auto& b = boxes[order[i]];
        node.maxRank = std::max(node.maxRank,order[i]);
        for (size_t k=0; k<2; ++k) {
            node.min[k] = std::min(node.min[k],b.min[k]); node.max[k] = std::max(node.max[k],b.max[k]);
            const auto center = std::midpoint(b.min[k],b.max[k]);
            lo[k] = std::min(lo[k],center); hi[k] = std::max(hi[k],center);
        }
    }
    const size_t index = nodes.size(); nodes.push_back(node);
    if (end-begin > 8) {
        const size_t axis = hi[1]-lo[1] > hi[0]-lo[0] ? 1 : 0;
        const size_t mid = begin+(end-begin)/2;
        std::nth_element(order.begin()+static_cast<ptrdiff_t>(begin),order.begin()+static_cast<ptrdiff_t>(mid),
            order.begin()+static_cast<ptrdiff_t>(end),[&](size_t a, size_t b) {
                const auto x = std::midpoint(boxes[a].min[axis],boxes[a].max[axis]);
                const auto y = std::midpoint(boxes[b].min[axis],boxes[b].max[axis]);
                return x == y ? a < b : x < y;
            });
        build(nodes,order,boxes,begin,mid,stop);
        nodes[index].right = build(nodes,order,boxes,mid,end,stop);
    }
    return index;
}
bool query(size_t nodeIndex, size_t rank, const std::vector<Node>& nodes, const std::vector<size_t>& order,
    const std::vector<Box>& boxes, std::vector<size_t>& hits, UvBroadPhaseProbe& probe,
    size_t workLimit, std::stop_token stop)
{
    canceled(stop);
    if (probe.nodeTests+probe.leafTests == workLimit) { probe.workTruncated = true; return false; }
    ++probe.nodeTests;
    const auto& node = nodes[nodeIndex];
    if (node.maxRank <= rank || !intersects(boxes[rank],node)) { return true; }
    if (node.right) {
        return query(nodeIndex+1,rank,nodes,order,boxes,hits,probe,workLimit,stop)
            && query(node.right,rank,nodes,order,boxes,hits,probe,workLimit,stop);
    }
    for (size_t j=node.begin; j<node.end; ++j) {
        if (probe.nodeTests+probe.leafTests == workLimit) { probe.workTruncated = true; return false; }
        ++probe.leafTests;
        if (order[j] > rank && intersects(boxes[rank],boxes[order[j]])) { hits.push_back(order[j]); }
    }
    return true;
}
}
void inspectUvOverlapsTree(UvQuality& quality, UvBroadPhaseProbe& probe, UvOverlapLimits limits,
    std::stop_token stop, size_t workLimit, bool lazyPatches)
{
    probe = {};
    quality.distributions[static_cast<size_t>(UvQualityMetric::overlap)].reset();
    quality.overlap.reset(); quality.overlapLimits = limits;
    if (!quality.settings.uvOverlapEnabled) { updateUvQualityStatistics(quality,stop); return; }
    const auto start = std::chrono::steady_clock::now();
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
    std::sort(boxes.begin(),boxes.end(),[&](const auto& a, const auto& b) {
        const auto pa = triangles[a.triangle].patch, pb = triangles[b.triangle].patch;
        if (perPatch && pa != pb) { return pa < pb; }
        return a.min[0] == b.min[0] ? a.triangle < b.triangle : a.min[0] < b.min[0];
    });
    std::vector<size_t> order(boxes.size()); std::iota(order.begin(),order.end(),size_t{});
    std::vector<Node> nodes;
    std::vector<std::pair<size_t,size_t>> roots;
    for (size_t begin=0; begin<boxes.size();) {
        size_t end = begin+1;
        while (end<boxes.size() && (!perPatch || triangles[boxes[begin].triangle].patch == triangles[boxes[end].triangle].patch)) { ++end; }
        roots.emplace_back(end,lazyPatches ? 0 : build(nodes,order,boxes,begin,end,stop)); begin = end;
    }
    const auto built = std::chrono::steady_clock::now();
    probe.buildMs = std::chrono::duration<double,std::milli>(built-start).count();
    std::vector<size_t> hits;
    size_t group = 0;
    double lazyBuildMs = 0;
    for (size_t i=0; i<boxes.size() && !result->truncated; ++i) {
        if (i == roots[group].first) { ++group; }
        if (lazyPatches && i == (group == 0 ? 0 : roots[group-1].first)) {
            const auto buildStart = std::chrono::steady_clock::now();
            nodes.clear();
            roots[group].second = build(nodes,order,boxes,i,roots[group].first,stop);
            lazyBuildMs += std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-buildStart).count();
        }
        hits.clear();
        if (!query(roots[group].second,i,nodes,order,boxes,hits,probe,workLimit,stop)) {
            result->truncated = true; break;
        }
        // Preserve the production pair order, even though tree traversal is spatial.
        std::sort(hits.begin(),hits.end());
        for (const size_t j : hits) {
            canceled(stop);
            if (result->candidates == limits.candidates) { result->truncated = true; break; }
            ++result->candidates;
            const auto& a = triangles[boxes[i].triangle]; const auto& b = triangles[boxes[j].triangle];
            if (!interiorsOverlap(a,b)) { continue; }
            if (result->pairs.size() == limits.pairs) { result->truncated = true; break; }
            const bool cross = a.patch != b.patch;
            result->pairs.push_back({boxes[i].triangle,boxes[j].triangle,cross});
            const uint8_t flag = cross ? uint8_t{2} : uint8_t{1};
            result->flags[boxes[i].triangle] |= flag; result->flags[boxes[j].triangle] |= flag;
            if (cross) { ++result->crossPatchPairs; }
        }
    }
    canceled(stop);
    probe.buildMs += lazyBuildMs;
    probe.queryMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-built).count()-lazyBuildMs;
    probe.scratchBytes = boxes.capacity()*sizeof(Box)+order.capacity()*sizeof(size_t)
        +nodes.capacity()*sizeof(Node)+hits.capacity()*sizeof(size_t)+roots.capacity()*sizeof(roots[0]);
    for (const auto flags : result->flags) { if (flags) { ++result->overlappingTriangles; } }
    quality.overlapCache = quality.overlap = std::move(result);
    updateUvQualityStatistics(quality,stop);
}

namespace {
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
    UvBroadPhaseProbe& probe;
    std::stop_token stop;
    size_t workLimit;
    std::vector<Interval> nodes;
};
void charge(Sweep& s, bool queryNode)
{
    canceled(s.stop);
    if (s.probe.nodeTests+s.probe.maintenanceTests >= s.workLimit) { throw SweepStop::work; }
    if (queryNode) { ++s.probe.nodeTests; } else { ++s.probe.maintenanceTests; }
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
    charge(s,false);
    if (root == noInterval) { return n; }
    if (before(s,n,root)) { s.nodes[root].left = insert(s,s.nodes[root].left,n); }
    else { s.nodes[root].right = insert(s,s.nodes[root].right,n); }
    return balance(s,root);
}
size_t detachFirst(Sweep& s, size_t root, size_t& first)
{
    charge(s,false);
    if (s.nodes[root].left == noInterval) { first = root; return s.nodes[root].right; }
    s.nodes[root].left = detachFirst(s,s.nodes[root].left,first); return balance(s,root);
}
size_t erase(Sweep& s, size_t root, size_t n)
{
    charge(s,false);
    if (root == n) {
        const auto left = s.nodes[root].left, right = s.nodes[root].right;
        if (left == noInterval) { return right; }
        if (right == noInterval) { return left; }
        size_t next = noInterval;
        const auto remainder = detachFirst(s,right,next);
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
    charge(s,true);
    const auto& node = s.nodes[root]; const auto& current = s.boxes[rank];
    if (node.maximum <= current.min[1]) { return true; }
    if (!sweepQuery(s,node.left,rank)) { return false; }
    const auto& previous = s.boxes[node.rank];
    if (previous.min[1] >= current.max[1]) { return true; }
    if (previous.max[1] > current.min[1]) {
        if (s.result.candidates == s.result.limits.candidates) { s.result.truncated = true; return false; }
        ++s.result.candidates;
        const auto& a = s.triangles[previous.triangle]; const auto& b = s.triangles[current.triangle];
        if (interiorsOverlap(a,b)) {
            if (s.result.pairs.size() == s.result.limits.pairs) { s.result.truncated = true; return false; }
            const bool cross = a.patch != b.patch;
            s.result.pairs.push_back({previous.triangle,current.triangle,cross});
            const uint8_t flag = cross ? uint8_t{2} : uint8_t{1};
            s.result.flags[previous.triangle] |= flag; s.result.flags[current.triangle] |= flag;
            if (cross) { ++s.result.crossPatchPairs; }
        }
    }
    return sweepQuery(s,node.right,rank);
}
size_t verify(const Sweep& s, size_t root, size_t low = noInterval, size_t high = noInterval)
{
    if (root == noInterval) { return 0; }
    const auto& n = s.nodes[root];
    if ((low != noInterval && !before(s,low,root)) || (high != noInterval && !before(s,root,high))) {
        throw std::runtime_error("Sweep interval order is invalid");
    }
    const size_t count = 1+verify(s,n.left,low,root)+verify(s,n.right,root,high);
    double maximum = s.boxes[n.rank].max[1];
    for (const size_t child : {n.left,n.right}) {
        if (child != noInterval) { maximum = std::max(maximum,s.nodes[child].maximum); }
    }
    if (std::abs(height(s,n.left)-height(s,n.right)) > 1
        || n.height != 1+std::max(height(s,n.left),height(s,n.right)) || n.maximum != maximum) {
        throw std::runtime_error("Sweep interval augmentation is invalid");
    }
    return count;
}
}

void inspectUvOverlapsSweep(UvQuality& quality, UvBroadPhaseProbe& probe, UvOverlapLimits limits,
    std::stop_token stop, size_t workLimit, bool verifyIndex)
{
    probe = {};
    quality.distributions[static_cast<size_t>(UvQualityMetric::overlap)].reset();
    quality.overlap.reset(); quality.overlapLimits = limits;
    if (!quality.settings.uvOverlapEnabled) { updateUvQualityStatistics(quality,stop); return; }
    const auto start = std::chrono::steady_clock::now();
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
    std::sort(boxes.begin(),boxes.end(),[&](const auto& a, const auto& b) {
        const auto pa = triangles[a.triangle].patch, pb = triangles[b.triangle].patch;
        if (perPatch && pa != pb) { return pa < pb; }
        return a.min[0] == b.min[0] ? a.triangle < b.triangle : a.min[0] < b.min[0];
    });
    const auto built = std::chrono::steady_clock::now();
    probe.buildMs = std::chrono::duration<double,std::milli>(built-start).count();
    Sweep sweep{boxes,triangles,*result,probe,stop,workLimit,{}};
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
                if (verifyIndex && verify(sweep,root) != expired.size()) { throw std::runtime_error("Sweep erase count differs"); }
            }
            if (!sweepQuery(sweep,root,i)) { break; }
            size_t n = sweep.nodes.size();
            if (free.empty()) { sweep.nodes.emplace_back(); } else { n = free.back(); free.pop_back(); }
            sweep.nodes[n] = {i,noInterval,noInterval,1,boxes[i].max[1]};
            root = insert(sweep,root,n);
            expired.push_back(n); std::push_heap(expired.begin(),expired.end(),expiresLater);
            probe.peakActive = std::max(probe.peakActive,expired.size());
            if (verifyIndex && verify(sweep,root) != expired.size()) { throw std::runtime_error("Sweep insert count differs"); }
        }
    } catch (SweepStop) { probe.workTruncated = result->truncated = true; }
    canceled(stop);
    probe.queryMs = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-built).count();
    probe.scratchBytes = boxes.capacity()*sizeof(Box)+sweep.nodes.capacity()*sizeof(Interval)
        +(expired.capacity()+free.capacity())*sizeof(size_t);
    for (const auto flags : result->flags) { if (flags) { ++result->overlappingTriangles; } }
    quality.overlapCache = quality.overlap = std::move(result);
    updateUvQualityStatistics(quality,stop);
}
}
