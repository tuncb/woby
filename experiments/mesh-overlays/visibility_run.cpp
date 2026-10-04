#include "visibility_run.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
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
    if (!stream) throw std::runtime_error("Cannot write visibility experiment results");
}
size_t colorDifferences(const Capture& a,const Capture& b) {
    size_t count=0;
    for (size_t p=0;p<a.rgba.size();p+=4)
        if (!std::equal(a.rgba.begin()+static_cast<ptrdiff_t>(p),a.rgba.begin()+static_cast<ptrdiff_t>(p+4),
                b.rgba.begin()+static_cast<ptrdiff_t>(p))) ++count;
    return count;
}
}
void measureVisibility(Renderer& r,const Mesh& mesh,const std::array<float,16>& projection,const VisibilityRun& options) {
    if (!r.options.ids) throw std::invalid_argument("Visibility diagnostics require all-sample marker IDs");
    Json report={{"schema",1},{"experiment","vertex_visibility_static_selection"},{"model",options.model},
        {"device",gpu::get_device_caps(r.device).device_name},{"width",r.options.width},{"height",r.options.height},
        {"samples",r.options.samples},{"id_attachment",true},{"camera_matrix",projection},
        {"zoom",options.zoom},{"orthographic",options.orthographic},{"rounds",options.rounds},
        {"vertices",r.scene.vertexCount},{"triangles",r.scene.triangleCount},{"markers",r.scene.markerCount},
        {"retained_geometry_bytes",r.scene.bytes},{"warmup_seconds",options.warmup},{"measurement_seconds",options.seconds},
        {"notes","Offline current-frame depth readback, CPU selection, compaction and upload excluded from draw timings. Oracle selection uses all final MSAA marker IDs and is an upper bound, not an executable faster visibility algorithm. Selections preserve order and original IDs. Center-only selection intentionally tests an unsafe approximation. Reversed-depth min pyramid covers full padded square footprints; no transparent occluders. Quad area is a geometric estimate, not fragment invocation count."},
        {"cases",Json::array()},{"results",Json::array()}};
    std::vector<float> sizes=options.pointSize>0?std::vector<float>{options.pointSize}:std::vector<float>{1,4,8};
    for (const bool solid : {true,false}) for (const float size : sizes) {
        if (options.solid>=0 && solid!=(options.solid!=0)) continue;
        Display display{.method=gpu::get_device_caps(r.device).fragment_barycentric?Method::barycentric:Method::pulled,
            .solid=solid,.edges=solid,.pointSize=size};
        const auto caseName=std::string(solid?"surface_":"points_")+std::to_string(static_cast<int>(size));
        std::cerr << "Inspecting " << caseName << std::endl;
        Capture depth; depth.depthRequested=true;
        auto surfaces=display; surfaces.points=false;
        (void)render(r,surfaces,projection,&depth);
        Capture baseline; (void)render(r,display,projection,&baseline);
        const auto start=Clock::now();
        const auto audit=inspectVisibility(mesh,r.scene,projection,r.options.width,r.options.height,size,depth.minimumSampleDepth);
        const auto oracle=finalMarkerOracle(r.scene,baseline.sampleIds);
        const auto analysisSeconds=std::chrono::duration<double>(Clock::now()-start).count();
        const auto pixels=std::count_if(baseline.ids.begin(),baseline.ids.end(),[](auto id){return id!=0;});
        if (!pixels) throw std::runtime_error("No visible markers in visibility benchmark");
        report["cases"].push_back({{"case",caseName},{"solid",solid},{"point_size",size},{"submitted",audit.submitted},
            {"depth_clipped",audit.depthClipped},{"offscreen",audit.offscreen},{"center_outside",audit.centerOutside},
            {"center_occluded",audit.centerOccluded},{"footprint_occluded",audit.footprintOccluded},
            {"frustum_retained",audit.frustum.markers.size()},{"conservative_retained",audit.conservative.markers.size()},
            {"center_retained",audit.center.markers.size()},{"oracle_retained",oracle.markers.size()},
            {"identified_pixels",pixels},{"projected_quad_pixels",audit.projectedQuadPixels},
            {"offline_classification_seconds",analysisSeconds}});
        struct Variant { const char* name; const MarkerSelection* selection; };
        const std::array variants{Variant{"all",nullptr},Variant{"frustum",&audit.frustum},
            Variant{"conservative",&audit.conservative},Variant{"center",&audit.center},Variant{"oracle",&oracle}};
        for (int round=0;round<options.rounds;++round) for (size_t order=0;order<variants.size();++order) {
            const auto& variant=variants[(order+static_cast<size_t>(round))%variants.size()];
            display.compacted=variant.selection!=nullptr;
            if (variant.selection) installMarkerSelection(r,*variant.selection);
            const std::string name=caseName+"-"+variant.name+"-r"+std::to_string(round);
            std::cerr << name << std::endl;
            Capture candidate; (void)render(r,display,projection,&candidate);
            const auto changed=colorDifferences(baseline,candidate);
            size_t changedIds=0;
            for (size_t i=0;i<baseline.sampleIds.size();++i) if (baseline.sampleIds[i]!=candidate.sampleIds[i]) ++changedIds;
            if (round==0) savePng(candidate,options.output/(name+".png"));
            if (std::string(variant.name)!="center" && (changed || changedIds)) {
                write(options.output/"failure.json",{{"case",name},{"color_pixel_changes",changed},{"id_sample_changes",changedIds}});
                throw std::runtime_error("Conservative/oracle marker selection changed the rendered output: "+name);
            }
            auto since=Clock::now();
            for (size_t frame=0;frame<12 || std::chrono::duration<double>(Clock::now()-since).count()<options.warmup;++frame)
                (void)render(r,display,projection);
            std::vector<double> total,points,surface,cpu; since=Clock::now();
            do {
                const auto m=render(r,display,projection);
                total.push_back(m.totalMs); points.push_back(m.pointMs); surface.push_back(m.surfaceMs); cpu.push_back(m.cpuSubmitMs);
            } while (total.size()<20 || std::chrono::duration<double>(Clock::now()-since).count()<options.seconds);
            report["results"].push_back({{"case",caseName},{"method",variant.name},{"round",round},{"frames",total.size()},
                {"gpu_ms",median(total)},{"point_and_resolve_ms",median(points)},{"surface_ms",median(surface)},
                {"cpu_submit_ms",median(cpu)},{"gpu_samples_ms",total},{"point_samples_ms",points},{"cpu_samples_ms",cpu},
                {"color_pixel_changes",changed},{"id_sample_changes",changedIds},
                {"submitted_markers",variant.selection?variant.selection->markers.size():audit.submitted}});
            write(options.output/"results.json",report);
            std::cout << name << " gpu=" << median(total) << " ms" << std::endl;
        }
    }
}
} // namespace woby::overlay
