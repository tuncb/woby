#include "adaptive_points.h"
#include "graphics_helpers.h"
#include <bx/math.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace woby {
namespace {
bool same(const AdaptivePointKey& a,const AdaptivePointKey& b) {
    return a.cloud==b.cloud && a.sourceGroup==b.sourceGroup && a.group.model==b.group.model
        && a.group.color==b.group.color && a.group.pointSize==b.group.pointSize
        && a.group.firstId==b.group.firstId && a.group.endId==b.group.endId;
}
void appendTasks(std::vector<graphics::OpaquePointTask>& tasks,const AdaptivePointDraw& draw,
    const points::Selection& selection,uint32_t group,bool query=false) {
    for (const auto& range:selection.ranges) {
        if (range.group!=draw.key.sourceGroup) continue;
        const auto& chunks=range.proxy?draw.mesh->proxyChunks:draw.mesh->pointChunks;
        for (uint32_t used=0;used<range.count;) {
            const uint32_t offset=range.begin+used,chunk=offset/pointChunkSize;
            const uint32_t count=std::min({4096u,range.count-used,pointChunkSize-offset%pointChunkSize});
            tasks.push_back({chunks.at(chunk),offset%pointChunkSize,count,group,query}); used+=count;
        }
    }
}
points::Selection sourceRange(const AdaptivePointKey& key) {
    points::Selection result;
    for (const auto root:key.cloud->roots) {
        const auto& n=key.cloud->nodes[root];
        if (n.group==key.sourceGroup) { result.ranges.push_back({n.begin,n.count,n.group,false}); result.points+=n.count; }
    }
    return result;
}
} // namespace

void prepareAdaptivePoints(AdaptivePointRuntime& runtime,const std::filesystem::path& assets,
    const SceneDrawPlan& plan,const ScenePickView& view,bool adaptive,double now,bool queryEnabled,std::array<float,2> query) {
    runtime.draws.clear(); runtime.active=false; runtime.submitted=0;
    runtime.width=view.width; runtime.height=view.height; runtime.now=now; runtime.adaptive=adaptive;
    runtime.queryEnabled=queryEnabled; runtime.query=query;
    bx::mtxMul(runtime.projection.data(),view.view.data(),view.renderProjection.data());
    const auto* caps=graphics::getCaps();
    runtime.enabled=!runtime.unavailable && (caps->supported&WOBY_GPU_CAPS_OPAQUE_POINTS)!=0
        && view.width && view.height && uint64_t(view.width)*view.height*4*sizeof(uint64_t)<=UINT32_MAX
        && std::none_of(plan.items.begin(),plan.items.end(),[](const auto& item) { return item.color[3]<.999f; });
    if (!runtime.enabled) { runtime.keys.clear(); return; }
    if (!graphics::isValid(runtime.resolve)) {
        try {
            const auto path=assets/"shaders"/rendererShaderFolder(caps->rendererType);
            runtime.clear=graphics::createProgram(loadShader(path/"cs_opaque_clear.bin"),true);
            runtime.raster=graphics::createProgram(loadShader(path/"cs_opaque_raster.bin"),true);
            runtime.resolve=loadProgram(assets,"vs_opaque_resolve.bin","fs_opaque_color.bin");
            runtime.resolveIds=loadProgram(assets,"vs_opaque_resolve.bin","fs_opaque_resolve.bin");
        } catch (const std::exception& error) {
            runtime.error=error.what(); runtime.unavailable=true; runtime.enabled=false;
        }
    }
}
bool queueAdaptivePoints(AdaptivePointRuntime& runtime,const GpuMesh& mesh,const SceneDrawItem& item,uint32_t firstId) {
    if (!runtime.enabled || !mesh.pointCloud || item.importedLines || !firstId) return false;
    const auto& source=mesh.pointCloud->groups.at(item.groupIndex);
    AdaptivePointDraw draw;
    draw.mesh=&mesh; draw.key.cloud=mesh.pointCloud; draw.key.sourceGroup=static_cast<uint32_t>(item.groupIndex);
    auto& group=draw.key.group;
    group.model=item.model; group.color=item.color;
    for (size_t i=0;i<3;++i) group.color[i]=std::min(1.0f,group.color[i]*1.5f);
    group.firstId=firstId; group.endId=firstId+source.endId-source.firstId;
    group.sourceFirstId=source.firstId; group.pointSize=item.pointSize;
    runtime.draws.push_back(std::move(draw)); return true;
}
void submitAdaptivePoints(AdaptivePointRuntime& runtime,graphics::ViewId view,bool markerIds) {
    if (runtime.draws.empty()) {
        runtime.keys.clear(); runtime.full.clear(); runtime.navigation.clear(); runtime.cursors.clear();
        if (runtime.pending.valid() && runtime.pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) (void)runtime.pending.get();
        runtime.total=runtime.refined=0; return;
    }
    const auto* stats=graphics::getStats();
    // Delayed timings from an explicitly full-detail frame must not throttle
    // the interactive budget when the user switches adaptive detail back on.
    if (runtime.adaptive && stats->pointRasterBudgeted && stats->pointRasterCount && stats->pointRasterFrame!=runtime.lastTiming) {
        runtime.lastTiming=stats->pointRasterFrame;
        runtime.budget=points::adjustedBudget(runtime.budget,
            stats->pointRasterMs,8,2000000);
    }
    const bool resized=runtime.width!=runtime.previousWidth || runtime.height!=runtime.previousHeight;
    const bool keysChanged=runtime.keys.size()!=runtime.draws.size() || !std::equal(runtime.keys.begin(),runtime.keys.end(),runtime.draws.begin(),
            [](const auto& key,const auto& draw) { return same(key,draw.key); });
    const bool changed=resized || runtime.adaptive!=runtime.previousAdaptive || runtime.projection!=runtime.previousProjection || keysChanged;
    bool reset=changed || !graphics::isValid(runtime.winners);
    if (resized || !graphics::isValid(runtime.winners)) {
        const auto replacement=graphics::createVertexBufferStorage(static_cast<uint32_t>(uint64_t(runtime.width)*runtime.height*4*sizeof(uint64_t)),{sizeof(uint64_t)});
        if (graphics::isValid(runtime.winners)) graphics::destroy(runtime.winners);
        runtime.winners=replacement;
    }
    if (changed) {
        ++runtime.epoch; runtime.lastChange=runtime.now; runtime.queryValid=false;
        runtime.keys.clear(); runtime.full.clear(); runtime.total=runtime.refined=0;
        for (const auto& draw:runtime.draws) {
            runtime.keys.push_back(draw.key); runtime.full.push_back(sourceRange(draw.key)); runtime.total+=runtime.full.back().points;
        }
        runtime.cursors.assign(runtime.keys.size(),{});
        if (keysChanged) { runtime.navigation.clear(); runtime.navigationBudget=0; }
        runtime.previousProjection=runtime.projection; runtime.previousWidth=runtime.width; runtime.previousHeight=runtime.height;
        runtime.previousAdaptive=runtime.adaptive;
    }
    std::vector<graphics::OpaquePointTask> tasks;
    if (runtime.pending.valid() && runtime.pending.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=runtime.pending.get();
        if (result.epoch==runtime.epoch) {
            for (size_t i=0;i<result.selections.size();++i)
                appendTasks(tasks,runtime.draws[i],result.selections[i],static_cast<uint32_t>(i));
        }
    }
    if (changed && runtime.adaptive && !runtime.pending.valid() && runtime.keys.size()<=32) {
        const auto keys=runtime.keys; const auto matrix=runtime.projection;
        const auto width=runtime.width,height=runtime.height,budget=runtime.budget;
        const auto epoch=runtime.epoch;
        runtime.pending=std::async(std::launch::async,[keys,matrix,width,height,budget,epoch] {
            AdaptivePointCut result; result.epoch=epoch;
            for (const auto& key:keys) {
                auto groups=key.cloud->groups;
                for (auto& group:groups) group.enabled=false;
                groups[key.sourceGroup].enabled=true; groups[key.sourceGroup].model=key.group.model;
                result.selections.push_back(points::selectDetail(*key.cloud,groups,matrix,width,height,key.group.pointSize,
                    std::max(1u,budget/static_cast<uint32_t>(keys.size())),std::min(1.0f,key.group.pointSize*.5f)));
            }
            return result;
        });
    }
    const bool navigating=runtime.adaptive && runtime.now-runtime.lastChange<.15;
    if (navigating) {
        // A cut always covers every spatial region, including during fast turns.
        // It is cached between changes; the GPU rejects complete offscreen circles.
        const bool newCut=runtime.navigation.empty() || runtime.navigationBudget!=runtime.budget;
        if (newCut) {
            runtime.navigationBudget=runtime.budget; runtime.navigation.clear();
            for (const auto& key:runtime.keys)
                runtime.navigation.push_back(points::navigationDetail(*key.cloud,std::max(1u,runtime.budget/static_cast<uint32_t>(runtime.keys.size())),key.sourceGroup));
        }
        if (reset || newCut) {
            for (size_t i=0;i<runtime.draws.size();++i)
                appendTasks(tasks,runtime.draws[i],runtime.navigation[i],static_cast<uint32_t>(i));
        }
    } else {
        uint32_t remaining=runtime.adaptive?runtime.budget:UINT32_MAX;
        for (size_t i=0;i<runtime.draws.size() && remaining;++i) {
            const auto selection=points::nextRefinement(runtime.full[i],runtime.cursors[i],remaining);
            appendTasks(tasks,runtime.draws[i],selection,static_cast<uint32_t>(i));
            runtime.refined+=selection.points; remaining-=static_cast<uint32_t>(selection.points);
        }
    }
    std::array<uint32_t,4> query{};
    if (runtime.queryEnabled && runtime.refined<runtime.total && (!runtime.queryValid || runtime.query!=runtime.previousQuery)) {
        runtime.previousQuery=runtime.query; runtime.queryValid=true;
        float radius=5;
        for (const auto& key:runtime.keys) radius=std::max(radius,key.group.pointSize+5);
        const auto bound=[](float v,uint32_t maximum) { return static_cast<uint32_t>(std::clamp(v,0.0f,float(maximum))); };
        query={bound(std::floor(runtime.query[0]-radius),runtime.width),bound(std::floor(runtime.query[1]-radius),runtime.height),
            bound(std::ceil(runtime.query[0]+radius+1),runtime.width),bound(std::ceil(runtime.query[1]+radius+1),runtime.height)};
        const std::array<float,4> rectangle{float(query[0]),float(query[1]),float(query[2]),float(query[3])};
        for (size_t i=0;i<runtime.draws.size();++i) {
            const auto& draw=runtime.draws[i]; points::Matrix matrix;
            bx::mtxMul(matrix.data(),draw.key.group.model.data(),runtime.projection.data());
            const auto selection=points::queryFootprints(*draw.key.cloud,draw.key.sourceGroup,matrix,runtime.width,runtime.height,
                draw.key.group.pointSize,rectangle);
            appendTasks(tasks,draw,selection,static_cast<uint32_t>(i),true);
        }
    }
    std::vector<graphics::OpaquePointGroup> groups; groups.reserve(runtime.keys.size());
    for (const auto& key:runtime.keys) groups.push_back(key.group);
    for (const auto& task:tasks) runtime.submitted+=task.count;
    graphics::submitOpaquePoints(view,runtime.winners,runtime.clear,runtime.raster,markerIds?runtime.resolveIds:runtime.resolve,groups,tasks,query,reset,runtime.adaptive);
    runtime.active=true; runtime.draws.clear();
}
void destroyAdaptivePoints(AdaptivePointRuntime& runtime) {
    if (runtime.pending.valid()) { runtime.pending.wait(); runtime.pending={}; }
    for (const auto program:{runtime.clear,runtime.raster,runtime.resolve,runtime.resolveIds}) if (graphics::isValid(program)) graphics::destroy(program);
    if (graphics::isValid(runtime.winners)) graphics::destroy(runtime.winners);
    runtime={};
}
void attachPointHierarchies(std::span<ScenePickPart> parts,const std::vector<LoadedModelRuntime>& runtimes) {
    for (auto& part:parts) if (part.sourceMesh && part.fileIndex<runtimes.size())
        part.pointCloud=runtimes[part.fileIndex].gpuMesh.pointCloud.get();
}
} // namespace woby
