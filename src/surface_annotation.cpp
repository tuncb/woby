#include "surface_annotation.h"
#include "hash_utils.h"

#include <bx/math.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace woby {
namespace {
using P2 = std::array<double, 2>;
using P3 = std::array<double, 3>;
using P4 = std::array<double, 4>;
double cross2(P2 a, P2 b) { return a[0] * b[1] - a[1] * b[0]; }
P2 sub(P2 a, P2 b) { return {a[0] - b[0], a[1] - b[1]}; }
P2 screen(const P4& p) { return {p[0] / p[3], p[1] / p[3]}; }
P3 weights(const AnnotationProjectedTriangle& triangle, P2 p)
{
    const auto a = screen(triangle.clip[0]), b = screen(triangle.clip[1]), c = screen(triangle.clip[2]);
    const double area = cross2(sub(b, a), sub(c, a));
    const double v = cross2(sub(p, a), sub(c, a)) / area;
    const double w = cross2(sub(b, a), sub(p, a)) / area;
    return {1 - v - w, v, w};
}
double depth(const AnnotationProjectedTriangle& triangle, P2 p)
{
    const auto w = weights(triangle, p);
    double z = 0;
    for (size_t k = 0; k < 3; ++k) { z += w[k] * triangle.clip[k][2] / triangle.clip[k][3]; }
    return z;
}
std::array<float, 3> anchor(const AnnotationProjectedTriangle& triangle, P2 p)
{
    auto w = weights(triangle, p);
    double sum = 0;
    for (size_t k = 0; k < 3; ++k) { w[k] /= triangle.clip[k][3]; sum += w[k]; }
    std::array<float, 3> result{};
    for (size_t axis = 0; axis < 3; ++axis) {
        double value = 0;
        for (size_t k = 0; k < 3; ++k) { value += w[k] * triangle.bary[k][axis] / sum; }
        result[axis] = static_cast<float>(std::clamp(value, 0.0, 1.0));
    }
    const float total = result[0] + result[1] + result[2];
    for (auto& value : result) { value /= total; }
    return result;
}
struct ClipVertex { P4 clip; P3 bary; };
// Call-local cache: adjacent triangles and groups often share mesh vertices and
// the same transform. No borrowed mesh pointer survives projection creation.
struct ProjectionVertexCache {
    const Mesh* mesh = nullptr;
    PickMatrix transform{};
    bool homogeneous = false;
    std::vector<size_t> vertices;
    std::vector<uint8_t> masks;
};
P4 clipPosition(const AnnotationProjectionVertex& vertex)
{
    return {vertex.clip[0], vertex.clip[1], vertex.clip[2], vertex.clip[3]};
}
AnnotationProjectedTriangle projectedTriangle(const AnnotationProjection& projection, size_t index)
{
    const auto& face = projection.triangles[index];
    if (face.clipped) { return projection.clippedTriangles[*face.clipped]; }
    AnnotationProjectedTriangle result;
    result.objectId = face.objectId; result.triangle = face.triangle;
    for (size_t k = 0; k < 3; ++k) {
        const auto& vertex = projection.vertices[face.vertices[k]];
        result.clip[k] = clipPosition(vertex);
        result.localTriangle[k] = vertex.local;
        result.bary[k][k] = 1;
    }
    return result;
}
double plane(const P4& p, size_t axis, bool homogeneous)
{
    switch (axis) {
    case 0: return p[3] + p[0];
    case 1: return p[3] - p[0];
    case 2: return p[3] + p[1];
    case 3: return p[3] - p[1];
    case 4: return homogeneous ? p[3] + p[2] : p[2];
    default: return p[3] - p[2];
    }
}
void appendProjection(AnnotationProjection& result, const ScenePickPart& part,
    const PickMatrix& transform, bool homogeneous, ProjectionVertexCache& cache)
{
    if (!part.mesh || part.opacity <= 0) { return; }
    const auto& mesh = *part.mesh;
    if (cache.mesh != &mesh || cache.transform != transform || cache.homogeneous != homogeneous) {
        cache.mesh = &mesh; cache.transform = transform; cache.homogeneous = homogeneous;
        cache.vertices.resize(mesh.vertices.size());
        cache.masks.assign(mesh.vertices.size(), 0xff);
    }
    const size_t finish = std::min(mesh.indices.size(), part.indexOffset + part.indexCount);
    for (size_t i = part.indexOffset; i + 2 < finish; i += 3) {
        AnnotationProjectionFace face;
        face.objectId = part.objectId;
        face.triangle = static_cast<uint32_t>((i - part.indexOffset) / 3);
        size_t count = 3;
        uint8_t outside = 0, common = 0x3f;
        for (size_t k = 0; k < 3; ++k) {
            const auto index = mesh.indices[i + k];
            if (index >= mesh.vertices.size()) { count = 0; break; }
            auto& mask = cache.masks[index];
            if (mask == 0xff) {
                const auto& p = mesh.vertices[index].position;
                const auto q = annotationTransform(transform, {p[0], p[1], p[2], 1});
                mask = 0x80;
                if (finitePosition(p) && std::all_of(q.begin(), q.end(), [](float v) { return std::isfinite(v); })) {
                    cache.vertices[index] = result.vertices.size();
                    result.vertices.push_back({q, p});
                    mask = 0;
                    for (size_t axis = 0; axis < 6; ++axis) {
                        if (plane({q[0], q[1], q[2], q[3]}, axis, homogeneous) < 0) { mask |= static_cast<uint8_t>(1u << axis); }
                    }
                }
            }
            if (mask == 0x80) { count = 0; break; }
            face.vertices[k] = cache.vertices[index];
            outside |= mask; common &= mask;
        }
        if (common || !count) { continue; }
        if (!outside) {
            // The overwhelmingly common case needs no polygon clipping, explicit
            // barycentrics, or repeated copies of the three source positions.
            std::array<P2, 3> points;
            for (size_t k = 0; k < 3; ++k) {
                const auto p = clipPosition(result.vertices[face.vertices[k]]);
                if (p[3] <= 0) { count = 0; break; }
                points[k] = screen(p);
            }
            if (!count || std::abs(cross2(sub(points[1], points[0]), sub(points[2], points[0]))) < 1e-15) { continue; }
            for (size_t axis = 0; axis < 2; ++axis) {
                face.minimum[axis] = std::min({points[0][axis], points[1][axis], points[2][axis]});
                face.maximum[axis] = std::max({points[0][axis], points[1][axis], points[2][axis]});
            }
            result.triangles.push_back(face);
            continue;
        }
        std::array<ClipVertex, 12> polygon, clipped;
        for (size_t k = 0; k < 3; ++k) {
            polygon[k] = {clipPosition(result.vertices[face.vertices[k]]), {0, 0, 0}};
            polygon[k].bary[k] = 1;
        }
        for (size_t axis = 0; axis < 6 && count; ++axis) {
            if (!(outside & (1u << axis))) { continue; }
            bool inside = true;
            for (size_t j = 0; j < count; ++j) { inside &= plane(polygon[j].clip, axis, homogeneous) >= 0; }
            if (inside) { continue; }
            size_t clippedCount = 0;
            auto previous = polygon[count - 1];
            double dp = plane(previous.clip, axis, homogeneous);
            for (size_t j = 0; j < count; ++j) {
                const auto& current = polygon[j];
                const double dc = plane(current.clip, axis, homogeneous);
                if ((dp >= 0) != (dc >= 0)) {
                    const double t = dp / (dp - dc);
                    ClipVertex v{};
                    for (size_t k = 0; k < 4; ++k) { v.clip[k] = previous.clip[k] + t * (current.clip[k] - previous.clip[k]); }
                    for (size_t k = 0; k < 3; ++k) { v.bary[k] = previous.bary[k] + t * (current.bary[k] - previous.bary[k]); }
                    clipped[clippedCount++] = v;
                }
                if (dc >= 0) { clipped[clippedCount++] = current; }
                previous = current; dp = dc;
            }
            std::copy_n(clipped.begin(), clippedCount, polygon.begin()); count = clippedCount;
        }
        for (size_t k = 1; k + 1 < count; ++k) {
            AnnotationProjectedTriangle t;
            t.objectId = part.objectId;
            for (size_t j = 0; j < 3; ++j) { t.localTriangle[j] = mesh.vertices[mesh.indices[i + j]].position; }
            t.triangle = static_cast<uint32_t>((i - part.indexOffset) / 3);
            const std::array<ClipVertex, 3> vertices{polygon[0], polygon[k], polygon[k + 1]};
            for (size_t j = 0; j < 3; ++j) { t.clip[j] = vertices[j].clip; t.bary[j] = vertices[j].bary; }
            if (std::any_of(t.clip.begin(), t.clip.end(), [](const auto& p) { return p[3] <= 0; })) { continue; }
            if (std::abs(cross2(sub(screen(t.clip[1]), screen(t.clip[0])),
                    sub(screen(t.clip[2]), screen(t.clip[0])))) < 1e-15) { continue; }
            t.minimum = t.maximum = screen(t.clip[0]);
            for (size_t j = 1; j < 3; ++j) {
                const auto p = screen(t.clip[j]);
                for (size_t axis = 0; axis < 2; ++axis) { t.minimum[axis] = std::min(t.minimum[axis], p[axis]); t.maximum[axis] = std::max(t.maximum[axis], p[axis]); }
            }
            face.minimum = t.minimum; face.maximum = t.maximum;
            face.clipped = result.clippedTriangles.size();
            result.clippedTriangles.push_back(t);
            result.triangles.push_back(face);
        }
    }
}
size_t buildProjectionNode(AnnotationProjection& projection, size_t begin, size_t end)
{
    AnnotationProjectionNode node;
    node.begin = begin; node.end = end;
    node.minimum.fill(std::numeric_limits<double>::infinity());
    node.maximum.fill(-std::numeric_limits<double>::infinity());
    const size_t index = projection.nodes.size(); projection.nodes.push_back(node);
    if (end - begin > 8) {
        const size_t middle = begin + (end - begin) / 2;
        const size_t left = buildProjectionNode(projection, begin, middle), right = buildProjectionNode(projection, middle, end);
        node.left = left; node.right = right;
        for (size_t axis = 0; axis < 2; ++axis) {
            node.minimum[axis] = std::min(projection.nodes[left].minimum[axis], projection.nodes[right].minimum[axis]);
            node.maximum[axis] = std::max(projection.nodes[left].maximum[axis], projection.nodes[right].maximum[axis]);
        }
    } else {
        for (size_t i = begin; i < end; ++i) {
            const auto& triangle = projection.triangles[projection.order[i]];
            for (size_t axis = 0; axis < 2; ++axis) {
                node.minimum[axis] = std::min(node.minimum[axis], triangle.minimum[axis]);
                node.maximum[axis] = std::max(node.maximum[axis], triangle.maximum[axis]);
            }
        }
    }
    projection.nodes[index] = node;
    return index;
}
uint32_t projectionMortonCoordinate(double center)
{
    // Interleave 16-bit screen coordinates so adjacent leaves stay spatially close.
    uint32_t value = static_cast<uint32_t>(std::clamp((center + 1) * .5, 0.0, 1.0) * 65535);
    value = (value | (value << 8)) & 0x00ff00ffu;
    value = (value | (value << 4)) & 0x0f0f0f0fu;
    value = (value | (value << 2)) & 0x33333333u;
    return (value | (value << 1)) & 0x55555555u;
}
void finishProjection(AnnotationProjection& projection)
{
    // Sort compact keys once, then combine bounds bottom-up. Repeated median
    // partitions used to rescan the large triangle records at every tree level.
    std::vector<std::pair<uint32_t, size_t>> keys;
    keys.reserve(projection.triangles.size());
    for (size_t i = 0; i < projection.triangles.size(); ++i) {
        const auto& triangle = projection.triangles[i];
        keys.emplace_back(projectionMortonCoordinate((triangle.minimum[0] + triangle.maximum[0]) * .5)
            | (projectionMortonCoordinate((triangle.minimum[1] + triangle.maximum[1]) * .5) << 1), i);
    }
    // Stable radix sorting preserves source-order ties and avoids comparison
    // sorting millions of Morton keys for every gesture.
    std::vector<std::pair<uint32_t, size_t>> scratch(keys.size());
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        std::array<size_t, 256> offsets{};
        for (const auto& key : keys) { ++offsets[(key.first >> shift) & 255]; }
        size_t begin = 0;
        for (auto& offset : offsets) { const auto count = offset; offset = begin; begin += count; }
        for (const auto& key : keys) { scratch[offsets[(key.first >> shift) & 255]++] = key; }
        keys.swap(scratch);
    }
    projection.order.resize(projection.triangles.size());
    for (size_t i = 0; i < projection.order.size(); ++i) { projection.order[i] = keys[i].second; }
    projection.nodes.clear();
    projection.nodes.reserve(projection.order.size() / 2 + 1);
    if (!projection.order.empty()) { buildProjectionNode(projection, 0, projection.order.size()); }
}
std::vector<size_t> projectionCandidates(const AnnotationProjection& projection, P2 start, P2 end)
{
    std::vector<size_t> result;
    const auto visit = [&](const auto& self, size_t index) -> void {
        const auto& node = projection.nodes[index];
        double low = 0, high = 1;
        for (size_t axis = 0; axis < 2; ++axis) {
            const double delta = end[axis] - start[axis];
            if (std::abs(delta) < 1e-15) {
                if (start[axis] < node.minimum[axis] - 1e-10 || start[axis] > node.maximum[axis] + 1e-10) { return; }
            } else {
                double a = (node.minimum[axis] - 1e-10 - start[axis]) / delta;
                double b = (node.maximum[axis] + 1e-10 - start[axis]) / delta;
                if (a > b) { std::swap(a, b); }
                low = std::max(low, a); high = std::min(high, b);
                if (low > high) { return; }
            }
        }
        if (node.left) { self(self, node.left); self(self, node.right); }
        else { for (size_t i = node.begin; i < node.end; ++i) { result.push_back(projection.order[i]); } }
    };
    if (!projection.nodes.empty()) { visit(visit, 0); }
    std::sort(result.begin(), result.end());
    return result;
}
struct Interval { size_t index; double begin, end, z0, z1; };
struct VisibleInterval { const Interval* surface; double begin, end; };
void appendVisibleInterval(std::vector<VisibleInterval>& result, const Interval* surface, double begin, double end)
{
    if (end <= begin) { return; }
    if (!result.empty() && result.back().surface == surface) { result.back().end = end; }
    else { result.push_back({surface, begin, end}); }
}
std::vector<VisibleInterval> visibleIntervals(std::span<const Interval> intervals)
{
    if (intervals.empty()) { return {{nullptr, 0, 1}}; }
    if (intervals.size() == 1) {
        const auto& interval = intervals.front();
        std::vector<VisibleInterval> result;
        appendVisibleInterval(result, nullptr, 0, interval.begin);
        appendVisibleInterval(result, &interval, interval.begin, interval.end);
        appendVisibleInterval(result, nullptr, interval.end, 1);
        return result;
    }
    // Merge the frontmost depth envelopes, discarding hidden intersections at
    // every level. Enumerating every overlapping pair is quadratic even for
    // coincident faces and used to hit an arbitrary two-million-pair limit.
    const size_t middle = intervals.size() / 2;
    const auto left = visibleIntervals(intervals.first(middle));
    const auto right = visibleIntervals(intervals.subspan(middle));
    std::vector<VisibleInterval> result;
    result.reserve(left.size() + right.size());
    size_t i = 0, j = 0;
    while (i < left.size() && j < right.size()) {
        const auto& a = left[i]; const auto& b = right[j];
        const double low = std::max(a.begin, b.begin), high = std::min(a.end, b.end);
        if (!a.surface || !b.surface) {
            appendVisibleInterval(result, a.surface ? a.surface : b.surface, low, high);
        } else {
            const auto* first = a.surface; const auto* second = b.surface;
            const double slope = (first->z1 - first->z0) - (second->z1 - second->z0);
            const auto appendNearest = [&](double begin, double end) {
                const double mid = (begin + end) * .5;
                const double za = first->z0 + mid * (first->z1 - first->z0);
                const double zb = second->z0 + mid * (second->z1 - second->z0);
                const bool chooseFirst = za < zb || (za == zb && first->index < second->index);
                appendVisibleInterval(result, chooseFirst ? first : second, begin, end);
            };
            const double crossing = std::abs(slope) < 1e-15 ? high : (second->z0 - first->z0) / slope;
            if (crossing > low && crossing < high) {
                appendNearest(low, crossing); appendNearest(crossing, high);
            } else { appendNearest(low, high); }
        }
        if (a.end <= b.end) { ++i; }
        if (b.end <= a.end) { ++j; }
    }
    return result;
}
void projectEdge(AnnotationGeometry& result, const AnnotationProjection& projection, P2 start, P2 end)
{
    for (const auto& point : {start, end}) {
        if (pickAnnotationSurface(projection, {static_cast<float>(point[0]), static_cast<float>(point[1])}) != projection.targetId) {
            throw std::runtime_error("Keep every endpoint or corner on the target surface.");
        }
    }
    const auto at = [&](double t) -> P2 { return {start[0] + t * (end[0] - start[0]), start[1] + t * (end[1] - start[1])}; };
    std::vector<Interval> intervals;
    for (const size_t i : projectionCandidates(projection, start, end)) {
        const auto triangle = projectedTriangle(projection, i);
        const auto a = weights(triangle, start), b = weights(triangle, end);
        double low = 0, high = 1;
        for (size_t k = 0; k < 3; ++k) {
            const double delta = b[k] - a[k];
            if (std::abs(delta) < 1e-15) { if (a[k] < -1e-10) { high = -1; } }
            else if (delta > 0) { low = std::max(low, -a[k] / delta); }
            else { high = std::min(high, -a[k] / delta); }
        }
        if (high - low <= 1e-10) { continue; }
        intervals.push_back({i, low, high, depth(triangle, start), depth(triangle, end)});
    }
    std::optional<AnnotationProjectedTriangle> previousTriangle;
    std::array<float, 3> previousPoint{};
    bool previous = false;
    bool gap = false;
    for (const auto& visible : visibleIntervals(intervals)) {
        const double low = visible.begin, high = visible.end;
        if (high - low < 1e-10) { continue; }
        const auto* best = visible.surface;
        if (!best) { gap = true; continue; }
        if (projection.triangles[best->index].objectId != projection.targetId) {
            throw std::runtime_error("Keep the outline clear of other objects.");
        }
        const auto triangle = projectedTriangle(projection, best->index);
        const AnnotationSegment segment{triangle.triangle, anchor(triangle, at(low)), anchor(triangle, at(high)), std::nullopt};
        const auto localPoint = [&](const auto& bary) {
            std::array<float, 3> p{};
            for (size_t k = 0; k < 3; ++k) { for (size_t axis = 0; axis < 3; ++axis) { p[axis] += bary[k] * triangle.localTriangle[k][axis]; } }
            return p;
        };
        const auto currentPoint = localPoint(segment.a);
        if (gap && previous) {
            const auto& rim = result.segments.back();
            result.segments.push_back({rim.triangle, rim.b, segment.a, segment.triangle});
        } else if (previousTriangle) {
            size_t shared = 0;
            for (const auto& p : triangle.localTriangle) {
                if (std::find(previousTriangle->localTriangle.begin(), previousTriangle->localTriangle.end(), p) != previousTriangle->localTriangle.end()) { ++shared; }
            }
            bool continuous = shared > 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                const float scale = std::max({1e-10f, std::abs(currentPoint[axis]), std::abs(previousPoint[axis])});
                continuous &= std::abs(previousPoint[axis] - currentPoint[axis]) <= scale * 4e-6f + 1e-8f;
            }
            if (!continuous) { throw std::runtime_error("The outline crosses a gap or a different surface layer."); }
        }
        // Merge surface fragments only within one face, never across a bridge.
        if (previous && !gap && !result.segments.empty() && result.segments.back().triangle == segment.triangle) {
            result.segments.back().b = segment.b;
        } else { result.segments.push_back(segment); }
        previousTriangle = triangle; previousPoint = localPoint(segment.b); previous = true;
        gap = false;
    }
}
} // namespace

PickMatrix annotationCompose(const PickMatrix& first, const PickMatrix& second)
{
    PickMatrix result;
    bx::mtxMul(result.data(), first.data(), second.data());
    return result;
}
std::array<float, 4> annotationTransform(const PickMatrix& m, const std::array<float, 4>& p)
{
    std::array<float, 4> result{};
    for (size_t row = 0; row < 4; ++row) {
        double value = 0;
        for (size_t k = 0; k < 4; ++k) { value += static_cast<double>(m[k * 4 + row]) * p[k]; }
        result[row] = static_cast<float>(value);
    }
    return result;
}
std::array<float, 2> annotationNdc(const ScenePickView& view, PickPoint p)
{
    return {2 * p[0] / static_cast<float>(view.width) - 1, 1 - 2 * p[1] / static_cast<float>(view.height)};
}
std::string annotationFingerprint(const Mesh& mesh, size_t offset, size_t count)
{
    uint64_t hash = 1469598103934665603ull;
    hashCombine(hash, count);
    const size_t end = std::min(mesh.indices.size(), offset + count);
    for (size_t i = offset; i < end; ++i) {
        hashCombine(hash, mesh.indices[i]);
        if (mesh.indices[i] < mesh.vertices.size()) { hashArray3(hash, mesh.vertices[mesh.indices[i]].position); }
    }
    return std::to_string(hash);
}
AnnotationProjection annotationProjection(std::span<const ScenePickPart> parts,
    const ScenePickView& view, SceneObjectId target)
{
    AnnotationProjection result;
    result.targetId = target;
    result.definition.homogeneousDepth = view.homogeneousDepth;
    const auto vp = annotationCompose(view.view, view.projection);
    ProjectionVertexCache cache;
    for (const auto& part : parts) {
        // Without a target, visible transparent surfaces are candidates too.
        // Once a target is chosen, only opaque neighbors occlude its outline.
        if (!part.mesh || (!part.solid && part.objectId != target)
            || (target != 0 && part.opacity < .999f && part.objectId != target)) { continue; }
        const auto transform = annotationCompose(part.model, vp);
        if (part.objectId == target) {
            result.definition.projector = transform;
            result.definition.fingerprint = annotationFingerprint(*part.mesh, part.indexOffset, part.indexCount);
        }
        appendProjection(result, part, transform, view.homogeneousDepth, cache);
    }
    finishProjection(result);
    return result;
}
AnnotationProjection annotationEditProjection(const ScenePickPart& target, const AnnotationGeometry& geometry)
{
    AnnotationProjection result;
    result.targetId = target.objectId;
    result.definition = geometry;
    result.definition.segments.clear();
    ProjectionVertexCache cache;
    appendProjection(result, target, geometry.projector, geometry.homogeneousDepth, cache);
    finishProjection(result);
    return result;
}
void setAnnotationProjectionTarget(AnnotationProjection& projection,
    std::span<const ScenePickPart> parts, const ScenePickView& view, SceneObjectId target)
{
    const auto part = std::find_if(parts.begin(), parts.end(), [target](const auto& p) { return p.objectId == target; });
    if (!target || part == parts.end() || !part->mesh || (projection.targetId && projection.targetId != target)) {
        throw std::runtime_error("The annotation target is unavailable.");
    }
    projection.targetId = target;
    projection.definition.projector = annotationCompose(part->model, annotationCompose(view.view, view.projection));
    projection.definition.fingerprint = annotationFingerprint(*part->mesh, part->indexOffset, part->indexCount);
    std::vector<SceneObjectId> transparent;
    for (const auto& p : parts) {
        if (p.objectId != target && p.opacity < .999f) { transparent.push_back(p.objectId); }
    }
    if (transparent.empty()) { return; }
    std::sort(transparent.begin(), transparent.end());
    const auto removed = std::erase_if(projection.triangles, [&](const auto& triangle) {
        return std::binary_search(transparent.begin(), transparent.end(), triangle.objectId);
    });
    if (removed) { finishProjection(projection); }
}
SceneObjectId pickAnnotationSurface(const AnnotationProjection& projection, std::array<float, 2> point)
{
    const auto hit = annotationSurfaceHit(projection, point);
    return hit ? hit->objectId : 0;
}
std::optional<AnnotationSurfaceHit> annotationSurfaceHit(const AnnotationProjection& projection, std::array<float, 2> point)
{
    double best = std::numeric_limits<double>::infinity();
    size_t bestIndex = std::numeric_limits<size_t>::max();
    std::optional<AnnotationSurfaceHit> hit;
    // Point queries dominate sampled previews. Traverse directly instead of
    // allocating and sorting a candidate vector for every sample.
    const auto visit = [&](const auto& self, size_t nodeIndex) -> void {
        const auto& node = projection.nodes[nodeIndex];
        for (size_t axis = 0; axis < 2; ++axis) {
            if (point[axis] < node.minimum[axis] - 1e-10 || point[axis] > node.maximum[axis] + 1e-10) { return; }
        }
        if (node.left) { self(self, node.left); self(self, node.right); return; }
        for (size_t i = node.begin; i < node.end; ++i) {
            const size_t index = projection.order[i];
            const auto& face = projection.triangles[index];
            // Wider than the barycentric tolerance even for viewport-sized faces.
            if (point[0] < face.minimum[0] - 1e-8 || point[0] > face.maximum[0] + 1e-8
                || point[1] < face.minimum[1] - 1e-8 || point[1] > face.maximum[1] + 1e-8) { continue; }
            const auto triangle = projectedTriangle(projection, index);
            const auto w = weights(triangle, {point[0], point[1]});
            if (*std::min_element(w.begin(), w.end()) < -1e-9) { continue; }
            double z = 0;
            for (size_t k = 0; k < 3; ++k) { z += w[k] * triangle.clip[k][2] / triangle.clip[k][3]; }
            if (z < best || (z == best && index < bestIndex)) {
                best = z; bestIndex = index;
                hit = AnnotationSurfaceHit{triangle.objectId, triangle.triangle, anchor(triangle, {point[0], point[1]})};
            }
        }
    };
    if (!projection.nodes.empty()) { visit(visit, 0); }
    return hit;
}
namespace {
AnnotationGeometry annotationDefinition(const AnnotationProjection& projection, AnnotationShape shape,
    std::array<float, 2> start, std::array<float, 2> end)
{
    for (float value : {start[0], start[1], end[0], end[1]}) {
        if (!std::isfinite(value) || std::abs(value) > 1.00001f) { throw std::runtime_error("Draw inside the viewport."); }
    }
    const double dx = end[0] - start[0], dy = end[1] - start[1];
    if (dx * dx + dy * dy < 1e-10 || (shape == AnnotationShape::rectangle && (std::abs(dx) < 1e-5 || std::abs(dy) < 1e-5))) {
        throw std::runtime_error("Drag to give the annotation a nonzero size.");
    }
    AnnotationGeometry result = projection.definition;
    result.shape = shape; result.start = start; result.end = end; result.segments.clear();
    return result;
}
std::vector<P2> outlinePoints(AnnotationShape shape, std::array<float, 2> start, std::array<float, 2> end)
{
    if (shape == AnnotationShape::line) { return {{start[0], start[1]}, {end[0], end[1]}}; }
    return {{start[0], start[1]}, {end[0], start[1]}, {end[0], end[1]}, {start[0], end[1]}, {start[0], start[1]}};
}
} // namespace
AnnotationGeometry projectAnnotation(const AnnotationProjection& projection, AnnotationShape shape,
    std::array<float, 2> start, std::array<float, 2> end)
{
    auto result = annotationDefinition(projection, shape, start, end);
    const auto points = outlinePoints(shape, start, end);
    for (size_t k = 1; k < points.size(); ++k) { projectEdge(result, projection, points[k - 1], points[k]); }
    validateAnnotationGeometry(result);
    return result;
}
AnnotationGeometry previewAnnotation(const AnnotationProjection& projection, AnnotationShape shape,
    std::array<float, 2> start, std::array<float, 2> end)
{
    auto result = annotationDefinition(projection, shape, start, end);
    const auto points = outlinePoints(shape, start, end);
    for (size_t edge = 1; edge < points.size(); ++edge) {
        std::optional<AnnotationSurfaceHit> previous;
        constexpr size_t steps = 32;
        for (size_t i = 0; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            const auto& a = points[edge - 1]; const auto& b = points[edge];
            const auto hit = annotationSurfaceHit(projection,
                {static_cast<float>(a[0] + t * (b[0] - a[0])), static_cast<float>(a[1] + t * (b[1] - a[1]))});
            if ((!hit || hit->objectId != projection.targetId) && (i == 0 || i == steps)) {
                throw std::runtime_error("Keep every endpoint or corner on the target surface.");
            }
            if (!hit) { continue; }
            if (hit->objectId != projection.targetId) { throw std::runtime_error("Keep the outline clear of other objects."); }
            if (previous) {
                result.segments.push_back({previous->triangle, previous->bary, hit->bary, hit->triangle});
            }
            previous = hit;
        }
    }
    validateAnnotationGeometry(result);
    return result;
}
std::array<float, 3> annotationPosition(const Mesh& mesh, size_t offset, uint32_t triangle, const std::array<float, 3>& bary)
{
    std::array<float, 3> result{};
    for (size_t k = 0; k < 3; ++k) {
        const size_t index = offset + static_cast<size_t>(triangle) * 3 + k;
        if (index >= mesh.indices.size() || mesh.indices[index] >= mesh.vertices.size()) { throw std::runtime_error("Annotation triangle is unavailable."); }
        const auto& p = mesh.vertices[mesh.indices[index]].position;
        for (size_t axis = 0; axis < 3; ++axis) { result[axis] += bary[k] * p[axis]; }
    }
    return result;
}
AnnotationSettings normalizedAnnotationSettings(AnnotationSettings settings)
{
    if (settings.name.empty()) { settings.name = "Annotation"; }
    settings.width = std::isfinite(settings.width) ? std::clamp(settings.width, 1.0f, 12.0f) : 3.0f;
    for (auto& value : settings.color) { value = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f; }
    return settings;
}
void validateAnnotationGeometry(const AnnotationGeometry& geometry)
{
    if (geometry.shape != AnnotationShape::line && geometry.shape != AnnotationShape::rectangle) { throw std::runtime_error("Invalid annotation shape."); }
    if (geometry.fingerprint.empty() || geometry.segments.empty() || geometry.segments.size() > 1000000) { throw std::runtime_error("Invalid annotation surface data."); }
    for (auto v : geometry.projector) { if (!std::isfinite(v)) { throw std::runtime_error("Invalid annotation projector."); } }
    PickMatrix inverse;
    bx::mtxInverse(inverse.data(), geometry.projector.data());
    for (auto v : inverse) { if (!std::isfinite(v)) { throw std::runtime_error("Singular annotation projector."); } }
    for (auto point : {geometry.start, geometry.end}) {
        for (auto v : point) { if (!std::isfinite(v) || std::abs(v) > 1.00001f) { throw std::runtime_error("Invalid annotation control point."); } }
    }
    for (const auto& segment : geometry.segments) {
        for (const auto& bary : {segment.a, segment.b}) {
            double sum = 0;
            for (float value : bary) {
                if (!std::isfinite(value) || value < 0 || value > 1) { throw std::runtime_error("Invalid annotation surface anchor."); }
                sum += value;
            }
            if (std::abs(sum - 1) > 1e-5) { throw std::runtime_error("Invalid annotation anchor weights."); }
        }
    }
}
std::vector<std::array<float, 3>> annotationControlPositions(const Mesh& mesh, size_t offset,
    const AnnotationGeometry& geometry)
{
    if (geometry.segments.empty()) { return {}; }
    std::vector<std::array<float, 2>> controls{geometry.start, geometry.end};
    if (geometry.shape == AnnotationShape::rectangle) {
        controls = {geometry.start, {geometry.end[0], geometry.start[1]}, geometry.end,
            {geometry.start[0], geometry.end[1]}};
    }
    std::vector<std::array<float, 3>> result;
    for (const auto& control : controls) {
        float best = std::numeric_limits<float>::max();
        std::optional<std::array<float, 3>> nearest;
        for (const auto& segment : geometry.segments) {
            size_t endpoint = 0;
            for (const auto& bary : {segment.a, segment.b}) {
                const auto triangle = endpoint++ == 0 ? segment.triangle : segment.endTriangle.value_or(segment.triangle);
                const auto local = annotationPosition(mesh, offset, triangle, bary);
                const auto clip = annotationTransform(geometry.projector, {local[0], local[1], local[2], 1});
                if (clip[3] <= 0) { continue; }
                const float x = clip[0] / clip[3] - control[0], y = clip[1] / clip[3] - control[1];
                if (x * x + y * y < best) { best = x * x + y * y; nearest = local; }
            }
        }
        if (!nearest) { return {}; }
        result.push_back(*nearest);
    }
    return result;
}
std::vector<DiagnosticEdge> annotationWorldLines(const UiAnnotation& item, std::span<const ScenePickPart> parts)
{
    std::vector<DiagnosticEdge> lines;
    if (!item.settings.visible || !item.targetValid || item.settings.color[3] <= 0) { return lines; }
    const auto target = std::find_if(parts.begin(), parts.end(), [&](const auto& part) { return part.objectId == item.targetId; });
    if (target == parts.end() || !target->mesh || target->opacity <= 0) { return lines; }
    for (const auto& segment : item.geometry.segments) {
        DiagnosticEdge edge;
        size_t k = 0;
        for (const auto& bary : {segment.a, segment.b}) {
            const auto triangle = k == 0 ? segment.triangle : segment.endTriangle.value_or(segment.triangle);
            const auto local = annotationPosition(*target->mesh, target->indexOffset, triangle, bary);
            const auto world = annotationTransform(target->model, {local[0], local[1], local[2], 1});
            auto& endpoint = k++ == 0 ? edge.a : edge.b;
            endpoint = {world[0] / world[3], world[1] / world[3], world[2] / world[3]};
        }
        lines.push_back(edge);
    }
    return lines;
}
void appendAnnotationPickParts(std::vector<ScenePickPart>& parts, const UiState& state,
    std::vector<std::vector<DiagnosticEdge>>& storage)
{
    storage.clear(); storage.reserve(state.annotations.size());
    for (const auto& item : state.annotations) { storage.push_back(annotationWorldLines(item, parts)); }
    for (size_t i = 0; i < state.annotations.size(); ++i) {
        ScenePickPart part;
        part.objectId = state.annotations[i].objectId; part.edgeXray = false;
        bx::mtxIdentity(part.model.data());
        part.diagnosticEdges = storage[i];
        parts.push_back(part);
    }
}
bool annotationEdgeHit(const UiAnnotation& item, std::span<const ScenePickPart> parts,
    const ScenePickView& view, PickPoint point)
{
    if (item.settings.locked) { return false; }
    const auto lines = annotationWorldLines(item, parts);
    if (lines.empty()) { return false; }
    ScenePickPart outline;
    outline.objectId = item.objectId; outline.edgeXray = false;
    outline.diagnosticEdges = lines;
    bx::mtxIdentity(outline.model.data());
    // Reject off-outline pointers before testing source/occluder triangles.
    if (pickSceneObject(std::span<const ScenePickPart>(&outline, 1), view, point) != item.objectId) { return false; }
    std::vector<ScenePickPart> pickParts(parts.begin(), parts.end());
    for (auto& part : pickParts) { part.edges = false; part.vertices = false; }
    pickParts.push_back(outline);
    return pickSceneObject(pickParts, view, point) == item.objectId;
}
} // namespace woby
