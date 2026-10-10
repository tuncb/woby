#include "comparison_scene.h"
#include "obj_mesh.h"
#include "uv_analysis.h"
#include "uv_quality.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace woby;
using Json = nlohmann::json;
void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
template<class T> size_t bytes(const std::vector<T>& values) { return values.capacity()*sizeof(T); }
template<class Work> auto timed(Json& sample, const char* key, Work work)
{
    const auto start = std::chrono::steady_clock::now();
    auto result = work();
    sample[key] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    return result;
}
ComparisonInputSnapshot snapshot(Mesh mesh)
{
    ComparisonInputSnapshot result;
    result.settings.type = AnalysisType::uvQuality;
    result.settings.uvView = UvView::surface;
    auto file = createUiFileState({}, std::move(mesh), 0);
    file.objectId = 1;
    for (size_t i=0; i<file.groupSettings.size(); ++i) {
        file.groupSettings[i].objectId = i+2;
        ComparisonInputPart part;
        part.groupIndex = i;
        coordinateIdentity(part.parent.data());
        result.parts[0].push_back(part);
    }
    result.files.push_back(std::move(file));
    return result;
}
Json payload(const Mesh& mesh)
{
    const auto& g = meshGeometry(mesh);
    return {{"vertices",g.vertices.size()}, {"faces",g.indices.size()/3},
        {"vertex_capacity_bytes",bytes(g.vertices)}, {"position_capacity_bytes",bytes(g.precisePositions)},
        {"index_capacity_bytes",bytes(g.indices)}, {"node_count",g.nodes.size()}};
}
Json payload(const UvQuality& q)
{
    return {{"triangles",woby::uvMeasurements(q).triangles.size()}, {"valid",q.statistics.count}, {"missing",woby::uvMeasurements(q).missing},
        {"collapsed",woby::uvMeasurements(q).collapsed}, {"degenerate_surface",woby::uvMeasurements(q).degenerateSurface},
        {"triangle_capacity_bytes",bytes(woby::uvMeasurements(q).triangles)}, {"finding_capacity_bytes",bytes(q.findings)},
        {"patch_capacity_bytes",bytes(woby::uvMeasurements(q).patchAreaLog2)}, {"overlap_capacity_bytes",bytes(woby::uvOverlapResult(q).pairs)}};
}
void verifyLayout(const Mesh& a, const Mesh& b)
{
    const auto& x = meshGeometry(a); const auto& y = meshGeometry(b);
    require(x.indices == y.indices && x.vertices.size() == y.vertices.size(), "Layout topology differs");
    require(x.precisePositions == y.precisePositions && x.nodes.size() == y.nodes.size(), "Layout coordinates differ");
    for (size_t i=0; i<x.vertices.size(); ++i) {
        require(x.vertices[i].position == y.vertices[i].position && x.vertices[i].texcoord == y.vertices[i].texcoord,
            "Layout vertex differs");
    }
    for (size_t i=0; i<x.nodes.size(); ++i) {
        require(x.nodes[i].uvQualityOffset == y.nodes[i].uvQualityOffset
            && x.nodes[i].sourceObjectId == y.nodes[i].sourceObjectId, "Layout provenance differs");
    }
}
void selfTest()
{
    auto input = snapshot(loadObjMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 4 0\nvt 0 4\n"
        "g first\nf 1/1 2/2 3/3\ng second\nf 1/1 2/2 3/3\ng collapsed\nf 1/1 2/1 3/1\ng missing\nf 1 2 3\n"));
    const auto surface = prepareUvComparisonInputs(input);
    const auto q = surface.buffers[0].uvQuality;
    require(q && woby::uvMeasurements(*q).triangles.size() == 4 && woby::uvMeasurements(*q).missing == 1 && woby::uvMeasurements(*q).collapsed == 1, "Invalid fixture");
    for (bool separated : {false,true}) {
        input.settings.uvView = UvView::layout; input.settings.uvSeparated = separated;
        const auto cold = prepareUvComparisonInputs(input);
        const auto reused = uvLayoutMesh((*surface.meshes)[0], separated);
        verifyLayout((*cold.meshes)[0], reused);
        require(meshGeometry(reused).indices.size() == 9, "Missing UV face was included in layout");
    }
    const std::array<std::shared_ptr<const UvQuality>,2> qualities{q,nullptr};
    auto settings = q->settings; settings.uvThresholdEnabled = true; settings.uvThreshold = 0;
    const auto refreshed = refreshUvComparisonInputs(surface.meshes, qualities, settings);
    require(refreshed.meshes == surface.meshes && refreshed.buffers[0].qualityOnly, "Refresh replaced geometry");
    require(!q->settings.uvThresholdEnabled && refreshed.buffers[0].uvQuality != q, "Refresh changed old result");
    auto overlaps = *q;
    settings.uvOverlapEnabled = true; settings.uvOverlapScope = UvOverlapScope::selectedPatches;
    updateUvQualitySettings(overlaps, settings);
    require(woby::uvOverlapResult(overlaps).crossPatchPairs == 1 && bool(overlaps.overlap), "Overlap fixture failed");
    settings.uvOverlapEnabled = false;
    updateUvQualitySettings(overlaps, settings);
    require(!bool(overlaps.overlap) && woby::uvOverlapResult(overlaps).pairs.empty(), "Disabled overlap retained findings");
    std::stop_source stop; stop.request_stop(); bool canceled = false;
    try { (void)refreshUvComparisonInputs(surface.meshes, qualities, settings, stop.get_token()); }
    catch (const std::runtime_error&) { canceled = true; }
    require(canceled, "Canceled refresh completed");
}
Json measure(const ComparisonInputSnapshot& input)
{
    Json sample;
    const auto surface = timed(sample,"prepare_surface_ms",[&]{ return prepareUvComparisonInputs(input); });
    const auto q = surface.buffers[0].uvQuality;
    require(q && q->statistics.count, "Model has no valid UV measurements");
    sample["surface"] = payload((*surface.meshes)[0]); sample["quality"] = payload(*q);
    sample["display_face_capacity_bytes"] = bytes(surface.buffers[0].quality);
    const std::array<std::shared_ptr<const UvQuality>,2> qualities{q,nullptr};
    {
        const auto measured = timed(sample,"analyze_existing_surface_ms",
            [&]{ return analyzeUvQuality((*surface.meshes)[0],input.settings); });
        require(measured.statistics.count == q->statistics.count && measured.findings == q->findings,
            "Measurements on prepared surface changed");
    }
    {
        auto copy = timed(sample,"copy_quality_ms",[&]{ return *q; });
        auto settings = input.settings; settings.uvThresholdEnabled = true; settings.uvThreshold = 5;
        timed(sample,"threshold_update_owned_ms",[&]{ updateUvQualitySettings(copy,settings); return copy.findings.size(); });
        const auto faces = timed(sample,"threshold_pack_faces_ms",[&]{ return uvQualityFaces((*surface.meshes)[0],copy); });
        sample["threshold_findings"] = copy.findings.size();
        require(faces.size() == surface.buffers[0].quality.size(), "Face count changed");
    }
    auto refresh = [&](const char* key, const ComparisonSettings& settings,
        const std::array<std::shared_ptr<const UvQuality>,2>& from) {
        auto result = timed(sample,key,[&]{ return refreshUvComparisonInputs(surface.meshes,from,settings); });
        require(result.meshes == surface.meshes, "Refresh rebuilt mesh");
        return result;
    };
    auto settings = input.settings; settings.uvThresholdEnabled = true; settings.uvThreshold = 5;
    { const auto result = refresh("threshold_refresh_ms",settings,qualities);
      require(result.buffers[0].updateQuality, "Threshold did not refresh colors"); }
    settings = input.settings; settings.uvNearCollapse = .02f;
    { const auto result = refresh("near_collapse_refresh_ms",settings,qualities);
      require(!result.buffers[0].updateQuality && result.buffers[0].quality.empty(), "Near-collapse uploaded colors"); }
    settings = input.settings; settings.uvMetric = UvQualityMetric::area;
    {
        const auto area = refresh("area_metric_cold_refresh_ms",settings,qualities);
        const std::array<std::shared_ptr<const UvQuality>,2> fromArea{area.buffers[0].uvQuality,nullptr};
        settings.uvThresholdEnabled = true; settings.uvThreshold = .5f;
        { const auto result = refresh("area_metric_cached_threshold_ms",settings,fromArea); }
        settings.uvThresholdEnabled = false; settings.uvNormalization = UvAreaNormalization::absolute;
        const auto normalized = refresh("area_normalization_refresh_ms",settings,fromArea);
        const std::array<std::shared_ptr<const UvQuality>,2> fromNormalized{normalized.buffers[0].uvQuality,nullptr};
        settings.uvNormalization = UvAreaNormalization::perPatch;
        { const auto result = refresh("area_normalization_cached_return_ms",settings,fromNormalized); }
    }
    {
        auto copy = *q; settings = input.settings; settings.uvOverlapEnabled = true;
        timed(sample,"enable_overlap_owned_ms",[&]{ updateUvQualitySettings(copy,settings); return woby::uvOverlapResult(copy).pairs.size(); });
        sample["overlap"] = {{"pairs",woby::uvOverlapResult(copy).pairs.size()}, {"candidates",woby::uvOverlapResult(copy).candidates},
            {"truncated",woby::uvOverlapResult(copy).truncated}, {"scope","per_patch"}};
    }
    for (bool separated : {false,true}) {
        auto layoutInput = input; layoutInput.settings.uvView = UvView::layout; layoutInput.settings.uvSeparated = separated;
        const auto cold = timed(sample,separated ? "prepare_separated_ms" : "prepare_layout_ms",
            [&]{ return prepareUvComparisonInputs(layoutInput); });
        const auto layout = timed(sample,separated ? "layout_from_surface_separated_ms" : "layout_from_surface_ms",
            [&]{ return uvLayoutMesh((*surface.meshes)[0],separated); });
        verifyLayout((*cold.meshes)[0],layout);
        const auto warm = timed(sample,separated ? "prepare_separated_cached_ms" : "prepare_layout_cached_ms",
            [&]{ return prepareUvComparisonInputs(layoutInput, {}, qualities); });
        verifyLayout((*cold.meshes)[0],(*warm.meshes)[0]);
        require(warm.buffers[0].uvQuality->measurements == q->measurements, "Layout rebuilt measurements");
        require(warm.buffers[0].uvQuality->findings == cold.buffers[0].uvQuality->findings, "Cached findings differ");
        sample[separated ? "separated_layout" : "layout"] = payload((*cold.meshes)[0]);
    }
    return sample;
}
}
int main(int argc, char** argv)
{
    try {
        selfTest();
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { return 0; }
        require(argc == 3,"Usage: woby_uv_stages_probe model.obj samples");
        const int count = std::stoi(argv[2]); require(count > 0 && count <= 5,"Invalid sample count");
        const auto input = snapshot(loadObjMesh(argv[1]));
        Json output{{"model",argv[1]}, {"sizeof_triangle",sizeof(UvTriangleQuality)},
            {"sizeof_display_face",sizeof(UvQualityFace)}, {"samples",Json::array()}};
        for (int i=0; i<count; ++i) {
            std::cerr << "Sample " << i+1 << '\n';
            output["samples"].push_back(measure(input));
        }
        std::cout << output.dump(2) << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
