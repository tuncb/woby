#include "control_scene.h"
#include "comparison_scene.h"
#include "ui_operations.h"
#include "uv_quality.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace woby {
namespace {
using Json = nlohmann::json;
Json triangleInfo(const UvTriangleQuality& t, const UvQuality& q, const ObjectIdFormatter& formatId)
{
    const bool valid = validUvTriangle(t);
    return {{"sourcePartId", formatId(t.partId)}, {"triangle", t.triangle}, {"valid", valid},
        {"missingUv", t.missing}, {"collapsedUv", t.collapsed}, {"degenerateSurface", t.degenerateSurface},
        {"mixedOrientation", t.mixedOrientation}, {"orientation", t.missing ? Json(nullptr) : Json(t.orientation)},
        {"surfaceArea", t.surfaceArea}, {"uvArea", t.missing ? Json(nullptr) : Json(t.uvArea)},
        {"uvVertices", t.missing ? Json(nullptr) : Json(t.uv)},
        {"angleDegrees", valid ? Json(t.angleDegrees) : Json(nullptr)},
        {"areaLog2", valid ? Json(t.areaLog2) : Json(nullptr)},
        {"minStretch", valid ? Json(t.minStretch) : Json(nullptr)},
        {"maxStretch", valid ? Json(t.maxStretch) : Json(nullptr)},
        {"anisotropy", valid ? Json(t.anisotropy) : Json(nullptr)},
        {"value", valid ? Json(uvQualityValue(t, q.metric)) : Json(nullptr)},
        {"thresholdExceeded", uvQualityExceedsThreshold(t, q.settings)},
        {"highlighted", uvQualityHighlighted(t, q.settings)},
        {"nearCollapse", valid && t.minStretch < q.settings.uvNearCollapse},
        {"overlapping", q.overlapChecked ? Json(t.overlapping) : Json(nullptr)},
        {"crossPatchOverlap", q.overlapChecked ? Json(t.crossPatchOverlap) : Json(nullptr)}};
}
Coordinate interpolate(const std::array<Coordinate, 3>& points, const std::array<double, 3>& weights)
{
    Coordinate result{};
    for (size_t k = 0; k < 3; ++k) {
        for (size_t j = 0; j < 3; ++j) { result[j] += points[k][j] * weights[k]; }
    }
    return result;
}
}

nlohmann::json controlUvTrianglePage(const UvQuality& quality, size_t offset, size_t limit, const ObjectIdFormatter& formatId)
{
    if (limit == 0 || limit > 100) { throw std::invalid_argument("limit must be from 1 to 100."); }
    const size_t begin = std::min(offset, quality.triangles.size());
    const size_t end = begin + std::min(limit, quality.triangles.size() - begin);
    auto items = Json::array();
    for (size_t i = begin; i < end; ++i) { items.push_back(triangleInfo(quality.triangles[i], quality, formatId)); }
    return {{"offset", offset}, {"limit", limit}, {"total", quality.triangles.size()},
        {"nextOffset", end < quality.triangles.size() ? Json(end) : Json(nullptr)}, {"items", std::move(items)},
        {"metric", uvQualityMetricKey(quality.metric)},
        {"normalization", quality.normalization == UvAreaNormalization::perPatch ? "per_patch" : "absolute"},
        {"convention", "surface_to_uv"}, {"overlapChecked", quality.overlapChecked}, {"overlapTruncated", quality.overlapTruncated}};
}

nlohmann::json controlUvProbe(UiState& state, const MeshComparison* result, uint64_t resultSignature,
    const ControlOperation& command, const ObjectIdFormatter& formatId)
{
    auto* comparison = findComparison(state, command.objectId);
    if (!comparison || comparison->settings.type != AnalysisType::uvQuality) {
        throw std::invalid_argument("UV probe requires a UV quality analysis ID.");
    }
    Json answer = {{"target", command.target}, {"probe", nullptr}, {"stale", false}};
    if (command.action == ControlAction::comparisonUvProbeClear) {
        clearUvProbe(state, command.objectId);
        return answer;
    }
    const bool setting = command.action == ControlAction::comparisonUvProbe;
    if (!setting && command.action != ControlAction::comparisonUvProbeGet) {
        throw std::invalid_argument("Expected a UV probe command.");
    }
    if (!setting && !comparison->uvProbe) { return answer; }
    const auto signature = comparisonGeometrySignature(state, command.objectId);
    const bool ready = result && result->original.source.uvQuality && resultSignature == signature;
    if (!ready || (!setting && (!comparison->settings.enabled || !comparison->settings.uvLinkedSelection
        || comparison->uvProbe->signature != signature))) {
        if (setting) { throw std::invalid_argument("UV results are not current; call analysis results first."); }
        answer["stale"] = true;
        return answer;
    }
    UvProbe probe;
    if (setting) {
        if (!command.index || !command.object) { throw std::invalid_argument("UV probe requires --object and --index."); }
        probe = {signature, command.memberId, static_cast<size_t>(*command.index),
            command.barycentric.value_or(std::array<double, 3>{1.0/3, 1.0/3, 1.0/3})};
    } else { probe = *comparison->uvProbe; }
    const auto& quality = *result->original.source.uvQuality;
    const auto triangle = std::find_if(quality.triangles.begin(), quality.triangles.end(), [&](const auto& t) {
        return t.partId == probe.partId && t.triangle == probe.triangle;
    });
    const auto surface = comparisonSourceTriangle(state, command.objectId, probe.partId, probe.triangle);
    if (triangle == quality.triangles.end() || !surface) {
        throw std::invalid_argument("UV probe requires an enabled source part and a valid one-based triangle index.");
    }
    auto info = triangleInfo(*triangle, quality, formatId);
    info["metric"] = uvQualityMetricKey(quality.metric);
    info["normalization"] = quality.normalization == UvAreaNormalization::perPatch ? "per_patch" : "absolute";
    info["convention"] = "surface_to_uv";
    info["barycentric"] = probe.barycentric;
    info["surfacePosition"] = interpolate(*surface, probe.barycentric);
    info["coordinateOrigin"] = state.coordinateOrigin.value_or(Coordinate{});
    info["uv"] = nullptr;
    if (!triangle->missing) {
        UvPoint uv{};
        for (size_t k = 0; k < 3; ++k) {
            for (size_t j = 0; j < 2; ++j) { uv[j] += triangle->uv[k][j] * probe.barycentric[k]; }
        }
        info["uv"] = uv;
    }
    info["displayPosition"] = nullptr;
    const auto& mesh = result->original.source;
    for (const auto& node : mesh.nodes) {
        if (node.sourceObjectId != probe.partId || probe.triangle > node.indexCount / 3) { continue; }
        std::array<Coordinate, 3> points;
        for (size_t k = 0; k < 3; ++k) {
            points[k] = meshPosition(mesh, mesh.indices.at(node.indexOffset + (probe.triangle - 1) * 3 + k));
        }
        auto display = interpolate(points, probe.barycentric);
        for (size_t k = 0; k < 3; ++k) { display[k] += comparison->translation[k]; }
        info["displayPosition"] = display;
        break;
    }
    if (setting && !setUvProbe(state, command.objectId, probe.partId, probe.triangle, probe.barycentric)) {
        throw std::invalid_argument("UV probe requires enabled linked selection, an enabled source part and valid barycentric coordinates.");
    }
    answer["probe"] = std::move(info);
    return answer;
}
} // namespace woby
