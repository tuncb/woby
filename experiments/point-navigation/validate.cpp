#include "adaptive_points.h"
#include "graphics_helpers.h"
#include "math_utils.h"
#include "obj_mesh.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace g=woby::graphics;
namespace {
struct Resources {
    bool initialized=false;
    woby::GpuMesh mesh;
    woby::AdaptivePointRuntime runtime;
    g::ProgramHandle reference{};
    g::TextureHandle color{},ids{},depth{};
    g::FrameBufferHandle target{};
    ~Resources() {
        if (!initialized) return;
        woby::destroyAdaptivePoints(runtime);
        woby::destroyGpuMesh(mesh);
        if (g::isValid(reference)) g::destroy(reference);
        if (g::isValid(target)) g::destroy(target);
        for (const auto texture:{color,ids,depth}) if (g::isValid(texture)) g::destroy(texture);
        g::shutdown();
    }
};
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
std::vector<uint64_t> readWinners(Resources& resources,uint32_t width,uint32_t height,uint32_t samples) {
    std::vector<uint64_t> result(size_t(width)*height*4);
    const auto ready=g::readBuffer(resources.runtime.winners,result.data());
    while (g::frame()<ready) {}
    result.resize(size_t(width)*height*samples);
    return result;
}
uint64_t fingerprint(const std::vector<uint64_t>& values) {
    uint64_t hash=14695981039346656037ull;
    for (const auto value:values) { hash^=value; hash*=1099511628211ull; }
    return hash;
}
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"Usage: woby_point_validation MODEL BASELINE_RASTER_SHADER OUTPUT_JSON");
        const auto model=std::filesystem::absolute(argv[1]);
        const auto baseline=std::filesystem::absolute(argv[2]);
        const auto output=std::filesystem::absolute(argv[3]);
        require(!std::filesystem::exists(output),"Output already exists");
        const auto assets=std::filesystem::absolute(argv[0]).parent_path()/"assets";
        auto source=woby::loadObjMesh(model);
        auto prepared=woby::prepareSceneMesh(source,woby::gpuMeshPoints);
        require(prepared.has_value() && prepared->pointCloud!=nullptr,"Point preparation failed");
        prepared->compactOnly=true;
        prepared->pointVertexIndices.clear();
        Resources resources;
        resources.initialized=g::init({}); require(resources.initialized,"Graphics initialization failed");
        require(g::getCaps()->supported&WOBY_GPU_CAPS_OPAQUE_POINTS,"64-bit point atomics are unavailable");
        auto upload=woby::beginGpuPointUpload(std::move(*prepared),source,woby::meshVertexLayout(),false);
        woby::waitGpuMeshUpload(upload); resources.mesh=std::move(upload.mesh);
        resources.reference=g::createProgram(woby::loadShader(baseline),true);
        const auto bounds=source.bounds;
        source={}; // Keep only the prepared point cloud for this rendering check.
        constexpr uint32_t width=1280,height=687;
        nlohmann::json report={{"model",model.string()},{"baseline_shader",baseline.string()},
            {"points",resources.mesh.pointCloud->points.size()},{"width",width},{"height",height},
            {"cases",nlohmann::json::array()}};
        double now=0;
        for (const uint32_t samples:{1u,4u}) {
            if (g::isValid(resources.target)) g::destroy(resources.target);
            for (const auto texture:{resources.color,resources.ids,resources.depth}) if (g::isValid(texture)) g::destroy(texture);
            const auto flags=samples==4?WOBY_GPU_TEXTURE_RT_MSAA_X4:WOBY_GPU_TEXTURE_RT;
            resources.color=g::createTexture2D(width,height,false,1,g::TextureFormat::RGBA8,flags);
            resources.ids=g::createTexture2D(width,height,false,1,g::TextureFormat::RGBA8,flags);
            resources.depth=g::createTexture2D(width,height,false,1,g::TextureFormat::D24S8,flags);
            const std::array targets{resources.color,resources.ids,resources.depth};
            resources.target=g::createFrameBuffer(3,targets.data());
            g::setViewFrameBuffer(0,resources.target); g::setViewRect(0,0,0,width,height);
            g::setViewClear(0,WOBY_GPU_CLEAR_COLOR|WOBY_GPU_CLEAR_DEPTH,0x000000ff,1);
            for (const float zoom:{1.0f,.35f}) for (const float yaw:{0.0f,.31f}) for (const float size:{1.0f,4.0f,8.0f}) {
                auto camera=woby::frameCameraBounds(bounds);
                camera.distance*=zoom; camera.yawRadians+=yaw;
                const auto view=woby::scenePickView(camera,woby::SceneUpAxis::z,bounds,width,height,false,1);
                g::setViewTransform(0,view.view.data(),view.renderProjection.data(),true);
                resources.runtime.keys.clear(); // Also invalidate across a sample-count change.
                woby::prepareAdaptivePoints(resources.runtime,assets,view,false,now++);
                require(resources.runtime.enabled,"Optimized point renderer is unavailable");
                for (uint32_t index=0;index<resources.mesh.pointCloud->groups.size();++index) {
                    const auto& group=resources.mesh.pointCloud->groups[index];
                    if (group.firstId==group.endId) continue;
                    woby::SceneDrawItem item; item.groupIndex=index; item.points=true; item.pointSize=size;
                    item.color={.3f,.4f,.6f,1}; woby::identityMatrix(item.model.data());
                    require(woby::queueAdaptivePoints(resources.runtime,resources.mesh,item,group.firstId),"Point queue rejected a group");
                }
                woby::submitAdaptivePoints(resources.runtime,0,true);
                const auto actual=readWinners(resources,width,height,samples);
                std::vector<g::OpaquePointGroup> groups;
                std::vector<g::OpaquePointTask> tasks;
                for (uint32_t index=0;index<resources.runtime.keys.size();++index) {
                    groups.push_back(resources.runtime.keys[index].group);
                    for (const auto& range:resources.runtime.full[index].ranges) for (uint32_t used=0;used<range.count;) {
                        const auto offset=range.begin+used;
                        const auto count=std::min({4096u,range.count-used,woby::pointChunkSize-offset%woby::pointChunkSize});
                        tasks.push_back({resources.mesh.pointChunks[offset/woby::pointChunkSize],offset%woby::pointChunkSize,count,index,false});
                        used+=count;
                    }
                }
                // The old root is the unchanged first 80 bytes of the current
                // root. The old shader executes its original sample loop.
                const g::PointPrograms programs{resources.runtime.clear,resources.reference,resources.runtime.resolveIds};
                g::submitOpaquePoints(0,g::PointBackend::atomic64,resources.runtime.winners,{},programs,groups,tasks,{},true,false);
                const auto expected=readWinners(resources,width,height,samples);
                uint64_t differences=0,covered=0;
                for (size_t i=0;i<actual.size();++i) { differences+=actual[i]!=expected[i]; covered+=expected[i]!=0; }
                auto row=nlohmann::json{{"samples",samples},{"zoom",zoom},{"yaw",yaw},{"point_size",size},
                    {"compared_samples",actual.size()},{"covered_samples",covered},{"different_depth_id_pairs",differences},
                    {"reference_fingerprint",fingerprint(expected)},{"optimized_fingerprint",fingerprint(actual)}};
                report["cases"].push_back(row); std::cout<<row.dump()<<std::endl;
                std::ofstream file(output); file<<report.dump(2)<<'\n';
                require(bool(file),"Cannot write the validation report");
                require(differences==0,"Point depth or original ID differs from the baseline shader");
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
