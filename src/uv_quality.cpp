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
    ComparisonSettings settings;
    settings.uvNormalization = normalization;
    settings.uvMetric = metric;
    return analyzeUvQuality(mesh, settings, stop);
}

UvQuality analyzeUvQuality(const Mesh& mesh, const ComparisonSettings& settings, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    UvQuality result;
    result.settings = normalizedComparisonSettings(settings);
    result.normalization = result.settings.uvNormalization;
    result.metric = result.settings.uvMetric;
    auto measurements = std::make_shared<UvMeasurements>();
    auto& raw = *measurements;
    raw.triangles.resize(meshGeometry(mesh).indices.size()/3);
    size_t patch = 0;
    for (const auto& node : meshGeometry(mesh).nodes) {
        const size_t begin = node.indexOffset/3, end = begin + node.indexCount/3;
        double worldArea = 0, uvArea = 0;
        bool positive = false, negative = false;
        for (size_t t = begin; t < end; ++t) {
            if (t % 4096 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            auto& q = raw.triangles.at(t);
            q.partId = node.sourceObjectId; q.triangle = t-begin+1; q.patch = patch;
            if (!node.hasTexcoords) { q.missing = true; ++raw.missing; continue; }
            std::array<Coordinate,3> p{}, uv{};
            for (size_t k = 0; k < 3; ++k) {
                const auto index = meshGeometry(mesh).indices.at(t*3+k);
                p[k] = meshPosition(mesh,index);
                const auto& tex = meshGeometry(mesh).vertices.at(index).texcoord;
                uv[k] = {tex[0],tex[1],0};
                q.uv[k] = {tex[0],tex[1]};
            }
            const auto a = subtract(p[1],p[0]), b = subtract(p[2],p[0]);
            const auto u = subtract(uv[1],uv[0]), v = subtract(uv[2],uv[0]);
            q.surfaceArea = crossLength(a,b)*.5;
            const double signedUvArea = (u[0]*v[1]-u[1]*v[0])*.5;
            q.uvArea = std::abs(signedUvArea);
            // Relative to edge length squared: invariant under translations and uniform scaling.
            q.degenerateSurface = q.surfaceArea <= 1e-12*std::max({dot(a,a),dot(b,b),dot(subtract(a,b),subtract(a,b))});
            q.collapsed = q.uvArea <= 1e-12*std::max({dot(u,u),dot(v,v),dot(subtract(u,v),subtract(u,v))});
            if (q.degenerateSurface) { ++raw.degenerateSurface; }
            if (q.collapsed) { ++raw.collapsed; }
            if (q.collapsed || q.degenerateSurface) { continue; }
            q.orientation = signedUvArea > 0 ? 1 : -1;
            positive |= q.orientation > 0; negative |= q.orientation < 0;
            worldArea += q.surfaceArea; uvArea += q.uvArea;
            // Express the 3D triangle in an orthonormal tangent frame, then
            // differentiate its affine surface -> UV map. Scale before the
            // singular-value calculation; det / sigmaMax avoids cancellation
            // in the small eigenvalue near a rank-one map.
            const double length = std::sqrt(dot(a,a));
            const double x = dot(a,b)/length, y = 2*q.surfaceArea/length;
            const double j00 = u[0]/length, j10 = u[1]/length;
            const double j01 = (v[0]-j00*x)/y, j11 = (v[1]-j10*x)/y;
            const double scale = std::max({std::abs(j00),std::abs(j01),std::abs(j10),std::abs(j11)});
            const double aa = j00/scale, bb = j01/scale, cc = j10/scale, dd = j11/scale;
            const double e = aa*aa+cc*cc, f = aa*bb+cc*dd, g = bb*bb+dd*dd;
            q.maxStretch = scale*std::sqrt((e+g+std::hypot(e-g,2*f))*.5);
            q.minStretch = (q.uvArea/q.surfaceArea)/q.maxStretch;
            q.anisotropy = q.maxStretch/q.minStretch;
            for (size_t k = 0; k < 3; ++k) {
                const double difference = std::abs(angle(subtract(p[(k+1)%3],p[k]),subtract(p[(k+2)%3],p[k]))
                    - angle(subtract(uv[(k+1)%3],uv[k]),subtract(uv[(k+2)%3],uv[k])));
                q.angleDegrees = std::max(q.angleDegrees, difference*180/std::numbers::pi);
            }
        }
        if (positive && negative) { ++raw.mixedOrientationPatches; }
        raw.patchAreaLog2.push_back(worldArea > 0 ? std::log2(uvArea)-std::log2(worldArea) : 0);
        raw.patchStretchScale.push_back(std::exp2(raw.patchAreaLog2.back()*.5));
        for (size_t t = begin; t < end; ++t) {
            if (t % 4096 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            auto& q = raw.triangles[t];
            q.mixedOrientation = positive && negative && q.orientation != 0;
            if (q.missing || q.collapsed || q.degenerateSurface) { continue; }
            q.areaLog2 = std::log2(q.uvArea)-std::log2(q.surfaceArea);
        }
        ++patch;
    }
    result.measurements = std::move(measurements);
    updateUvQualityStatistics(result, stop);
    return result;
}

const UvMeasurements& uvMeasurements(const UvQuality& quality)
{
    static const UvMeasurements empty;
    return quality.measurements ? *quality.measurements : empty;
}
const UvOverlapResult& uvOverlapResult(const UvQuality& quality)
{
    static const UvOverlapResult empty;
    return quality.overlap ? *quality.overlap : empty;
}
UvTriangleQuality uvTriangleQuality(const UvQuality& quality, size_t index)
{
    const auto& measurements = uvMeasurements(quality);
    auto result = measurements.triangles.at(index);
    if (validUvTriangle(result) && quality.normalization == UvAreaNormalization::perPatch) {
        result.areaLog2 -= measurements.patchAreaLog2.at(result.patch);
        const auto scale = measurements.patchStretchScale.at(result.patch);
        result.minStretch /= scale; result.maxStretch /= scale;
    }
    if (quality.overlap) {
        const auto flags = quality.overlap->flags.at(index);
        result.overlapping = (flags & 1) != 0; result.crossPatchOverlap = (flags & 2) != 0;
    }
    return result;
}
void updateUvQualitySettings(UvQuality& quality, const ComparisonSettings& settings, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    const auto next = normalizedComparisonSettings(settings);
    const bool scopeChanged = quality.settings.uvOverlapScope != next.uvOverlapScope;
    const auto previous = quality.overlap;
    quality.settings = next;
    quality.normalization = next.uvNormalization;
    quality.metric = next.uvMetric;
    if (!next.uvOverlapEnabled) { quality.overlap.reset(); }
    else if (!scopeChanged && quality.overlapCache && quality.overlapCache->measurements == quality.measurements
        && quality.overlapCache->scope == next.uvOverlapScope && quality.overlapCache->limits == quality.overlapLimits) {
        quality.overlap = quality.overlapCache;
    } else { inspectUvOverlaps(quality, stop); return; }
    if (previous != quality.overlap) { quality.distributions[static_cast<size_t>(UvQualityMetric::overlap)].reset(); }
    updateUvQualityStatistics(quality, stop);
}

const char* uvQualityMetricKey(UvQualityMetric metric)
{
    switch (metric) {
    case UvQualityMetric::area: return "area";
    case UvQualityMetric::orientation: return "orientation";
    case UvQualityMetric::anisotropy: return "anisotropy";
    case UvQualityMetric::minStretch: return "min_stretch";
    case UvQualityMetric::overlap: return "overlap";
    case UvQualityMetric::angle: default: return "angle";
    }
}
UvQualityMetric parseUvQualityMetric(const std::string& key)
{
    for (auto metric : {UvQualityMetric::angle, UvQualityMetric::area, UvQualityMetric::orientation,
        UvQualityMetric::anisotropy, UvQualityMetric::minStretch, UvQualityMetric::overlap}) {
        if (key == uvQualityMetricKey(metric)) { return metric; }
    }
    throw std::invalid_argument("Unknown UV quality metric: " + key);
}
const char* uvQualityMetricName(UvQualityMetric metric)
{
    switch (metric) {
    case UvQualityMetric::area: return "Signed area stretch (log2 UV / surface)";
    case UvQualityMetric::orientation: return "UV orientation";
    case UvQualityMetric::anisotropy: return "Stretch anisotropy";
    case UvQualityMetric::minStretch: return "Minimum local stretch";
    case UvQualityMetric::overlap: return "UV overlaps";
    case UvQualityMetric::angle: default: return "Angle distortion (degrees)";
    }
}
const char* uvQualityLegend(UvQualityMetric metric)
{
    switch (metric) {
    case UvQualityMetric::area: return "Area UV / surface: blue 1/8 or less; neutral 1; red 8 or more. Negative log2 = compression; positive = expansion.";
    case UvQualityMetric::orientation: return "UV winding: blue positive; red negative. Uniformly mirrored patches are valid.";
    case UvQualityMetric::anisotropy: return "Anisotropy: blue 1; yellow 8; red 64 or more. Logarithmic colors; statistics show the ratio.";
    case UvQualityMetric::minStretch: return "Minimum surface-to-UV stretch: blue 1 or more; yellow 0.1; red 0.01 or less. Smaller values approach UV collapse.";
    case UvQualityMetric::overlap: return "Blue: no detected overlap; red: within-patch overlap; yellow: cross-patch overlap only (may be intentional).";
    case UvQualityMetric::angle: default: return "Angle distortion: blue 0; yellow 45; red 90+ degrees. Maximum triangle corner-angle difference.";
    }
}
bool validUvTriangle(const UvTriangleQuality& q) { return !q.missing && !q.collapsed && !q.degenerateSurface; }
double uvQualityValue(const UvTriangleQuality& q, UvQualityMetric metric)
{
    switch (metric) {
    case UvQualityMetric::area: return q.areaLog2;
    case UvQualityMetric::orientation: return q.orientation;
    case UvQualityMetric::anisotropy: return q.anisotropy;
    case UvQualityMetric::minStretch: return q.minStretch;
    case UvQualityMetric::overlap: return q.overlapping ? 1 : q.crossPatchOverlap ? .5 : 0;
    case UvQualityMetric::angle: default: return q.angleDegrees;
    }
}
namespace {
double measurementValue(const UvQuality& quality, const UvTriangleQuality& raw, size_t index, UvQualityMetric metric)
{
    if (metric == UvQualityMetric::overlap) {
        const auto flags = quality.overlap ? quality.overlap->flags.at(index) : 0;
        return (flags & 1) ? 1 : (flags & 2) ? .5 : 0;
    }
    const auto value = uvQualityValue(raw, metric);
    if (quality.normalization != UvAreaNormalization::perPatch || !validUvTriangle(raw)) { return value; }
    if (metric == UvQualityMetric::area) { return value - uvMeasurements(quality).patchAreaLog2.at(raw.patch); }
    if (metric == UvQualityMetric::minStretch) { return value / uvMeasurements(quality).patchStretchScale.at(raw.patch); }
    return value;
}
bool exceedsThreshold(double value, const ComparisonSettings& settings)
{
    return settings.uvMetric == UvQualityMetric::minStretch ? value < settings.uvThreshold
        : (settings.uvMetric == UvQualityMetric::area ? std::abs(value) : value) > settings.uvThreshold;
}
bool highlighted(double value, const ComparisonSettings& settings)
{
    return settings.uvRangeEnabled ? value >= settings.uvRangeMinimum && value <= settings.uvRangeMaximum
        : settings.uvThresholdEnabled && exceedsThreshold(value, settings);
}
std::array<float,2> displayValue(const UvQuality& quality, size_t index)
{
    const auto& raw = uvMeasurements(quality).triangles.at(index);
    const auto metric = measurementValue(quality, raw, index, quality.metric);
    float value = 0;
    if (quality.metric == UvQualityMetric::angle) { value = static_cast<float>(metric/90); }
    else if (quality.metric == UvQualityMetric::area) { value = static_cast<float>(std::clamp(.5+metric/6,0.0,1.0)); }
    else if (quality.metric == UvQualityMetric::anisotropy) { value = static_cast<float>(std::log2(metric)/6); }
    else if (quality.metric == UvQualityMetric::minStretch) { value = static_cast<float>(std::clamp(-std::log10(std::max(metric,1e-30))/2,0.0,1.0)); }
    else if (quality.metric == UvQualityMetric::overlap) { value = static_cast<float>(metric); }
    else { value = raw.orientation < 0 ? 1.0f : 0.0f; }
    if (raw.collapsed) { value = -1; }
    if (raw.missing || raw.degenerateSurface) { value = -2; }
    const auto selection = quality.settings.uvMetric == quality.metric ? metric
        : measurementValue(quality, raw, index, quality.settings.uvMetric);
    return {value, validUvTriangle(raw) && highlighted(selection, quality.settings) ? 1.0f : 0.0f};
}
}
bool uvQualityExceedsThreshold(const UvTriangleQuality& q, const ComparisonSettings& settings)
{
    return validUvTriangle(q) && exceedsThreshold(uvQualityValue(q,settings.uvMetric), settings);
}
bool uvQualityHighlighted(const UvTriangleQuality& q, const ComparisonSettings& settings)
{
    return validUvTriangle(q) && highlighted(uvQualityValue(q,settings.uvMetric), settings);
}
void updateUvQualityStatistics(UvQuality& quality, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    auto& s = quality.statistics;
    const bool normalizedMetric = quality.metric == UvQualityMetric::area || quality.metric == UvQualityMetric::minStretch;
    const size_t variant = normalizedMetric && quality.normalization == UvAreaNormalization::absolute ? 6 : 0;
    auto& distribution = quality.distributions.at(variant + static_cast<size_t>(quality.metric));
    s = distribution.value_or(UvQualityStatistics{});
    s.thresholdCount = s.nearCollapseCount = s.highlightedCount = 0;
    s.thresholdAreaPercent = s.highlightedAreaPercent = 0;
    quality.findings.clear();
    std::vector<double> values;
    if (!distribution) { values.reserve(uvMeasurements(quality).triangles.size()); }
    double thresholdArea = 0, highlightedArea = 0;
    const auto& triangles = uvMeasurements(quality).triangles;
    for (size_t i = 0; i < triangles.size(); ++i) {
        if (i % 4096 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
        const auto& q = triangles[i];
        if (!validUvTriangle(q)) { quality.findings.push_back(i); continue; }
        const auto value = measurementValue(quality, q, i, quality.metric);
        const auto selection = quality.settings.uvMetric == quality.metric ? value
            : measurementValue(quality, q, i, quality.settings.uvMetric);
        const bool threshold = exceedsThreshold(selection, quality.settings);
        const bool selected = highlighted(selection, quality.settings);
        const bool nearCollapse = measurementValue(quality, q, i, UvQualityMetric::minStretch) < quality.settings.uvNearCollapse;
        const bool overlapping = quality.overlap && quality.overlap->flags.at(i) != 0;
        if (q.mixedOrientation || overlapping || nearCollapse || selected) { quality.findings.push_back(i); }
        if (!distribution) { values.push_back(value); s.surfaceArea += q.surfaceArea; }
        if (nearCollapse) { ++s.nearCollapseCount; }
        if (threshold) { ++s.thresholdCount; thresholdArea += q.surfaceArea; }
        if (selected) { ++s.highlightedCount; highlightedArea += q.surfaceArea; }
    }
    s.thresholdAreaPercent = s.surfaceArea > 0 ? thresholdArea/s.surfaceArea*100 : 0;
    s.highlightedAreaPercent = s.surfaceArea > 0 ? highlightedArea/s.surfaceArea*100 : 0;
    if (distribution) { return; }
    s.count = values.size();
    if (values.empty()) { return; }
    std::sort(values.begin(),values.end());
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    const auto percentile = [&](double p) {
        const double index = p*static_cast<double>(values.size()-1);
        const auto low = static_cast<size_t>(index);
        return std::lerp(values[low],values[std::min(low+1,values.size()-1)],index-static_cast<double>(low));
    };
    s.minimum = values.front(); s.maximum = values.back(); s.median = percentile(.5); s.percentile95 = percentile(.95);
    s.histogramMinimum = s.minimum; s.histogramMaximum = s.maximum;
    if (s.minimum == s.maximum) { s.histogramMaximum = s.minimum + std::max(1.0,std::abs(s.minimum)*.01); }
    for (size_t i = 0; i < triangles.size(); ++i) {
        const auto& q = triangles[i];
        if (!validUvTriangle(q)) { continue; }
        const double fraction = (measurementValue(quality,q,i,quality.metric)-s.histogramMinimum)/(s.histogramMaximum-s.histogramMinimum);
        const auto bin = std::min(uvHistogramBins-1,static_cast<size_t>(std::clamp(fraction,0.0,1.0)*uvHistogramBins));
        ++s.counts[bin]; s.areas[bin] += q.surfaceArea;
    }
    distribution = s;
}

std::array<float,4> uvQualityColor(double value, bool area, bool highlighted)
{
    if (value < -1.5) { return {.56f,.61f,.67f,1}; }
    if (value < 0) { return {1,0,1,1}; }
    if (highlighted) { return {.1f,1,.8f,1}; }
    const std::array<float,3> blue{.18f,.48f,.85f}, red{.94f,.22f,.055f};
    const std::array<float,3> middle = area ? std::array<float,3>{.92f,.92f,.92f} : std::array<float,3>{1,.82f,.3f};
    const float t = static_cast<float>(std::clamp(value,0.0,1.0));
    std::array<float,4> result{0,0,0,1};
    for (size_t k=0;k<3;++k) { result[k] = t <= .5f ? std::lerp(blue[k],middle[k],t*2) : std::lerp(middle[k],red[k],(t-.5f)*2); }
    return result;
}

const char* uvFindingLabel(const UvTriangleQuality& triangle, const ComparisonSettings& settings)
{
    if (triangle.missing) { return "Missing UVs"; }
    if (triangle.collapsed) { return "Collapsed UV"; }
    if (triangle.degenerateSurface) { return "Degenerate surface"; }
    if (triangle.mixedOrientation) { return "Mixed orientation"; }
    if (triangle.overlapping) { return "Within-patch overlap"; }
    if (triangle.crossPatchOverlap) { return "Cross-patch overlap"; }
    if (triangle.minStretch < settings.uvNearCollapse) { return "Near collapse"; }
    return "Threshold / selected range";
}

std::optional<std::array<std::array<float, 3>, 3>> uvFindingGeometry(const Mesh& display, size_t finding)
{
    if (!display.uvQuality || finding >= display.uvQuality->findings.size()) { return {}; }
    const auto triangle = uvTriangleQuality(*display.uvQuality, display.uvQuality->findings[finding]);
    for (const auto& node : meshGeometry(display).nodes) {
        if (node.sourceObjectId != triangle.partId || !triangle.triangle || triangle.triangle > node.indexCount / 3) { continue; }
        std::array<std::array<float, 3>, 3> result;
        for (size_t k = 0; k < 3; ++k) {
            result[k] = meshGeometry(display).vertices.at(meshGeometry(display).indices.at(node.indexOffset + (triangle.triangle - 1) * 3 + k)).position;
        }
        return result;
    }
    return {};
}

std::vector<Vertex> uvQualityVertices(const Mesh& display, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    return display.uvQuality ? uvQualityVertices(display, *display.uvQuality, stop) : std::vector<Vertex>{};
}

std::vector<Vertex> uvQualityVertices(const Mesh& display, const UvQuality& quality, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    std::vector<Vertex> result;
    result.reserve(meshGeometry(display).indices.size());
    for (const auto& node : meshGeometry(display).nodes) {
        for (size_t i = 0; i < node.indexCount; i += 3) {
            if (i % 12288 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            const auto q = uvTriangleQuality(quality, node.uvQualityOffset+i/3);
            float value = 0;
            if (quality.metric == UvQualityMetric::angle) { value = static_cast<float>(q.angleDegrees/90); }
            else if (quality.metric == UvQualityMetric::area) { value = static_cast<float>(std::clamp(.5+q.areaLog2/6,0.0,1.0)); }
            else if (quality.metric == UvQualityMetric::anisotropy) { value = static_cast<float>(std::log2(q.anisotropy)/6); }
            else if (quality.metric == UvQualityMetric::minStretch) { value = static_cast<float>(std::clamp(-std::log10(std::max(q.minStretch,1e-30))/2,0.0,1.0)); }
            else if (quality.metric == UvQualityMetric::overlap) { value = static_cast<float>(uvQualityValue(q,UvQualityMetric::overlap)); }
            else { value = q.orientation < 0 ? 1.0f : 0.0f; }
            if (q.collapsed) { value = -1; }
            if (q.missing || q.degenerateSurface) { value = -2; }
            for (size_t k = 0; k < 3; ++k) {
                auto vertex = meshGeometry(display).vertices.at(meshGeometry(display).indices.at(node.indexOffset+i+k));
                vertex.texcoord = {value, uvQualityHighlighted(q,quality.settings) ? 1.0f : 0.0f};
                result.push_back(vertex);
            }
        }
    }
    return result;
}
std::vector<UvQualityFace> uvQualityFaces(const Mesh& display, const UvQuality& quality, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
    std::vector<UvQualityFace> result;
    const auto& geometry = meshGeometry(display);
    if (geometry.indices.size()%3 != 0) { throw std::runtime_error("Invalid UV display triangle count."); }
    result.resize(geometry.indices.size()/3);
    for (const auto& node : geometry.nodes) {
        if (node.indexOffset%3 != 0 || node.indexCount%3 != 0
            || size_t(node.indexOffset)+node.indexCount > geometry.indices.size()) {
            throw std::runtime_error("Invalid UV display node range.");
        }
        for (size_t i = 0; i < node.indexCount; i += 3) {
            if (i % 12288 == 0 && stop.stop_requested()) { throw std::runtime_error("UV analysis canceled."); }
            UvQualityFace face;
            face.value = displayValue(quality, node.uvQualityOffset+i/3);
            for (size_t k = 0; k < 3; ++k) {
                face.normals[k] = meshGeometry(display).vertices.at(meshGeometry(display).indices.at(node.indexOffset+i+k)).normal;
            }
            result[(node.indexOffset+i)/3] = face;
        }
    }
    return result;
}
} // namespace woby
