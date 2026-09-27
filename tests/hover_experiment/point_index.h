#pragma once
// Experimental, CPU-only spatial index. No renderer, UI, or timing dependencies.
#include "model_mesh.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <stdexcept>
#include <vector>

namespace hover_experiment {
using Matrix = std::array<float, 16>;
using Vec4 = std::array<float, 4>;
constexpr uint32_t invalid = UINT32_MAX;
constexpr uint32_t leafSize = 256;
struct Range { uint32_t begin = 0, count = 0; };
struct Box { std::array<float, 3> low{}, high{}; };
struct Node { Box box; uint32_t begin = 0, count = 0, left = invalid, right = invalid; };
struct Cluster { Box box; uint32_t begin = 0, count = 0, group = 0; };
struct Index {
    std::vector<uint32_t> order;
    std::vector<Node> nodes;
    std::vector<uint32_t> roots;
    std::vector<Cluster> clusters;
};
struct Group { Matrix model{}, mvp{}; float radius = 3; bool active = false; };
struct Query { Matrix view{}, projection{}; float x = 0, y = 0; uint32_t width = 1, height = 1; bool homogeneous = false; };
struct Hit { uint32_t rank = invalid, group = invalid; float depth = 0, distance = 0; };
struct Counters { uint64_t nodes = 0, leaves = 0, points = 0; };

inline Vec4 transform(const Matrix& m, const Vec4& p) {
    return {m[0]*p[0]+m[4]*p[1]+m[8]*p[2]+m[12]*p[3],
        m[1]*p[0]+m[5]*p[1]+m[9]*p[2]+m[13]*p[3],
        m[2]*p[0]+m[6]*p[1]+m[10]*p[2]+m[14]*p[3],
        m[3]*p[0]+m[7]*p[1]+m[11]*p[2]+m[15]*p[3]};
}
inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (size_t c = 0; c < 4; ++c) {
        auto v = transform(a, {b[c*4],b[c*4+1],b[c*4+2],b[c*4+3]});
        std::copy(v.begin(), v.end(), r.begin()+static_cast<ptrdiff_t>(c*4));
    }
    return r;
}
inline bool better(const Hit& a, const Hit& b) {
    return a.rank != invalid && (b.rank == invalid || a.depth < b.depth ||
        (a.depth == b.depth && (a.distance < b.distance ||
        (a.distance == b.distance && a.rank < b.rank))));
}
inline Hit pointHit(const std::array<float,3>& p, uint32_t rank, uint32_t groupId,
    const Group& group, const Query& q) {
    if (!group.active) { return {}; }
    const auto world = transform(group.model, {p[0],p[1],p[2],1});
    const auto eye = transform(q.view, world);
    const auto clip = transform(q.projection, eye);
    if (clip[3] <= 0.000001f) { return {}; }
    const float x = clip[0]/clip[3], y = clip[1]/clip[3], z = clip[2]/clip[3];
    if (!std::isfinite(x+y+z) || x < -1 || x > 1 || y < -1 || y > 1 ||
        z < (q.homogeneous ? -1.0f : 0.0f) || z > 1) { return {}; }
    const float dx = (x*.5f+.5f)*static_cast<float>(q.width)-q.x;
    const float dy = (.5f-y*.5f)*static_cast<float>(q.height)-q.y;
    const float d = dx*dx+dy*dy;
    return d <= group.radius*group.radius ? Hit{rank, groupId, z, d} : Hit{};
}
inline std::array<Vec4,5> queryPlanes(const Group& g, const Query& q) {
    const float loX = 2*(q.x-g.radius)/static_cast<float>(q.width)-1;
    const float hiX = 2*(q.x+g.radius)/static_cast<float>(q.width)-1;
    const float loY = 1-2*(q.y+g.radius)/static_cast<float>(q.height);
    const float hiY = 1-2*(q.y-g.radius)/static_cast<float>(q.height);
    std::array<Vec4,5> planes{};
    for (size_t c = 0; c < 4; ++c) {
        const float x = g.mvp[c*4], y = g.mvp[c*4+1], w = g.mvp[c*4+3];
        planes[0][c]=x-loX*w; planes[1][c]=hiX*w-x;
        planes[2][c]=y-loY*w; planes[3][c]=hiY*w-y; planes[4][c]=w;
    }
    return planes;
}
inline bool intersects(const Box& b, const std::array<Vec4,5>& planes) {
    for (const auto& p : planes) {
        float value = p[3], magnitude = std::abs(p[3])+1;
        for (size_t k = 0; k < 3; ++k) {
            value += p[k]*(p[k] >= 0 ? b.high[k] : b.low[k]);
            magnitude += std::abs(p[k])*(std::max(std::abs(b.low[k]),std::abs(b.high[k]))+1);
        }
        // Conservative margin for combined-matrix versus separate-transform rounding.
        if (value < -0.00001f*magnitude) { return false; }
    }
    return true;
}
inline uint32_t buildNode(Index& index, std::span<const woby::Vertex> vertices,
    std::span<const uint32_t> pointIds, uint32_t begin, uint32_t count, uint32_t group) {
    Node n; n.begin=begin; n.count=count;
    n.box.low.fill(std::numeric_limits<float>::max());
    n.box.high.fill(std::numeric_limits<float>::lowest());
    for (uint32_t i=begin; i<begin+count; ++i) {
        const auto& p=vertices[pointIds[index.order[i]]].position;
        for (size_t k=0; k<3; ++k) { n.box.low[k]=std::min(n.box.low[k],p[k]); n.box.high[k]=std::max(n.box.high[k],p[k]); }
    }
    const auto at=static_cast<uint32_t>(index.nodes.size()); index.nodes.push_back(n);
    if (count<=leafSize) { index.clusters.push_back({n.box,begin,count,group}); return at; }
    size_t axis=0;
    for (size_t k=1; k<3; ++k) { if (n.box.high[k]-n.box.low[k]>n.box.high[axis]-n.box.low[axis]) { axis=k; } }
    const uint32_t middle=begin+count/2;
    std::nth_element(index.order.begin()+begin,index.order.begin()+middle,index.order.begin()+begin+count,
        [&](uint32_t a,uint32_t b) { const float x=vertices[pointIds[a]].position[axis],y=vertices[pointIds[b]].position[axis]; return x==y ? a<b : x<y; });
    const uint32_t left=buildNode(index,vertices,pointIds,begin,count/2,group);
    const uint32_t right=buildNode(index,vertices,pointIds,middle,count-count/2,group);
    index.nodes[at].left=left; index.nodes[at].right=right;
    return at;
}
inline Index buildIndex(std::span<const woby::Vertex> vertices, std::span<const uint32_t> pointIds,
    std::span<const Range> ranges) {
    if (pointIds.size()>UINT32_MAX) { throw std::runtime_error("Too many point IDs"); }
    for (const auto id:pointIds) {
        if (id>=vertices.size()) { throw std::runtime_error("Invalid point ID"); }
        for (float x:vertices[id].position) { if (!std::isfinite(x)) { throw std::runtime_error("Nonfinite point"); } }
    }
    Index index; index.order.resize(pointIds.size()); std::iota(index.order.begin(),index.order.end(),0u);
    index.nodes.reserve(pointIds.size()/64+ranges.size()*2);
    index.clusters.reserve(pointIds.size()/128+ranges.size());
    for (size_t g=0; g<ranges.size(); ++g) {
        const auto r=ranges[g];
        if (size_t(r.begin)+r.count>pointIds.size()) { throw std::runtime_error("Invalid point range"); }
        index.roots.push_back(r.count ? buildNode(index,vertices,pointIds,r.begin,r.count,static_cast<uint32_t>(g)) : invalid);
    }
    return index;
}
inline Hit queryIndex(const Index& index, std::span<const woby::Vertex> vertices,
    std::span<const uint32_t> ids, std::span<const Group> groups, const Query& q, Counters& count) {
    Hit best;
    std::array<uint32_t,64> stack{};
    for (size_t g=0; g<index.roots.size(); ++g) {
        if (!groups[g].active || index.roots[g]==invalid) { continue; }
        const auto planes=queryPlanes(groups[g],q);
        size_t size=0; stack[size++]=index.roots[g];
        while (size) {
            const auto& n=index.nodes[stack[--size]]; ++count.nodes;
            if (!intersects(n.box,planes)) { continue; }
            if (n.left!=invalid) { stack[size++]=n.left; stack[size++]=n.right; continue; }
            ++count.leaves; count.points+=n.count;
            for (uint32_t i=n.begin; i<n.begin+n.count; ++i) {
                const auto rank=index.order[i];
                const auto h=pointHit(vertices[ids[rank]].position,rank,static_cast<uint32_t>(g),groups[g],q);
                if (better(h,best)) { best=h; }
            }
        }
    }
    return best;
}
inline Hit queryLinear(std::span<const woby::Vertex> vertices,std::span<const uint32_t> ids,
    std::span<const Range> ranges,std::span<const Group> groups,const Query& q) {
    Hit best;
    for (size_t g=0; g<ranges.size(); ++g) {
        if (!groups[g].active) { continue; }
        for (uint32_t i=ranges[g].begin; i<ranges[g].begin+ranges[g].count; ++i) {
            const auto h=pointHit(vertices[ids[i]].position,i,static_cast<uint32_t>(g),groups[g],q);
            if (better(h,best)) { best=h; }
        }
    }
    return best;
}
} // namespace hover_experiment
