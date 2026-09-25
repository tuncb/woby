#include "mesh_degenerates.h"
#include "parallel_work.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>

namespace woby {
namespace {
void canceled(const std::stop_token& stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
}
using Point = std::array<double, 3>;
double dot(const Point& a, const Point& b)
{
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
}

DegenerateSettings normalizedDegenerateSettings(DegenerateSettings settings)
{
    settings.needleThresholdRatio = std::isfinite(settings.needleThresholdRatio)
        ? std::max(1.0f, settings.needleThresholdRatio) : 1000.0f;
    settings.capMinAngleDegrees = std::isfinite(settings.capMinAngleDegrees)
        ? std::clamp(settings.capMinAngleDegrees, 90.0f, 180.0f) : 177.5f;
    return settings;
}
bool sameDegenerateThresholds(const DegenerateSettings& a, const DegenerateSettings& b)
{
    return a.needleThresholdRatio == b.needleThresholdRatio && a.capMinAngleDegrees == b.capMinAngleDegrees;
}
const char* triangleProvenanceName(SourceProvenance provenance)
{
    return provenance == SourceProvenance::stlCorners ? "STL facets / generated triangles" : sourceProvenanceName(provenance);
}
const char* degenerateStatus(const MeshDegenerates& result)
{
    if (!result.settings.enabled) { return "disabled"; }
    if (result.unavailableSources) { return result.availableSources ? "partial" : "unavailable"; }
    return "complete";
}

TriangleDegeneracy classifyDegenerateTriangle(const std::array<Point, 3>& points, const DegenerateSettings& input)
{
    const auto settings = normalizedDegenerateSettings(input);
    std::array<Point, 3> edges{};
    double scale = 0;
    for (size_t i = 0; i < 3; ++i) {
        for (size_t k = 0; k < 3; ++k) {
            if (!std::isfinite(points[i][k])) { throw std::invalid_argument("Non-finite analysis coordinate."); }
            edges[i][k] = points[(i+1)%3][k] - points[i][k];
            if (!std::isfinite(edges[i][k])) { throw std::invalid_argument("Analysis edge exceeds numeric range."); }
            scale = std::max(scale, std::abs(edges[i][k]));
        }
    }
    TriangleDegeneracy result;
    const auto unavailable = std::numeric_limits<double>::quiet_NaN();
    result.edgeRatio = result.maximumAngleDegrees = unavailable;
    if (scale == 0) { result.collapsed = true; return result; }
    // Normalize before products to preserve very small/large triangle behavior.
    for (auto& edge : edges) { for (auto& value : edge) { value /= scale; } }
    std::array<double, 3> lengths{};
    for (size_t i = 0; i < 3; ++i) { lengths[i] = std::hypot(edges[i][0], edges[i][1], edges[i][2]); }
    const auto [shortest, longest] = std::minmax_element(lengths.begin(), lengths.end());
    const auto& u = edges[0]; const auto& v = edges[1];
    result.collapsed = (u[1]*v[2] - u[2]*v[1] == 0)
        && (u[2]*v[0] - u[0]*v[2] == 0) && (u[0]*v[1] - u[1]*v[0] == 0);
    if (*shortest == 0) { result.collapsed = true; return result; }
    result.edgeRatio = *longest / *shortest;
    const double threshold = settings.needleThresholdRatio;
    // Strict longest/shortest > threshold, expressed with bounded squared values.
    const double relativeShortest = *shortest / *longest;
    result.needle = relativeShortest * relativeShortest < 1.0 / (threshold * threshold);
    double minimumCosine = 1;
    for (size_t i = 0; i < 3; ++i) {
        Point a{}, b{};
        for (size_t k = 0; k < 3; ++k) { a[k] = edges[i][k]/lengths[i]; b[k] = -edges[(i+2)%3][k]/lengths[(i+2)%3]; }
        minimumCosine = std::min(minimumCosine, std::clamp(dot(a, b), -1.0, 1.0));
    }
    result.maximumAngleDegrees = std::acos(minimumCosine) * 180.0 / std::numbers::pi;
    const double capCosine = settings.capMinAngleDegrees == 90 ? 0
        : settings.capMinAngleDegrees == 180 ? -1 : std::cos(settings.capMinAngleDegrees * std::numbers::pi / 180.0);
    result.cap = minimumCosine < capCosine;
    return result;
}

MeshDegenerates inspectDegenerates(const std::vector<DuplicateSource>& sources, DegenerateSettings settings, std::stop_token stop)
{
    MeshDegenerates result;
    result.settings = normalizedDegenerateSettings(settings);
    canceled(stop);
    if (!result.settings.enabled) { return result; }
    for (const auto& source : sources) {
        canceled(stop);
        if (!source.data) { ++result.unavailableSources; continue; }
        ++result.availableSources;
        const auto& data = *source.data;
        if (data.indices.size()%3) { throw std::invalid_argument("Invalid source triangle indices."); }
        for (const auto& point : data.points) {
            canceled(stop);
            for (const auto value : point) { if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite source coordinate."); } }
        }
        for (const auto index : data.indices) {
            canceled(stop);
            if (index >= data.points.size()) { throw std::invalid_argument("Invalid source point index."); }
        }
        std::set<uint64_t> partIds, repeatedPartIds;
        for (const auto& part : source.parts) {
            if (!partIds.insert(part.partId).second) { repeatedPartIds.insert(part.partId); }
        }
        std::set<std::pair<uint64_t, size_t>> visited;
        std::vector<std::array<size_t, 2>> work;
        work.reserve(data.indices.size() / 3);
        for (size_t partIndex = 0; partIndex < source.parts.size(); ++partIndex) {
            const auto& part = source.parts[partIndex];
            canceled(stop);
            if (part.firstIndex%3 || part.indexCount%3 || part.firstIndex > data.indices.size()
                || part.indexCount > data.indices.size()-part.firstIndex) { throw std::invalid_argument("Invalid source part range."); }
            for (const auto value : part.transform) {
                if (!std::isfinite(value)) { throw std::invalid_argument("Non-finite source transform."); }
            }
            for (size_t i = part.firstIndex; i < part.firstIndex+part.indexCount; i += 3) {
                canceled(stop);
                if (repeatedPartIds.contains(part.partId) && !visited.emplace(part.partId, i/3).second) { continue; }
                work.push_back({partIndex, i});
            }
        }
        constexpr size_t batchSize = 1024;
        std::vector<std::vector<DegenerateFinding>> batches((work.size() + batchSize - 1) / batchSize);
        parallelAnalysisBatches(work.size(), batchSize, stop, [&](size_t begin, size_t end) {
            auto& findings = batches[begin / batchSize];
            for (size_t item = begin; item < end; ++item) {
                canceled(stop);
                const auto& part = source.parts[work[item][0]];
                const auto i = work[item][1];
                std::array<Point, 3> points{};
                DegenerateFinding finding;
                for (size_t j = 0; j < 3; ++j) {
                    const auto& p = data.points[data.indices[i+j]];
                    const auto& m = part.transform;
                    for (size_t k = 0; k < 3; ++k) {
                        const double value = m[k]*p[0] + m[k+4]*p[1] + m[k+8]*p[2] + m[k+12];
                        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
                            throw std::invalid_argument("Degenerate inspection requires finite renderable world coordinates.");
                        }
                        points[j][k] = value;
                        finding.geometry[j][k] = static_cast<float>(value);
                    }
                }
                finding.reasons = classifyDegenerateTriangle(points, result.settings);
                const auto& reasons = finding.reasons;
                if (!reasons.collapsed && !reasons.needle && !reasons.cap) { continue; }
                finding.fileId = source.fileId; finding.partId = part.partId; finding.triangleId = i/3;
                finding.source = source.name; finding.provenance = data.provenance;
                findings.push_back(std::move(finding));
            }
        });
        for (auto& batch : batches) {
            for (auto& finding : batch) {
                canceled(stop);
                result.collapsedCount += finding.reasons.collapsed;
                result.needleCount += finding.reasons.needle;
                result.capCount += finding.reasons.cap;
                result.findings.push_back(std::move(finding));
            }
        }
    }
    std::sort(result.findings.begin(), result.findings.end(), [&](const auto& a, const auto& b) {
        canceled(stop);
        return std::tie(a.fileId, a.triangleId, a.partId) < std::tie(b.fileId, b.triangleId, b.partId);
    });
    canceled(stop);
    return result;
}
} // namespace woby
