#include "uv_quality.h"

#include <boost/multiprecision/cpp_int.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

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
}

void inspectUvOverlaps(UvQuality& quality, std::stop_token stop, UvOverlapLimits limits)
{
    quality.overlaps.clear(); quality.overlapCandidates = quality.overlappingTriangles = quality.crossPatchPairs = 0;
    quality.overlapTruncated = quality.overlapChecked = false;
    for (auto& q : quality.triangles) { q.overlapping = q.crossPatchOverlap = false; }
    if (!quality.settings.uvOverlapEnabled) { updateUvQualityStatistics(quality); return; }
    const auto canceled = [&] { if (stop.stop_requested()) { throw std::runtime_error("UV overlap analysis canceled."); } };
    struct Box { size_t triangle; UvPoint min, max; };
    std::vector<Box> boxes;
    for (size_t i = 0; i < quality.triangles.size(); ++i) {
        canceled();
        const auto& q = quality.triangles[i];
        if (!validUvTriangle(q)) { continue; }
        Box box{i,q.uv[0],q.uv[0]};
        for (const auto& p : q.uv) {
            for (size_t k=0;k<2;++k) { box.min[k] = std::min(box.min[k],p[k]); box.max[k] = std::max(box.max[k],p[k]); }
        }
        boxes.push_back(box);
    }
    const bool perPatch = quality.settings.uvOverlapScope == UvOverlapScope::perPatch;
    std::sort(boxes.begin(),boxes.end(),[&](const auto& a, const auto& b) {
        const auto pa = quality.triangles[a.triangle].patch, pb = quality.triangles[b.triangle].patch;
        if (perPatch && pa != pb) { return pa < pb; }
        return a.min[0] == b.min[0] ? a.triangle < b.triangle : a.min[0] < b.min[0];
    });
    for (size_t i=0;i<boxes.size() && !quality.overlapTruncated;++i) {
        canceled();
        auto& a = quality.triangles[boxes[i].triangle];
        for (size_t j=i+1;j<boxes.size();++j) {
            canceled();
            auto& b = quality.triangles[boxes[j].triangle];
            if ((perPatch && a.patch != b.patch) || boxes[j].min[0] >= boxes[i].max[0]) { break; }
            // Bound broad-phase work too: long strips can have few actual hits.
            if (quality.overlapCandidates == limits.candidates) { quality.overlapTruncated = true; break; }
            ++quality.overlapCandidates;
            if (boxes[j].min[1] >= boxes[i].max[1] || boxes[i].min[1] >= boxes[j].max[1]) { continue; }
            if (!interiorsOverlap(a,b)) { continue; }
            if (quality.overlaps.size() == limits.pairs) { quality.overlapTruncated = true; break; }
            const bool cross = a.patch != b.patch;
            quality.overlaps.push_back({boxes[i].triangle,boxes[j].triangle,cross});
            if (cross) { a.crossPatchOverlap = b.crossPatchOverlap = true; ++quality.crossPatchPairs; }
            else { a.overlapping = b.overlapping = true; }
        }
    }
    canceled();
    quality.overlapChecked = true;
    for (const auto& q : quality.triangles) { if (q.overlapping || q.crossPatchOverlap) { ++quality.overlappingTriangles; } }
    updateUvQualityStatistics(quality);
}
} // namespace woby
