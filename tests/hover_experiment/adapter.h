#pragma once
#include "point_index.h"
#include "render_fps_probe.h"
#include "hover_pick.h"
#include "bgfx_helpers.h"
#include <bit>
#include <chrono>
#include <unordered_map>

namespace hover_experiment {
using Clock=std::chrono::steady_clock;
inline double ms(Clock::time_point a,Clock::time_point b) { return std::chrono::duration<double,std::milli>(b-a).count(); }
struct Request {
    bool pending=false, measured=false, readRequested=false;
    uint32_t ready=0, frame=0;
    size_t scenario=0;
    Clock::time_point issued;
    std::array<uint32_t,4> data{};
    Hit expected;
    bool validate=false;
    bgfx::TextureHandle staging=BGFX_INVALID_HANDLE;
};
struct Runtime {
    Index index;
    std::vector<Range> ranges;
    std::vector<Group> groups;
    std::vector<woby::ScenePickPart> parts;
    std::unordered_map<size_t,uint32_t> groupAtOffset;
    bgfx::IndexBufferHandle order=BGFX_INVALID_HANDLE;
    bgfx::VertexBufferHandle clusters=BGFX_INVALID_HANDLE;
    bgfx::DynamicVertexBufferHandle groupBuffer=BGFX_INVALID_HANDLE, winners=BGFX_INVALID_HANDLE, partials=BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle candidates=BGFX_INVALID_HANDLE, reduce=BGFX_INVALID_HANDLE;
    bgfx::UniformHandle view=BGFX_INVALID_HANDLE, projection=BGFX_INVALID_HANDLE, query=BGFX_INVALID_HANDLE, dispatch=BGFX_INVALID_HANDLE, reduceParams=BGFX_INVALID_HANDLE;
    bgfx::TextureHandle result=BGFX_INVALID_HANDLE;
    std::array<Request,8> requests;
    std::vector<render_fps::Json> events, completions;
    render_fps::Json metadata;
    bool built=false,gpu=false;
    uint32_t frame=0, partialCount=0;
    uint64_t skipped=0;
    std::optional<woby::HoveredVertex> last;
};
inline bgfx::VertexLayout vec4Layout() { bgfx::VertexLayout l; l.begin().add(bgfx::Attrib::Position,4,bgfx::AttribType::Float).end(); return l; }
inline uint32_t bytes(size_t n) { if (n>UINT32_MAX) { throw std::runtime_error("Experiment buffer too large"); } return static_cast<uint32_t>(n); }
inline void build(Runtime& r,const woby::UiState& ui,const std::vector<woby::LoadedModelRuntime>& runtimes) {
    if (ui.files.size()!=1 || runtimes.size()!=1) { throw std::runtime_error("Experiment expects one model per process"); }
    const auto start=Clock::now();
    const auto& mesh=ui.files[0].mesh; const auto& gpu=runtimes[0].gpuMesh;
    for (size_t i=0;i<gpu.nodeRanges.size();++i) {
        const auto& n=gpu.nodeRanges[i]; r.ranges.push_back({n.pointIndexOffset,n.pointIndexCount});
        if (n.pointIndexCount) { r.groupAtOffset.emplace(n.triangleIndexOffset,static_cast<uint32_t>(i)); }
    }
    r.index=buildIndex(mesh.vertices,gpu.pointVertexIndices,r.ranges);
    r.groups.resize(r.ranges.size()); r.built=true;
    r.metadata={{"build_ms",ms(start,Clock::now())},{"points",gpu.pointVertexIndices.size()},
        {"nodes",r.index.nodes.size()},{"clusters",r.index.clusters.size()},
        {"cpu_index_bytes",r.index.order.capacity()*sizeof(uint32_t)+r.index.nodes.capacity()*sizeof(Node)+r.index.roots.capacity()*sizeof(uint32_t)+r.index.clusters.capacity()*sizeof(Cluster)}};
    std::cout << "hover_index " << r.metadata.dump() << std::endl;
}
inline void setupGpu(Runtime& r,const std::filesystem::path& assets) {
    const uint64_t caps=bgfx::getCaps()->supported;
    if ((caps & BGFX_CAPS_TEXTURE_READ_BACK)==0 || (caps & BGFX_CAPS_TEXTURE_BLIT)==0 || bgfx::getCaps()->limits.maxComputeBindings<6) {
        throw std::runtime_error("Compute experiment requires readback, blit, and six compute bindings");
    }
    const auto shaderRoot=assets/"shaders"/woby::rendererShaderFolder(bgfx::getRendererType());
    r.candidates=bgfx::createProgram(woby::loadShader(shaderRoot/"cs_hover_candidates.bin"),true);
    r.reduce=bgfx::createProgram(woby::loadShader(shaderRoot/"cs_hover_reduce.bin"),true);
    r.view=bgfx::createUniform("u_hoverView",bgfx::UniformType::Mat4);
    r.projection=bgfx::createUniform("u_hoverProjection",bgfx::UniformType::Mat4);
    r.query=bgfx::createUniform("u_hoverQuery",bgfx::UniformType::Vec4);
    r.dispatch=bgfx::createUniform("u_hoverDispatch",bgfx::UniformType::Vec4);
    r.reduceParams=bgfx::createUniform("u_hoverReduce",bgfx::UniformType::Vec4);
    r.order=bgfx::createIndexBuffer(bgfx::copy(r.index.order.data(),bytes(r.index.order.size()*4)),BGFX_BUFFER_INDEX32|BGFX_BUFFER_COMPUTE_READ);
    std::vector<Vec4> packed; packed.reserve(r.index.clusters.size()*3);
    for (const auto& c:r.index.clusters) {
        packed.push_back({c.box.low[0],c.box.low[1],c.box.low[2],std::bit_cast<float>(c.begin)});
        packed.push_back({c.box.high[0],c.box.high[1],c.box.high[2],std::bit_cast<float>(c.count)});
        packed.push_back({std::bit_cast<float>(c.group),0,0,0});
    }
    r.clusters=bgfx::createVertexBuffer(bgfx::copy(packed.data(),bytes(packed.size()*sizeof(Vec4))),vec4Layout(),BGFX_BUFFER_COMPUTE_READ);
    r.groupBuffer=bgfx::createDynamicVertexBuffer(bytes(r.groups.size()*9),vec4Layout(),BGFX_BUFFER_COMPUTE_READ);
    constexpr uint16_t flags=BGFX_BUFFER_COMPUTE_READ_WRITE|BGFX_BUFFER_COMPUTE_FORMAT_32X4|BGFX_BUFFER_COMPUTE_TYPE_UINT;
    r.winners=bgfx::createDynamicVertexBuffer(bytes(r.index.clusters.size()),vec4Layout(),flags);
    r.partialCount=bytes((r.index.clusters.size()+255)/256);
    r.partials=bgfx::createDynamicVertexBuffer(r.partialCount,vec4Layout(),flags);
    r.result=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32U,BGFX_TEXTURE_COMPUTE_WRITE|BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP);
    for (auto& slot:r.requests) { slot.staging=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32U,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST); }
    if (!bgfx::isValid(r.candidates)||!bgfx::isValid(r.reduce)||!bgfx::isValid(r.order)||!bgfx::isValid(r.clusters)||!bgfx::isValid(r.groupBuffer)||!bgfx::isValid(r.winners)||!bgfx::isValid(r.partials)||!bgfx::isValid(r.result)) {
        throw std::runtime_error("Failed to allocate compute experiment resources");
    }
    for (const auto& slot:r.requests) { if (!bgfx::isValid(slot.staging)) { throw std::runtime_error("Failed to allocate readback slot"); } }
    bgfx::setViewName(60,"HoverCandidates"); bgfx::setViewName(61,"HoverReduce"); bgfx::setViewName(62,"HoverFinal"); bgfx::setViewName(63,"HoverReadback");
    r.metadata["gpu_extra_bytes"]=r.index.order.size()*4+r.index.clusters.size()*(48+16)+r.groups.size()*9*16+size_t(r.partialCount)*16+9*16;
    r.gpu=true;
}
inline void updateGroups(Runtime& r,const woby::UiState& ui,const Query& q) {
    for (auto& g:r.groups) { g.active=false; }
    woby::scenePickParts(ui,r.parts);
    for (const auto& p:r.parts) {
        if (!p.vertices || p.opacity<=0.000001f) { continue; }
        const auto found=r.groupAtOffset.find(p.indexOffset);
        if (found==r.groupAtOffset.end()) { continue; }
        auto& g=r.groups[found->second];
        if (g.active) { throw std::runtime_error("Repeated group instance is outside experiment scope"); }
        g.model=p.model; g.mvp=multiply(q.projection,multiply(q.view,p.model));
        g.radius=std::max(p.pointSize*.5f,3.0f); g.active=true;
    }
}
inline void submit(Runtime& r,const woby::GpuMesh& mesh,const Query& q,bool cull,Request& request,bool deferred,bool readback) {
    std::vector<Vec4> groupData; groupData.reserve(r.groups.size()*9);
    for (const auto& g:r.groups) {
        for (const auto* m:{&g.model,&g.mvp}) { for (size_t k=0;k<4;++k) { groupData.push_back({(*m)[k*4],(*m)[k*4+1],(*m)[k*4+2],(*m)[k*4+3]}); } }
        groupData.push_back({g.radius,g.active ? 1.0f:0.0f,0,0});
    }
    bgfx::update(r.groupBuffer,0,bgfx::copy(groupData.data(),bytes(groupData.size()*16)));
    const Vec4 cursor={q.x,q.y,static_cast<float>(q.width),static_cast<float>(q.height)};
    const Vec4 dispatch={static_cast<float>(r.index.clusters.size()),cull ? 1.0f:0.0f,q.homogeneous ? 1.0f:0.0f,0};
    bgfx::setUniform(r.view,q.view.data()); bgfx::setUniform(r.projection,q.projection.data());
    bgfx::setUniform(r.query,cursor.data()); bgfx::setUniform(r.dispatch,dispatch.data());
    bgfx::setBuffer(0,mesh.vertexBuffer,bgfx::Access::Read); bgfx::setBuffer(1,mesh.pointIdBuffer,bgfx::Access::Read);
    bgfx::setBuffer(2,r.order,bgfx::Access::Read); bgfx::setBuffer(3,r.clusters,bgfx::Access::Read);
    bgfx::setBuffer(4,r.groupBuffer,bgfx::Access::Read); bgfx::setBuffer(5,r.winners,bgfx::Access::Write);
    const uint32_t n=bytes(r.index.clusters.size());
    bgfx::dispatch(60,r.candidates,std::min(n,32768u),(n+32767u)/32768u);
    const Vec4 reduce1={static_cast<float>(n),0,0,0};
    bgfx::setUniform(r.reduceParams,reduce1.data());
    bgfx::setBuffer(0,r.winners,bgfx::Access::Read); bgfx::setBuffer(1,r.partials,bgfx::Access::Write);
    bgfx::setImage(2,r.result,0,bgfx::Access::Write,bgfx::TextureFormat::RGBA32U);
    bgfx::dispatch(61,r.reduce,r.partialCount);
    const Vec4 reduce2={static_cast<float>(r.partialCount),1,0,0};
    bgfx::setUniform(r.reduceParams,reduce2.data());
    bgfx::setBuffer(0,r.partials,bgfx::Access::Read); bgfx::setBuffer(1,r.winners,bgfx::Access::Write);
    bgfx::setImage(2,r.result,0,bgfx::Access::Write,bgfx::TextureFormat::RGBA32U);
    bgfx::dispatch(62,r.reduce,1);
    if (readback) {
        bgfx::blit(63,request.staging,0,0,r.result);
        request.pending=true; request.readRequested=!deferred;
        if (!deferred) { request.ready=bgfx::readTexture(request.staging,request.data.data()); }
    }
}
inline std::optional<woby::HoveredVertex> overlayHit(const Hit& hit,const Runtime& r,
    const woby::Mesh& mesh,const woby::GpuMesh& gpu) {
    if (hit.rank==invalid) { return {}; }
    const auto& p=mesh.vertices[gpu.pointVertexIndices[hit.rank]].position;
    const auto w=transform(r.groups[hit.group].model,{p[0],p[1],p[2],1});
    return woby::HoveredVertex{p,{w[0]/w[3],w[1]/w[3],w[2]/w[3]},hit.depth,hit.distance};
}
inline std::optional<woby::HoveredVertex> update(Runtime& r,const woby::UiState& ui,
    const std::vector<woby::LoadedModelRuntime>& runtimes,const woby::ScenePickView& view,
    woby::MousePosition mouse,const render_fps::Capture& capture,const std::filesystem::path& assets) {
    const auto mode=render_fps::current(capture).value("picker",std::string("none"));
    if (mode=="none") { return {}; }
    const auto start=Clock::now();
    if (!r.built) { build(r,ui,runtimes); }
    Query q{view.view,view.projection,mouse.x,mouse.y,view.width,view.height,view.homogeneousDepth};
    updateGroups(r,ui,q);
    const auto& mesh=ui.files[0].mesh; const auto& gpu=runtimes[0].gpuMesh;
    Counters counters; Hit hit;
    const bool validation=capture.config.value("validate",false);
    auto& event=r.events.emplace_back(render_fps::Json{{"scenario",capture.scenario},{"measured",capture.measuring},{"frame",r.frame},
        {"picker",mode},{"x",q.x},{"y",q.y}});
    if (mode=="legacy") {
        r.last=woby::findHoveredVertex(ui.files,ui.sceneNodes,runtimes,mouse,ui.masterVertexPointSize,view.view.data(),view.projection.data(),view.width,view.height,view.homogeneousDepth);
    } else if (mode=="cpu") {
        hit=queryIndex(r.index,mesh.vertices,gpu.pointVertexIndices,r.groups,q,counters);
        r.last=overlayHit(hit,r,mesh,gpu);
        event["rank"]=hit.rank;
        event["query_ms"]=ms(start,Clock::now());
        if (validation) {
            const auto ref=queryLinear(mesh.vertices,gpu.pointVertexIndices,r.ranges,r.groups,q);
            event["reference_rank"]=ref.rank;
            if (hit.rank!=ref.rank) { throw std::runtime_error("CPU hierarchy disagrees with linear query"); }
            const auto legacy=woby::findHoveredVertex(ui.files,ui.sceneNodes,runtimes,mouse,ui.masterVertexPointSize,view.view.data(),view.projection.data(),view.width,view.height,view.homogeneousDepth);
            if (bool(legacy)!=bool(r.last) || (legacy && (legacy->localPosition!=r.last->localPosition || legacy->depth!=r.last->depth || legacy->distanceSquared!=r.last->distanceSquared))) {
                throw std::runtime_error("CPU hierarchy disagrees with production picker");
            }
        }
    } else {
        if (!r.gpu) { setupGpu(r,assets); }
        auto slot=std::find_if(r.requests.begin(),r.requests.end(),[](const auto& s){return !s.pending;});
        if (slot==r.requests.end()) { ++r.skipped; }
        else {
            slot->scenario=capture.scenario; slot->measured=capture.measuring; slot->frame=r.frame;
            slot->validate=validation;
            if (validation) { slot->expected=queryIndex(r.index,mesh.vertices,gpu.pointVertexIndices,r.groups,q,counters); }
            slot->issued=Clock::now();
            submit(r,gpu,q,mode!="gpu_flat",*slot,mode!="gpu_immediate",mode!="gpu_resident");
        }
        // Results are logged asynchronously. Do not display a result from an older camera/cursor.
        r.last.reset();
    }
    event["cpu_ms"]=ms(start,Clock::now()); event["nodes"]=counters.nodes; event["leaves"]=counters.leaves; event["points"]=counters.points;
    return r.last;
}
inline void poll(Runtime& r,uint32_t frame,const render_fps::Capture& capture) {
    r.frame=frame;
    const auto now=Clock::now();
    for (auto& s:r.requests) {
        if (!s.pending) { continue; }
        if (!s.readRequested) {
            // D3D11 bgfx readTexture maps synchronously on the render thread.
            // Let the older copy finish before enqueueing that map command.
            if (frame>=s.frame+3u) {
                s.ready=bgfx::readTexture(s.staging,s.data.data()); s.readRequested=true;
            }
            continue;
        }
        if (frame<s.ready) { continue; }
        s.pending=false;
        auto event=render_fps::Json{{"scenario",s.scenario},{"measured",s.measured},{"frame",s.frame},{"completed_frame",frame},
            {"latency_ms",ms(s.issued,now)},{"latency_frames",frame-s.frame},{"rank",s.data[2]},
            {"group",s.data[3]},{"depth",std::bit_cast<float>(s.data[0])},{"distance",std::bit_cast<float>(s.data[1])}};
        if (s.validate) { event["expected_rank"]=s.expected.rank; event["expected_depth"]=s.expected.depth; event["expected_distance"]=s.expected.distance; }
        r.completions.push_back(std::move(event));
    }
    if (capture.done) { return; }
    const auto* stats=bgfx::getStats();
    render_fps::Json event={{"scenario",capture.scenario},{"measured",capture.measuring},{"frame",frame},{"gpu_frame",stats->gpuFrameNum},{"picker", "gpu_times"}};
    for (uint16_t i=0;i<stats->numViews;++i) {
        const auto& v=stats->viewStats[i];
        if (v.view>=60 && v.view<=63) {
            event["view_"+std::to_string(v.view)]=1000.0*static_cast<double>(v.gpuTimeEnd-v.gpuTimeBegin)/static_cast<double>(stats->gpuTimerFreq);
            event["view_frame_"+std::to_string(v.view)]=v.gpuFrameNum;
        }
    }
    r.events.push_back(std::move(event));
}
inline void finish(Runtime& r,const render_fps::Capture& capture) {
    for (int i=0;i<8;++i) { poll(r,bgfx::frame(),capture); }
    r.metadata["skipped_requests"]=r.skipped;
    std::ofstream(capture.output.string()+".hover.json") << render_fps::Json{{"metadata",r.metadata},{"events",r.events},{"completions",r.completions}}.dump();
    if (!r.gpu) { return; }
    bgfx::destroy(r.order); bgfx::destroy(r.clusters); bgfx::destroy(r.groupBuffer); bgfx::destroy(r.winners); bgfx::destroy(r.partials);
    bgfx::destroy(r.candidates); bgfx::destroy(r.reduce); bgfx::destroy(r.result);
    for (const auto u:{r.view,r.projection,r.query,r.dispatch,r.reduceParams}) { bgfx::destroy(u); }
    for (const auto& s:r.requests) { bgfx::destroy(s.staging); }
}
} // namespace hover_experiment
