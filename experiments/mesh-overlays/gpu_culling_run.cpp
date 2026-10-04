#include "visibility_run.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace woby::overlay {
namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
double median(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    const auto n=values.size(); return n%2?values[n/2]:(values[n/2-1]+values[n/2])*.5;
}
void write(const std::filesystem::path& path,const Json& value) {
    std::ofstream stream(path); stream << value.dump(2) << '\n';
    if (!stream) throw std::runtime_error("Cannot write GPU culling experiment results");
}
size_t colorDifferences(const Capture& a,const Capture& b) {
    size_t count=0;
    for (size_t p=0;p<a.rgba.size();p+=4)
        if (!std::equal(a.rgba.begin()+static_cast<ptrdiff_t>(p),a.rgba.begin()+static_cast<ptrdiff_t>(p+4),
                b.rgba.begin()+static_cast<ptrdiff_t>(p))) ++count;
    return count;
}
}
void measureGpuCulling(Renderer& r,const std::array<float,16>& projection,const VisibilityRun& options) {
    const auto method=gpu::get_device_caps(r.device).fragment_barycentric?Method::barycentric:Method::pulled;
    // Allocate before timing any variant so controls have the same memory pressure.
    prepareGpuCulling(r);
    Json report={{"schema",1},{"experiment","vertex_gpu_culling"},{"model",options.model},
        {"device",gpu::get_device_caps(r.device).device_name},{"width",r.options.width},{"height",r.options.height},
        {"samples",r.options.samples},{"id_attachment",r.options.ids},{"camera_matrix",projection},
        {"zoom",options.zoom},{"orthographic",options.orthographic},{"rounds",options.rounds},
        {"vertices",r.scene.vertexCount},{"triangles",r.scene.triangleCount},{"markers",r.scene.markerCount},
        {"groups",r.scene.groups.size()},{"retained_geometry_bytes",r.scene.bytes},
        {"culling_buffer_bytes",r.culling.residentBytes},{"compaction_blocks",r.culling.blockCount},
        {"scan_levels",r.culling.levels.size()},{"surface_method",methodName(method)},
        {"warmup_seconds",options.warmup},{"measurement_seconds",options.seconds},
        {"notes","Every timed frame builds current-frame visibility, stable GPU compaction and indirect arguments. Total GPU time includes attachment clear/store/load, depth pyramid, all compute passes, barriers, indirect marker draws and MSAA resolve. Capture and counter readback are outside timed intervals. No CPU visibility readback or selection upload is needed to draw. Full padded-square conservative footprints, min across all MSAA depth samples; transparent occlusion disabled. Split control isolates render-pass break. CPU submission excludes fence wait. Headless Vulkan only; no UI/presentation."},
        {"results",Json::array()}};
    struct Variant { const char* name; Culling mode; };
    const std::array variants{Variant{"all",Culling::none},Variant{"split",Culling::split},
        Variant{"gpu_frustum",Culling::frustum},Variant{"gpu_footprint",Culling::footprint}};
    const std::vector<float> sizes=options.pointSize>0?std::vector<float>{options.pointSize}:std::vector<float>{1,4,8};
    for (const bool solid:{true,false}) for (const float size:sizes) {
        if (options.solid>=0 && solid!=(options.solid!=0)) continue;
        Display display{.method=method,.solid=solid,.edges=solid,.pointSize=size};
        const auto caseName=std::string(solid?"surface_":"points_")+std::to_string(static_cast<int>(size));
        Capture baseline; (void)render(r,display,projection,&baseline);
        if (r.options.ids && std::none_of(baseline.ids.begin(),baseline.ids.end(),[](auto id){return id!=0;}))
            throw std::runtime_error("No visible marker IDs in culling benchmark");
        for (int round=0;round<options.rounds;++round) for (size_t order=0;order<variants.size();++order) {
            const auto& variant=variants[(order+static_cast<size_t>(round))%variants.size()];
            display.culling=variant.mode;
            const auto name=caseName+"-"+variant.name+"-r"+std::to_string(round);
            std::cerr << name << std::endl;
            Capture candidate; (void)render(r,display,projection,&candidate);
            const auto changed=colorDifferences(baseline,candidate);
            size_t changedIds=0;
            for (size_t i=0;i<baseline.sampleIds.size();++i) if (baseline.sampleIds[i]!=candidate.sampleIds[i]) ++changedIds;
            if (round==0) savePng(candidate,options.output/(name+".png"));
            if (changed || changedIds) {
                write(options.output/"failure.json",{{"case",name},{"color_pixel_changes",changed},{"id_sample_changes",changedIds}});
                throw std::runtime_error("GPU culling changed the rendered output: "+name);
            }
            auto since=Clock::now();
            for (size_t frame=0;frame<12 || std::chrono::duration<double>(Clock::now()-since).count()<options.warmup;++frame)
                (void)render(r,display,projection);
            struct Series { const char* name; double Measurement::* member; std::vector<double> values; };
            std::array series{Series{"gpu_ms",&Measurement::totalMs,{}},Series{"surface_ms",&Measurement::surfaceMs,{}},
                Series{"edge_ms",&Measurement::edgeMs,{}},Series{"point_and_resolve_ms",&Measurement::pointMs,{}},
                Series{"culling_ms",&Measurement::cullingMs,{}},Series{"pyramid_ms",&Measurement::pyramidMs,{}},
                Series{"classify_ms",&Measurement::classifyMs,{}},Series{"scan_ms",&Measurement::scanMs,{}},
                Series{"scatter_ms",&Measurement::scatterMs,{}},Series{"cpu_submit_ms",&Measurement::cpuSubmitMs,{}}};
            since=Clock::now();
            do {
                const auto measurement=render(r,display,projection);
                for (auto& s:series) s.values.push_back(measurement.*s.member);
            } while (series[0].values.size()<20 || std::chrono::duration<double>(Clock::now()-since).count()<options.seconds);
            const bool compacted=variant.mode==Culling::frustum || variant.mode==Culling::footprint;
            const uint64_t submitted=compacted?std::accumulate(candidate.cullingCounts.begin(),candidate.cullingCounts.end(),uint64_t{0}):r.scene.markerCount;
            Json result={{"case",caseName},{"method",variant.name},{"round",round},{"frames",series[0].values.size()},
                {"solid",solid},{"point_size",size},{"color_pixel_changes",changed},{"id_sample_changes",changedIds},
                {"submitted_markers",submitted},{"group_counts",candidate.cullingCounts}};
            for (const auto& s:series) { result[s.name]=median(s.values); result[std::string(s.name)+"_samples"]=s.values; }
            report["results"].push_back(result); write(options.output/"results.json",report);
            std::cout << name << " gpu=" << result["gpu_ms"] << " ms kept=" << submitted << std::endl;
        }
    }
}
} // namespace woby::overlay
