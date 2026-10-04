#include "point_renderer.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>

namespace {
namespace p=woby::points;
namespace o=woby::overlay;
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
double percentile(std::vector<double> values,double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(),values.end()); return values[static_cast<size_t>(std::ceil(double(values.size()-1)*fraction))];
}
Json timings(const std::vector<p::Timing>& samples) {
    std::vector<double> gpu,cpu,wall,raster,resolve,clear;
    std::vector<uint64_t> submitted;
    for (const auto& m:samples) { gpu.push_back(m.gpuMs); cpu.push_back(m.cpuMs); wall.push_back(m.wallMs); raster.push_back(m.rasterMs); resolve.push_back(m.resolveMs); clear.push_back(m.clearMs); submitted.push_back(m.submitted); }
    return {{"frames",samples.size()},{"gpu_ms",percentile(gpu,.5)},{"gpu_p95_ms",percentile(gpu,.95)},{"gpu_max_ms",percentile(gpu,1)},
        {"cpu_ms",percentile(cpu,.5)},{"wall_ms",percentile(wall,.5)},{"wall_p95_ms",percentile(wall,.95)},{"raster_ms",percentile(raster,.5)},
        {"clear_surface_ms",percentile(clear,.5)},{"resolve_ms",percentile(resolve,.5)},
        {"gpu_samples_ms",gpu},{"cpu_samples_ms",cpu},{"wall_samples_ms",wall},{"submitted_points",submitted}};
}
void write(const std::filesystem::path& path,const Json& report) { std::ofstream stream(path); stream<<report.dump(2)<<'\n'; if (!stream) throw std::runtime_error("Cannot write point report"); }
Json compare(const o::Capture& a,const o::Capture& b) {
    size_t reference=0,covered=0,extra=0,different=0;
    if (a.sampleIds.size()!=b.sampleIds.size()) throw std::runtime_error("Capture size mismatch");
    for (size_t i=0;i<a.sampleIds.size();++i) {
        if (b.sampleIds[i]) { ++reference; if (a.sampleIds[i]) ++covered; }
        else if (a.sampleIds[i]) ++extra;
        if (a.sampleIds[i]!=b.sampleIds[i]) ++different;
    }
    return {{"reference_covered_samples",reference},{"retained_covered_samples",covered},{"coverage",reference?double(covered)/double(reference):1},
        {"extra_samples",extra},{"different_ids",different},{"identical_rgba",a.rgba==b.rgba}};
}
p::Matrix camera(const woby::Bounds& bounds,uint32_t width,uint32_t height,float angle,float zoom) {
    auto c=woby::frameCameraBounds(bounds); c.distance*=zoom; c.yawRadians+=angle; c.pitchRadians+=std::sin(angle)*.12f;
    const auto range=woby::cameraDepthRange(c,bounds); float view[16],projection[16];
    bx::mtxLookAt(view,woby::cameraEye(c),woby::cameraLookAt(c),woby::cameraUp(c)); const float aspect=float(width)/float(height);
    bx::mtxProj(projection,woby::cameraViewportFov(c,aspect),aspect,range.nearPlane,range.farPlane,false);
    for (size_t i=0;i<4;++i) projection[i*4+2]=projection[i*4+3]-projection[i*4+2];
    p::Matrix result; bx::mtxMul(result.data(),view,projection); return result;
}
}
int main(int argc,char** argv) {
    try {
        std::filesystem::path model,output; std::string mode="compute";
        o::Options options{1280,720,1,true}; uint32_t frames=12,navigationFrames=90,rounds=3,maximum=2000000;
        float pointSize=0,zoom=1; bool solid=false,navigationCuts=false; double targetMs=8;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i]; if (i+1==argc) throw std::invalid_argument("Missing option value"); const std::string value=argv[++i];
            if (arg=="--model") model=woby::pathFromUtf8(value); else if (arg=="--output") output=woby::pathFromUtf8(value);
            else if (arg=="--mode") mode=value;
            else if (arg=="--width") options.width=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--height") options.height=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--samples") options.samples=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--frames") frames=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--navigation-frames") navigationFrames=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--rounds") rounds=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--budget") maximum=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--point-size") pointSize=std::stof(value);
            else if (arg=="--zoom") zoom=std::stof(value);
            else if (arg=="--target-ms") targetMs=std::stod(value);
            else if (arg=="--solid" && (value=="0" || value=="1")) solid=value=="1";
            else if (arg=="--navigation-cuts" && (value=="0" || value=="1")) navigationCuts=value=="1";
            else throw std::invalid_argument("Unknown option: "+arg);
        }
        if (model.empty() || output.empty() || (mode!="compute" && mode!="control") || frames<3 || frames>1000 || navigationFrames<2 || navigationFrames>1000
            || rounds<1 || rounds>10 || maximum<4096 || maximum>10000000 || !std::isfinite(targetMs) || targetMs<=0 || targetMs>100
            || !std::isfinite(zoom) || zoom<=0 || !std::isfinite(pointSize) || pointSize<0 || pointSize>40)
            throw std::invalid_argument("Usage: woby_point_prototype --model FILE --output NEW_DIR [--mode compute|control --samples 1|4 --rounds 3 --frames 12 --navigation-frames 90 --point-size 4 --budget 2000000 --target-ms 8 --solid 0|1 --zoom 1]");
        o::validate(options,{.pointSize=pointSize?pointSize:4});
        output=std::filesystem::absolute(output); if (std::filesystem::exists(output)) throw std::invalid_argument("Output must be new"); std::filesystem::create_directories(output);
        std::cerr<<"Loading source mesh"<<std::endl; auto start=Clock::now();
        const auto mesh=woby::loadObjMesh(std::filesystem::absolute(model)); const double loadMs=elapsed(start);
        Json report={{"schema",1},{"mode",mode},{"model",std::filesystem::absolute(model).string()},{"width",options.width},{"height",options.height},{"samples",options.samples},
            {"source_vertices",mesh.vertices.size()},{"source_triangles",mesh.indices.size()/3},{"source_precise_positions",mesh.precisePositions.size()},
            {"source_load_ms",loadMs},{"opaque_surfaces",solid},{"zoom",zoom},{"rounds",rounds},{"point_budget_max",maximum},{"target_gpu_ms",targetMs},
            {"navigation_cuts",navigationCuts},{"results",Json::array()}};
        write(output/"results.json",report);
        const auto projection=camera(mesh.bounds,options.width,options.height,0,zoom);
        const std::vector<float> sizes=pointSize?std::vector<float>{pointSize}:std::vector<float>{1,4,8};
        if (mode=="control") {
            o::Renderer renderer; o::initialize(renderer,options); start=Clock::now(); o::upload(renderer,mesh,woby::gpuMeshPoints);
            report["upload_ms"]=elapsed(start); report["geometry_bytes"]=renderer.scene.bytes; report["device"]=gpu::get_device_caps(renderer.device).device_name;
            for (uint32_t round=0;round<rounds;++round) for (size_t order=0;order<sizes.size();++order) {
                const auto size=sizes[(order+round)%sizes.size()]; const o::Display display{.solid=solid,.edges=false,.pointSize=size};
                for (uint32_t frame=0;frame<3;++frame) (void)o::render(renderer,display,projection);
                std::vector<p::Timing> samples;
                for (uint32_t frame=0;frame<frames;++frame) {
                    start=Clock::now(); const auto t=o::render(renderer,display,projection);
                    samples.push_back({.gpuMs=t.totalMs,.rasterMs=t.pointMs,.cpuMs=t.cpuSubmitMs,.wallMs=elapsed(start),.submitted=renderer.scene.markerCount});
                }
                o::Capture capture; (void)o::render(renderer,display,projection,&capture);
                if (std::none_of(capture.ids.begin(),capture.ids.end(),[](auto id){return id!=0;})) throw std::runtime_error("Empty control rendering");
                o::savePng(capture,output/("control-"+std::to_string(int(size))+"-r"+std::to_string(round)+".png"));
                auto entry=timings(samples); entry["point_size"]=size; entry["round"]=round; entry["kind"]="quad_full";
                report["results"].push_back(entry); write(output/"results.json",report);
                std::cout<<"quad size="<<size<<" gpu="<<entry["gpu_ms"]<<std::endl;
            }
            return 0;
        }
        std::cerr<<"Building compact spatial hierarchy"<<std::endl; start=Clock::now(); const auto cloud=p::buildCloud(mesh);
        report["hierarchy_ms"]=elapsed(start); report["point_count"]=cloud.points.size(); report["proxy_count"]=cloud.proxies.size(); report["hierarchy_nodes"]=cloud.nodes.size();
        report["compact_cpu_bytes"]=(cloud.points.size()+cloud.proxies.size())*sizeof(p::Point)+cloud.nodes.size()*sizeof(p::Node)+cloud.sourceVertices.size()*4;
        std::cerr<<"Uploading compact chunks"<<std::endl; p::PointRenderer renderer; start=Clock::now(); p::initialize(renderer,cloud,mesh,options);
        report["upload_ms"]=elapsed(start); report["geometry_bytes"]=renderer.geometryBytes; report["visibility_bytes"]=renderer.visibilityBytes;
        report["device"]=gpu::get_device_caps(renderer.gpu.device).device_name; report["allocation_points"]=renderer.chunkPoints;
        report["notes"]="Unquantized float positions plus stable IDs; fully resident compact hierarchy; CPU source mesh retained by adapter. 64-bit atomic full-circle visibility with true per-sample circles, not legacy pixel-frequency circle discard. Headless serialized frames, no presentation. Navigation CPU timings include hierarchy selection; wall timings include submission and fence wait. Captures and reference rendering excluded. Refinement replays all source points into persistent winners. No transparency, paging, or production integration.";
        write(output/"results.json",report); const auto full=p::allPoints(cloud);
        for (uint32_t round=0;round<rounds;++round) for (size_t order=0;order<sizes.size();++order) {
            const auto size=sizes[(order+round)%sizes.size()];
            const auto name=std::to_string(int(size))+"-r"+std::to_string(round); std::cerr<<"compute "<<name<<std::endl;
            for (uint32_t frame=0;frame<3;++frame) (void)p::render(renderer,cloud,full,projection,size,solid,true);
            std::vector<p::Timing> raw;
            for (uint32_t frame=0;frame<frames;++frame) raw.push_back(p::render(renderer,cloud,full,projection,size,solid,true));
            auto rawResult=timings(raw); rawResult["point_size"]=size; rawResult["round"]=round; rawResult["kind"]="compute_full";
            report["results"].push_back(rawResult); write(output/"results.json",report);
            std::cout<<"full size="<<size<<" gpu="<<rawResult["gpu_ms"]<<std::endl;
            uint32_t budget=navigationCuts?maximum:std::min(maximum,500000u); std::vector<p::Timing> navigation; std::vector<double> selectionMs; std::vector<uint32_t> visited;
            p::Matrix view=projection; p::Selection selected;
            for (uint32_t frame=0;frame<navigationFrames;++frame) {
                view=camera(mesh.bounds,options.width,options.height,float(frame)*.006f,zoom);
                start=Clock::now(); selected=navigationCuts?p::navigationDetail(cloud,budget)
                    :p::selectDetail(cloud,view,options.width,options.height,size,budget,std::min(1.0f,size*.5f)); const double selectMs=elapsed(start);
                auto t=p::render(renderer,cloud,selected,view,size,solid,true); t.cpuMs+=selectMs; t.wallMs+=selectMs; navigation.push_back(t);
                selectionMs.push_back(selectMs); visited.push_back(selected.visited);
                budget=p::adjustedBudget(budget,t.rasterMs,navigationCuts?targetMs:std::max(.5,targetMs-t.clearMs-t.resolveMs),maximum);
            }
            o::Capture coarse,reference,refined; (void)p::render(renderer,cloud,selected,view,size,solid,true,&coarse);
            (void)p::render(renderer,cloud,full,view,size,solid,true,&reference);
            if (std::none_of(reference.ids.begin(),reference.ids.end(),[](auto id){return id!=0;})) throw std::runtime_error("Empty compute rendering");
            o::savePng(coarse,output/("navigation-"+name+".png")); o::savePng(reference,output/("full-"+name+".png"));
            auto navResult=timings(navigation); navResult["point_size"]=size; navResult["round"]=round; navResult["kind"]="navigation";
            navResult["spacing_pixels"]=std::min(1.0f,size*.5f); navResult["selection_samples_ms"]=selectionMs; navResult["visited_nodes"]=visited; navResult["last_view"]=view; navResult["last_view_quality"]=compare(coarse,reference);
            report["results"].push_back(navResult); write(output/"results.json",report);
            std::cout<<"navigation size="<<size<<" gpu="<<navResult["gpu_ms"]<<" wall_p95="<<navResult["wall_p95_ms"]<<" coverage="<<navResult["last_view_quality"]["coverage"]<<std::endl;
            (void)p::render(renderer,cloud,selected,view,size,solid,true);
            p::Refinement cursor; std::vector<p::Timing> refinement;
            while (cursor.processed<full.points) {
                start=Clock::now(); const auto batch=p::nextRefinement(full,cursor,budget); const double selectMs=elapsed(start);
                auto t=p::render(renderer,cloud,batch,view,size,solid,false); t.cpuMs+=selectMs; t.wallMs+=selectMs; refinement.push_back(t);
                budget=p::adjustedBudget(budget,t.rasterMs,navigationCuts?targetMs:std::max(.5,targetMs-t.clearMs-t.resolveMs),maximum);
            }
            (void)p::render(renderer,cloud,{},view,size,solid,false,&refined);
            const auto quality=compare(refined,reference);
            if (refined.rgba!=reference.rgba || refined.sampleIds!=reference.sampleIds) throw std::runtime_error("Progressive refinement differs from full compute reference");
            auto refinedResult=timings(refinement); refinedResult["point_size"]=size; refinedResult["round"]=round; refinedResult["kind"]="refinement";
            refinedResult["quality"]=quality; refinedResult["total_wall_ms"]=std::accumulate(refinement.begin(),refinement.end(),0.0,[](double sum,const auto& t){return sum+t.wallMs;});
            refinedResult["source_points_processed"]=cursor.processed; report["results"].push_back(refinedResult);
            // An already refined stationary view only resolves its cached visibility.
            std::vector<p::Timing> idle;
            for (uint32_t frame=0;frame<frames;++frame) idle.push_back(p::render(renderer,cloud,{},view,size,solid,false));
            auto idleResult=timings(idle); idleResult["point_size"]=size; idleResult["round"]=round; idleResult["kind"]="stationary_cached"; report["results"].push_back(idleResult);
            if (round==0) {
                std::vector<size_t> visible;
                for (size_t i=0;i<reference.sampleIds.size();++i) if (reference.sampleIds[i]) visible.push_back(i);
                std::vector<double> picking; uint64_t tested=0; uint32_t hits=0;
                const std::array<std::array<float,2>,4> offsets{{{.375f,.125f},{.875f,.375f},{.125f,.625f},{.625f,.875f}}};
                for (uint32_t i=0;i<32;++i) {
                    const size_t index=visible[size_t(i)*visible.size()/32],pixel=index/options.samples;
                    const auto x=static_cast<uint32_t>(pixel%options.width),y=static_cast<uint32_t>(pixel/options.width);
                    const auto offset=options.samples==1?std::array{.5f,.5f}:offsets[index%options.samples]; start=Clock::now();
                    const auto result=p::pick(cloud,view,options.width,options.height,size,float(x)+offset[0],float(y)+offset[1]);
                    picking.push_back(elapsed(start)); tested+=result.tested; if (result.id) ++hits;
                    if (!solid && result.id!=reference.sampleIds[index]) throw std::runtime_error("Precise point query differs from GPU visibility");
                }
                report["picking"].push_back({{"point_size",size},{"queries",32},{"query_distribution","uniform over covered samples"},{"hits",hits},{"tested_source_points",tested},{"median_ms",percentile(picking,.5)},{"p95_ms",percentile(picking,.95)}});
            }
            write(output/"results.json",report);
            std::cout<<"refined size="<<size<<" frames="<<refinement.size()<<" wall="<<refinedResult["total_wall_ms"]<<std::endl;
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<"Point prototype: "<<error.what()<<'\n'; return 1; }
}
