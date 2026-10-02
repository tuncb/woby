#include "uv_quality.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace woby {
namespace {
Coordinate subtract(const Coordinate& a, const Coordinate& b)
{
    return {a[0]-b[0], a[1]-b[1], a[2]-b[2]};
}
double dot(const Coordinate& a, const Coordinate& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
double crossLength(const Coordinate& a, const Coordinate& b)
{
    return std::hypot(a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]);
}
double angle(const Coordinate& a, const Coordinate& b) { return std::atan2(crossLength(a,b), dot(a,b)); }
}

UvQuality analyzeUvQuality(const Mesh& mesh, UvAreaNormalization normalization, UvQualityMetric metric, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    UvQuality result;
    result.normalization = normalization;
    result.metric = metric;
    result.triangles.resize(mesh.indices.size()/3);
    for (const auto& node : mesh.nodes) {
        const size_t begin = node.indexOffset/3, end = begin + node.indexCount/3;
        double worldArea = 0, uvArea = 0;
        bool positive = false, negative = false;
        for (size_t t = begin; t < end; ++t) {
            if (t % 4096 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            auto& q = result.triangles.at(t);
            q.partId = node.sourceObjectId; q.triangle = t-begin+1;
            if (!node.hasTexcoords) { q.missing = true; ++result.missing; continue; }
            std::array<Coordinate,3> p{}, uv{};
            for (size_t k = 0; k < 3; ++k) {
                const auto index = mesh.indices.at(t*3+k);
                p[k] = meshPosition(mesh,index);
                const auto& tex = mesh.vertices.at(index).texcoord;
                uv[k] = {tex[0],tex[1],0};
            }
            const auto a = subtract(p[1],p[0]), b = subtract(p[2],p[0]);
            const auto u = subtract(uv[1],uv[0]), v = subtract(uv[2],uv[0]);
            q.surfaceArea = crossLength(a,b)*.5;
            const double signedUvArea = (u[0]*v[1]-u[1]*v[0])*.5;
            q.uvArea = std::abs(signedUvArea);
            // Relative to edge length squared: invariant under translations and uniform scaling.
            q.degenerateSurface = q.surfaceArea <= 1e-12*std::max({dot(a,a),dot(b,b),dot(subtract(a,b),subtract(a,b))});
            q.collapsed = q.uvArea <= 1e-12*std::max({dot(u,u),dot(v,v),dot(subtract(u,v),subtract(u,v))});
            if (q.degenerateSurface) { ++result.degenerateSurface; }
            if (q.collapsed) { ++result.collapsed; }
            if (q.collapsed || q.degenerateSurface) { continue; }
            q.orientation = signedUvArea > 0 ? 1 : -1;
            positive |= q.orientation > 0; negative |= q.orientation < 0;
            worldArea += q.surfaceArea; uvArea += q.uvArea;
            for (size_t k = 0; k < 3; ++k) {
                const double difference = std::abs(angle(subtract(p[(k+1)%3],p[k]),subtract(p[(k+2)%3],p[k]))
                    - angle(subtract(uv[(k+1)%3],uv[k]),subtract(uv[(k+2)%3],uv[k])));
                q.angleDegrees = std::max(q.angleDegrees, difference*180/std::numbers::pi);
            }
        }
        if (positive && negative) { ++result.mixedOrientationPatches; }
        const double baselineLog2 = normalization == UvAreaNormalization::perPatch && worldArea > 0
            ? std::log2(uvArea)-std::log2(worldArea) : 0;
        for (size_t t = begin; t < end; ++t) {
            if (t % 4096 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            auto& q = result.triangles[t];
            q.mixedOrientation = positive && negative && q.orientation != 0;
            if (q.missing || q.collapsed || q.degenerateSurface) { continue; }
            q.areaLog2 = std::log2(q.uvArea)-std::log2(q.surfaceArea)-baselineLog2;
        }
    }
    return result;
}

std::vector<Vertex> uvQualityVertices(const Mesh& display, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    std::vector<Vertex> result;
    if (!display.uvQuality) { return result; }
    result.reserve(display.indices.size());
    for (const auto& node : display.nodes) {
        for (size_t i = 0; i < node.indexCount; i += 3) {
            if (i % 12288 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            const auto& q = display.uvQuality->triangles.at(node.uvQualityOffset+i/3);
            float value = 0;
            if (display.uvQuality->metric == UvQualityMetric::angle) { value = static_cast<float>(q.angleDegrees/90); }
            else if (display.uvQuality->metric == UvQualityMetric::area) { value = static_cast<float>(std::abs(q.areaLog2)/3); }
            else { value = q.orientation < 0 ? 1.0f : 0.0f; }
            if (q.collapsed) { value = -1; }
            if (q.missing || q.degenerateSurface) { value = -2; }
            for (size_t k = 0; k < 3; ++k) {
                auto vertex = display.vertices.at(display.indices.at(node.indexOffset+i+k));
                vertex.texcoord = {value, q.mixedOrientation ? 1.0f : 0.0f};
                result.push_back(vertex);
            }
        }
    }
    return result;
}
} // namespace woby
