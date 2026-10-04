#include "renderer.h"
#include "visibility_run.h"
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
namespace o=woby::overlay;
using Clock=std::chrono::steady_clock;
using Json=nlohmann::json;
struct Scenario { std::string name; o::Display display; };
double median(std::vector<double> values) {
    std::sort(values.begin(),values.end());
    const size_t n=values.size();
    return n%2?values[n/2]:(values[n/2-1]+values[n/2])*.5;
}
Json summary(const std::vector<o::Measurement>& samples) {
    std::vector<double> total,surfaces,edges,points,cpu;
    for (const auto& m:samples) {
        total.push_back(m.totalMs); surfaces.push_back(m.surfaceMs); edges.push_back(m.edgeMs);
        points.push_back(m.pointMs); cpu.push_back(m.cpuSubmitMs);
    }
    Json result={{"frames",samples.size()},{"gpu_ms",median(total)},{"cpu_submit_ms",median(cpu)},
        {"draws",samples.front().draws},{"gpu_samples_ms",total},{"cpu_samples_ms",cpu}};
    if (samples.front().separated) {
        result["surface_ms"]=median(surfaces); result["edge_ms"]=median(edges); result["point_and_resolve_ms"]=median(points);
    }
    return result;
}
void writeJson(const std::filesystem::path& path,const Json& data) {
    std::ofstream stream(path); stream << data.dump(2) << '\n';
    if (!stream) throw std::runtime_error("Cannot write experiment results");
}
}

int main(int argc,char** argv) {
    try {
        std::filesystem::path model,output;
        o::Options options;
        int rounds=3;
        double seconds=2,warmup=1;
        bool fixture=false,orthographic=false,visibility=false;
        float zoom=1;
        float pointSize=0;
        int solid=-1;
        std::string methodFilter,scenarioFilter;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--fixture") { fixture=true; continue; }
            if (arg=="--orthographic") { orthographic=true; continue; }
            if (arg=="--visibility") { visibility=true; continue; }
            if (i+1==argc) throw std::invalid_argument("Option needs a value: "+arg);
            const std::string value=argv[++i];
            if (arg=="--model") model=woby::pathFromUtf8(value);
            else if (arg=="--output") output=woby::pathFromUtf8(value);
            else if (arg=="--width") options.width=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--height") options.height=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--samples") options.samples=static_cast<uint32_t>(std::stoul(value));
            else if (arg=="--ids") { if (value!="0" && value!="1") throw std::invalid_argument("IDs must be 0 or 1"); options.ids=value=="1"; }
            else if (arg=="--rounds") rounds=std::stoi(value);
            else if (arg=="--seconds") seconds=std::stod(value);
            else if (arg=="--warmup") warmup=std::stod(value);
            else if (arg=="--zoom") zoom=std::stof(value);
            else if (arg=="--point-size") { pointSize=std::stof(value); o::validate(options,{.pointSize=pointSize}); }
            else if (arg=="--solid") {
                if (value!="0" && value!="1") throw std::invalid_argument("Solid must be 0 or 1");
                solid=value=="1"?1:0;
            }
            else if (arg=="--method") methodFilter=value;
            else if (arg=="--scenario") scenarioFilter=value;
            else throw std::invalid_argument("Unknown option: "+arg);
        }
        if ((!fixture && model.empty()) || output.empty() || rounds<1 || rounds>10
            || !std::isfinite(seconds) || seconds<=0 || seconds>60
            || !std::isfinite(warmup) || warmup<0 || warmup>60)
            throw std::invalid_argument("Usage: woby_overlay_prototype (--model FILE | --fixture) --output NEW_DIRECTORY [--samples 1|4 --ids 0|1 --rounds 3 --seconds 2 --warmup 1 --method NAME --scenario NAME --zoom 1 --orthographic]");
        if (visibility) options.ids=true;
        o::validate(options,{});
        output=std::filesystem::absolute(output);
        if (std::filesystem::exists(output)) throw std::invalid_argument("Output directory must be new");
        std::filesystem::create_directories(output);
        std::cerr << "Loading " << (fixture?"synthetic fixture":model.string()) << std::endl;
        const auto loadStart=Clock::now();
        const auto mesh=fixture?o::fixtureMesh():woby::loadObjMesh(std::filesystem::absolute(model));
        const double loadMs=std::chrono::duration<double,std::milli>(Clock::now()-loadStart).count();
        o::Renderer renderer; o::initialize(renderer,options);
        std::cerr << "Uploading " << mesh.vertices.size() << " vertices, " << mesh.indices.size()/3 << " triangles" << std::endl;
        const auto uploadStart=Clock::now(); o::upload(renderer,mesh);
        const double uploadMs=std::chrono::duration<double,std::milli>(Clock::now()-uploadStart).count();
        std::array<float,16> projection{};
        if (fixture) bx::mtxIdentity(projection.data());
        else projection=o::fittedProjection(mesh.bounds,options.width,options.height,zoom,orthographic);
        if (visibility) {
            o::measureVisibility(renderer,mesh,projection,{output,fixture?"fixture":std::filesystem::absolute(model).string(),
                rounds,solid,seconds,warmup,pointSize,zoom,orthographic});
            return 0;
        }
        std::vector<o::Method> methods{o::Method::legacy,o::Method::ordered};
        if (gpu::get_device_caps(renderer.device).fragment_barycentric) methods.push_back(o::Method::barycentric);
        methods.push_back(o::Method::pulled);
        if (!methodFilter.empty()) std::erase_if(methods,[&](auto m){return methodFilter!=o::methodName(m);});
        if (methods.empty()) throw std::invalid_argument("Requested method unavailable");
        const std::vector<Scenario> allScenarios{
            {"solid",{.edges=false,.points=false}},
            {"edges",{.solid=false,.points=false}},
            {"solid_edges",{.points=false}},
            {"vertices_1",{.solid=false,.edges=false,.pointSize=1}},
            {"vertices_4",{.solid=false,.edges=false,.pointSize=4}},
            {"vertices_8",{.solid=false,.edges=false,.pointSize=8}},
            {"combined",{}},
            {"xray",{.xray=true}},
            {"transparent",{.opacity=.4f}}};
        auto scenarios=allScenarios;
        if (!scenarioFilter.empty()) std::erase_if(scenarios,[&](const auto& s){return s.name!=scenarioFilter;});
        if (scenarios.empty()) throw std::invalid_argument("Unknown scenario");
        Json report={{"schema",1},{"model",fixture?"fixture":std::filesystem::absolute(model).string()},
            {"device",gpu::get_device_caps(renderer.device).device_name},{"native_barycentric",gpu::get_device_caps(renderer.device).fragment_barycentric},
            {"width",options.width},{"height",options.height},{"samples",options.samples},{"id_attachment",options.ids},
            {"camera_matrix",projection},{"orthographic",orthographic},{"zoom",zoom},
            {"vertices",renderer.scene.vertexCount},{"triangles",renderer.scene.triangleCount},
            {"markers",renderer.scene.markerCount},{"groups",renderer.scene.groups.size()},
            {"retained_geometry_bytes",renderer.scene.bytes},{"edge_index_bytes",mesh.indices.size()*8},
            {"load_ms",loadMs},{"prepare_and_upload_ms",uploadMs},{"rounds",rounds},
            {"warmup_seconds",warmup},{"measurement_seconds",seconds},
            {"notes","Headless fixed-size rendering; production shaders and preparation; scene GPU only, no UI/presentation. All methods retain control edge buffers. Legacy draws X-ray lines in group order. Ordered and barycentric use visible-surface semantics. Shader edge coverage differs. CPU submission excludes fence wait. Captures excluded from timings."},
            {"results",Json::array()}};
        for (int round=0;round<rounds;++round) {
            // Rotate method order between rounds to reduce temperature/order bias.
            for (const auto& scenario:scenarios) for (size_t order=0;order<methods.size();++order) {
                const auto method=methods[(order+static_cast<size_t>(round))%methods.size()];
                auto display=scenario.display; display.method=method;
                const std::string name=scenario.name+"-"+o::methodName(method)+"-r"+std::to_string(round);
                std::cerr << name << std::endl;
                auto since=Clock::now();
                for (size_t frame=0;frame<12 || std::chrono::duration<double>(Clock::now()-since).count()<warmup;++frame)
                    (void)o::render(renderer,display,projection);
                std::vector<o::Measurement> samples; since=Clock::now();
                do { samples.push_back(o::render(renderer,display,projection)); }
                while (samples.size()<20 || std::chrono::duration<double>(Clock::now()-since).count()<seconds);
                auto result=summary(samples);
                result["scenario"]=scenario.name; result["method"]=o::methodName(method); result["round"]=round;
                result["point_size"]=display.pointSize; result["opacity"]=display.opacity; result["xray"]=display.xray;
                o::Capture capture; (void)o::render(renderer,display,projection,&capture);
                o::savePng(capture,output/(name+".png"));
                size_t changed=0,identified=0;
                for (size_t p=0;p<capture.rgba.size();p+=4)
                    if (capture.rgba[p]!=32 || capture.rgba[p+1]!=36 || capture.rgba[p+2]!=42) ++changed;
                for (auto id:capture.ids) if (id) ++identified;
                result["non_background_pixels"]=changed; result["identified_pixels"]=identified;
                if (!changed) throw std::runtime_error("Empty rendering is not a valid performance result");
                report["results"].push_back(result); writeJson(output/"results.json",report);
                std::cout << name << " gpu=" << result["gpu_ms"] << " ms cpu=" << result["cpu_submit_ms"] << " ms" << std::endl;
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << "Overlay experiment: " << error.what() << '\n'; return 1; }
}
