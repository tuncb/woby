#pragma once
#include "marker_logic.h"
#include "marker_hooks.h"
#include "render_fps_probe.h"
#include "bgfx_helpers.h"
#include "hover_pick.h"
#include "scene_renderer.h"
#include "scene_viewport.h"
#include <chrono>
#include <fstream>
#include <stdexcept>

namespace marker_experiment {
using Clock=std::chrono::steady_clock;
using Json=render_fps::Json;
inline double milliseconds(Clock::time_point a,Clock::time_point b) { return std::chrono::duration<double,std::milli>(b-a).count(); }
struct Request {
    bool pending=false, requested=false, measured=false, audit=false;
    uint32_t frame=0, ready=0, delay=3;
    size_t scenario=0;
    float x=0,y=0;
    int width=0,height=0,samples=4;
    Clock::time_point issued;
    Pixel data{};
    std::array<Pixel,196> pixels{};
    std::vector<Draw> draws;
    bgfx::TextureHandle staging=BGFX_INVALID_HANDLE, auditStaging=BGFX_INVALID_HANDLE;
};
struct Runtime {
    bool initialized=false, active=false, routed=false, audit=false, compact=true;
    uint32_t frame=0, latest=0;
    size_t scenario=SIZE_MAX;
    uint16_t width=0,height=0;
    int samples=4;
    std::string mode;
    std::array<float,4> query{},options{};
    bgfx::TextureHandle color=BGFX_INVALID_HANDLE,ids=BGFX_INVALID_HANDLE,depth=BGFX_INVALID_HANDLE,
        result=BGFX_INVALID_HANDLE,auditTexture=BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle framebuffer=BGFX_INVALID_HANDLE,colorFramebuffer=BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle point=BGFX_INVALID_HANDLE,mesh=BGFX_INVALID_HANDLE,line=BGFX_INVALID_HANDLE,
        composite=BGFX_INVALID_HANDLE;
    std::array<bgfx::ProgramHandle,2> lookup={{{bgfx::kInvalidHandle},{bgfx::kInvalidHandle}}},
        highlight={{{bgfx::kInvalidHandle},{bgfx::kInvalidHandle}}};
    bgfx::UniformHandle tag=BGFX_INVALID_HANDLE,queryUniform=BGFX_INVALID_HANDLE,optionsUniform=BGFX_INVALID_HANDLE,
        colorSampler=BGFX_INVALID_HANDLE,idSampler=BGFX_INVALID_HANDLE,resultSampler=BGFX_INVALID_HANDLE;
    std::vector<Draw> draws;
    std::array<Request,8> requests;
    std::vector<Json> events,completions;
    Json metadata;
    uint64_t skipped=0,checked=0;
    std::optional<woby::HoveredVertex> last;
};
inline bool isMode(const std::string& mode) { return mode.starts_with("id_") || mode=="routed"; }
inline bgfx::ProgramHandle program(const std::filesystem::path& root,const char* vs,const char* fs) {
    return bgfx::createProgram(woby::loadShader(root/(std::string(vs)+".bin")),woby::loadShader(root/(std::string(fs)+".bin")),true);
}
inline void initialize(Runtime& r,const std::filesystem::path& assets) {
    const auto start=Clock::now();
    const auto* caps=bgfx::getCaps();
    const uint64_t required=BGFX_CAPS_COMPUTE|BGFX_CAPS_TEXTURE_READ_BACK|BGFX_CAPS_TEXTURE_BLIT|BGFX_CAPS_BLEND_INDEPENDENT;
    if ((caps->supported&required)!=required || caps->limits.maxFBAttachments<2 || caps->originBottomLeft) {
        throw std::runtime_error("Marker experiment requires compute, MRT, independent blend, readback, and top-left texture origin");
    }
    const auto root=assets/"shaders"/woby::rendererShaderFolder(bgfx::getRendererType());
    r.point=program(root,"vs_marker_point",r.compact ? "fs_marker_point_compact":"fs_marker_point");
    r.mesh=program(root,"vs_mesh","fs_marker_mesh");
    r.line=program(root,"vs_color","fs_marker_line");
    r.composite=program(root,"vs_marker_screen","fs_marker_composite");
    for (int i=0;i<2;++i) {
        const auto suffix=std::string(i==0 ? "single":"msaa")+(r.compact ? "_compact":"");
        r.lookup[i]=bgfx::createProgram(woby::loadShader(root/(std::string("cs_marker_lookup_")+suffix+".bin")),true);
        r.highlight[i]=program(root,"vs_marker_highlight",(std::string("fs_marker_highlight_")+suffix).c_str());
    }
    r.tag=bgfx::createUniform("u_markerTag",bgfx::UniformType::Vec4);
    r.queryUniform=bgfx::createUniform("u_markerQuery",bgfx::UniformType::Vec4);
    r.optionsUniform=bgfx::createUniform("u_markerOptions",bgfx::UniformType::Vec4);
    r.colorSampler=bgfx::createUniform("s_markerColor",bgfx::UniformType::Sampler);
    r.idSampler=bgfx::createUniform("s_markerIds",bgfx::UniformType::Sampler);
    r.resultSampler=bgfx::createUniform("s_markerResult",bgfx::UniformType::Sampler);
    r.result=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_COMPUTE_WRITE|BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP);
    r.auditTexture=bgfx::createTexture2D(196,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_COMPUTE_WRITE);
    for (auto& s:r.requests) {
        s.staging=bgfx::createTexture2D(1,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST);
        if (r.audit) { s.auditStaging=bgfx::createTexture2D(196,1,false,1,bgfx::TextureFormat::RGBA32F,BGFX_TEXTURE_READ_BACK|BGFX_TEXTURE_BLIT_DST); }
    }
    for (auto h:{r.point,r.mesh,r.line,r.composite,r.lookup[0],r.lookup[1],r.highlight[0],r.highlight[1]}) {
        if (!bgfx::isValid(h)) { throw std::runtime_error("Marker shader allocation failed"); }
    }
    for (auto h:{r.result,r.auditTexture}) { if (!bgfx::isValid(h)) { throw std::runtime_error("Marker result allocation failed"); } }
    for (const auto& s:r.requests) {
        if (!bgfx::isValid(s.staging) || (r.audit && !bgfx::isValid(s.auditStaging))) { throw std::runtime_error("Marker readback allocation failed"); }
    }
    r.metadata={{"shader_setup_ms",milliseconds(start,Clock::now())},{"spatial_index_bytes",0},{"spatial_index_build_ms",0},
        {"compact_ids",r.compact},{"rgba32f_caps",caps->formats[bgfx::TextureFormat::RGBA32F]}};
    r.initialized=true;
}
inline void destroyTargets(Runtime& r) {
    for (auto h:{r.framebuffer,r.colorFramebuffer}) { if (bgfx::isValid(h)) { bgfx::destroy(h); } }
    for (auto h:{r.color,r.ids,r.depth}) { if (bgfx::isValid(h)) { bgfx::destroy(h); } }
    r.framebuffer=BGFX_INVALID_HANDLE; r.colorFramebuffer=BGFX_INVALID_HANDLE;
    r.color=BGFX_INVALID_HANDLE; r.ids=BGFX_INVALID_HANDLE; r.depth=BGFX_INVALID_HANDLE;
}
inline void targets(Runtime& r,uint16_t width,uint16_t height,int samples) {
    if (r.width==width && r.height==height && r.samples==samples && bgfx::isValid(r.framebuffer)) { return; }
    const auto start=Clock::now(); destroyTargets(r);
    r.width=width; r.height=height; r.samples=samples;
    const uint64_t msaa=samples==4 ? BGFX_TEXTURE_RT_MSAA_X4:BGFX_TEXTURE_RT;
    const uint64_t idFlags=msaa|(samples==4 ? BGFX_TEXTURE_MSAA_SAMPLE:0u)|BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP;
    const auto idFormat=r.compact ? bgfx::TextureFormat::RGBA8:bgfx::TextureFormat::RGBA32F;
    if (!bgfx::isTextureValid(0,false,1,idFormat,idFlags)) { throw std::runtime_error("MSAA ID target unsupported"); }
    r.color=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::BGRA8,msaa|BGFX_SAMPLER_POINT|BGFX_SAMPLER_UVW_CLAMP);
    r.ids=bgfx::createTexture2D(width,height,false,1,idFormat,idFlags);
    r.depth=bgfx::createTexture2D(width,height,false,1,bgfx::TextureFormat::D24S8,msaa|BGFX_TEXTURE_RT_WRITE_ONLY);
    const std::array<bgfx::TextureHandle,3> mrt={r.color,r.ids,r.depth};
    const std::array<bgfx::TextureHandle,2> plain={r.color,r.depth};
    r.framebuffer=bgfx::createFrameBuffer(3,mrt.data(),false);
    r.colorFramebuffer=bgfx::createFrameBuffer(2,plain.data(),false);
    if (!bgfx::isValid(r.framebuffer)||!bgfx::isValid(r.colorFramebuffer)) { throw std::runtime_error("Marker framebuffer allocation failed"); }
    bgfx::setPaletteColor(14,0x20242affu); bgfx::setPaletteColor(15,0x00000000u);
    r.metadata["target_allocations"].push_back({{"width",width},{"height",height},{"samples",samples},
        {"submit_ms",milliseconds(start,Clock::now())},{"id_payload_bytes",uint64_t(width)*height*(r.compact ? 4u:16u)*static_cast<uint64_t>(samples)},
        {"all_target_payload_bytes",uint64_t(width)*height*((r.compact ? 12u:24u)*static_cast<uint64_t>(samples)+(samples==4 ? 4u:0u))}});
}
inline void begin(Runtime& r,const woby::ScenePickView& view,woby::MousePosition mouse,
    const render_fps::Capture& capture,const std::filesystem::path& assets) {
    r.mode=render_fps::current(capture).value("picker",std::string("none"));
    r.active=r.mode.starts_with("id_"); r.routed=r.active||r.mode=="routed";
    if (capture.scenario!=r.scenario) { r.last.reset(); r.scenario=capture.scenario; }
    hooks.active=false;
    if (!r.routed) {
        bgfx::setViewFrameBuffer(1,BGFX_INVALID_HANDLE); bgfx::setViewClear(1,BGFX_CLEAR_NONE);
        bgfx::setViewOrder(0); return;
    }
    r.audit=capture.config.value("validate",false);
    r.compact=capture.config.value("compact_ids",true);
    if (!r.initialized) { initialize(r,assets); }
    targets(r,static_cast<uint16_t>(view.width),static_cast<uint16_t>(view.height),render_fps::current(capture).value("msaa",true) ? 4:1);
    bgfx::setViewFrameBuffer(1,r.active ? r.framebuffer:r.colorFramebuffer);
    bgfx::setViewRect(1,0,0,r.width,r.height);
    bgfx::setViewClear(1,BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH,1.0f,0,14,r.active ? 15:UINT8_MAX);
    // Keep numeric order: this pinned bgfx backend compares remapped blit keys
    // against original view IDs. Non-monotonic remapping copies the old result.
    bgfx::setViewOrder(0);
    bgfx::setViewName(2,"MarkerLookup"); bgfx::setViewName(3,"MarkerComposite");
    bgfx::setViewName(4,"MarkerHighlight"); bgfx::setViewName(6,"MarkerReadback");
    r.query={mouse.x,mouse.y,static_cast<float>(view.width),static_cast<float>(view.height)};
    r.draws.clear(); hooks={r.active,r.tag,&r.draws,0};
}
inline void submit(Runtime& r,const render_fps::Capture& capture,const woby::SceneViewport& viewport) {
    hooks.active=false;
    if (!r.routed) { return; }
    const auto start=Clock::now();
    r.options={static_cast<float>(r.samples),r.audit ? 1.0f:0.0f,hooks.largestPoint+5.0f,0};
    const int variant=r.samples==4 ? 1:0;
    if (r.active && r.mode!="id_buffer") {
        bgfx::setUniform(r.queryUniform,r.query.data()); bgfx::setUniform(r.optionsUniform,r.options.data());
        bgfx::setTexture(0,r.idSampler,r.ids);
        bgfx::setImage(1,r.result,0,bgfx::Access::Write,bgfx::TextureFormat::RGBA32F);
        bgfx::setImage(2,r.auditTexture,0,bgfx::Access::Write,bgfx::TextureFormat::RGBA32F);
        bgfx::dispatch(2,r.lookup[variant],1);
    }
    for (bgfx::ViewId id:std::array<bgfx::ViewId,2>{3,4}) {
        bgfx::setViewFrameBuffer(id,BGFX_INVALID_HANDLE);
        bgfx::setViewRect(id,static_cast<uint16_t>(viewport.x),static_cast<uint16_t>(viewport.y),r.width,r.height);
        bgfx::setViewTransform(id,nullptr,nullptr);
    }
    bgfx::setTexture(0,r.colorSampler,r.color); bgfx::setVertexCount(4);
    bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_PT_TRISTRIP);
    bgfx::submit(3,r.composite);
    if (r.active && r.mode!="id_buffer") {
        bgfx::setUniform(r.queryUniform,r.query.data()); bgfx::setUniform(r.optionsUniform,r.options.data());
        bgfx::setTexture(0,r.idSampler,r.ids); bgfx::setTexture(1,r.resultSampler,r.result); bgfx::setVertexCount(4);
        bgfx::setState(BGFX_STATE_WRITE_RGB|BGFX_STATE_WRITE_A|BGFX_STATE_PT_TRISTRIP|BGFX_STATE_BLEND_ALPHA);
        bgfx::submit(4,r.highlight[variant]);
    }
    if (r.mode=="id_async" || r.mode=="id_immediate" || r.audit) {
        auto slot=std::find_if(r.requests.begin(),r.requests.end(),[](const auto& s){return !s.pending;});
        if (slot==r.requests.end()) { ++r.skipped; }
        else {
            auto& s=*slot; s.pending=true; s.requested=false; s.measured=capture.measuring; s.audit=r.audit;
            s.frame=r.frame; s.scenario=capture.scenario; s.issued=Clock::now(); s.draws=r.draws;
            s.x=r.query[0]; s.y=r.query[1]; s.width=r.width; s.height=r.height; s.samples=r.samples;
            s.delay=r.mode=="id_immediate" ? 0u:3u;
            bgfx::blit(6,s.staging,0,0,r.result);
            if (s.audit) { bgfx::blit(6,s.auditStaging,0,0,r.auditTexture); }
            if (s.delay==0) { s.ready=bgfx::readTexture(s.staging,s.data.data()); s.requested=true; }
        }
    }
    r.events.push_back({{"kind","submit"},{"scenario",capture.scenario},{"measured",capture.measuring},{"frame",r.frame},
        {"cpu_ms",milliseconds(start,Clock::now())},{"draws",r.draws.size()},{"snapshot_bytes",r.draws.size()*sizeof(Draw)}});
}
inline void poll(Runtime& r,uint32_t frame,const render_fps::Capture& capture,const woby::UiState& ui,
    const std::vector<woby::LoadedModelRuntime>& runtimes) {
    r.frame=frame; const auto now=Clock::now();
    for (auto& s:r.requests) {
        if (!s.pending) { continue; }
        if (!s.requested) {
            if (frame>=s.frame+s.delay) {
                s.ready=bgfx::readTexture(s.staging,s.data.data());
                if (s.audit) { s.ready=std::max(s.ready,bgfx::readTexture(s.auditStaging,s.pixels.data())); }
                s.requested=true;
            }
            continue;
        }
        if (frame<s.ready) { continue; }
        s.pending=false;
        if (!validPixel(s.data)) { throw std::runtime_error("Marker readback contains an invalid encoded ID: "+Json(s.data).dump()); }
        const uint32_t id=identity(s.data);
        Json event={{"scenario",s.scenario},{"measured",s.measured},{"frame",s.frame},{"completed_frame",frame},
            {"latency_ms",milliseconds(s.issued,now)},{"latency_frames",frame-s.frame},{"id",id},{"draw",s.data[3]},{"depth",s.data[2]},
            {"x",s.x},{"y",s.y}};
        if (s.audit) {
            const auto expected=selectPixel(s.pixels,s.x,s.y,s.width,s.height,s.samples);
            if (expected!=s.data) {
                std::ofstream(capture.output.string()+".mismatch.json")<<Json{{"expected",expected},{"actual",s.data},{"pixels",s.pixels},{"x",s.x},{"y",s.y},{"width",s.width},{"height",s.height},{"samples",s.samples}}.dump(2);
                throw std::runtime_error("GPU marker lookup disagrees with independent CPU sample reduction: expected "+Json(expected).dump()+" actual "+Json(s.data).dump());
            }
            for (int i=0;i<49;++i) { for (int j=0;j<s.samples;++j) {
                const auto& p=s.pixels[static_cast<size_t>(i*4+j)];
                if (!validPixel(p)) { throw std::runtime_error("Blended or invalid sample ID"); }
                const auto sampleId=identity(p);
                const auto sampleDraw=r.compact ? drawForIdentity(s.draws,sampleId)
                    : (p[3]<static_cast<float>(s.draws.size()) ? std::optional<size_t>(static_cast<size_t>(p[3])):std::nullopt);
                if (sampleId && (!sampleDraw || sampleId-1<s.draws[*sampleDraw].begin
                    || sampleId-1-s.draws[*sampleDraw].begin>=s.draws[*sampleDraw].count)) {
                    throw std::runtime_error("Sample ID outside its source draw");
                }
            } }
            event["validated"]=true; event["samples"]=s.pixels; ++r.checked;
        }
        std::optional<woby::HoveredVertex> hit;
        if (id) {
            const auto draw=r.compact ? drawForIdentity(s.draws,id)
                : (s.data[3]<static_cast<float>(s.draws.size()) ? std::optional<size_t>(static_cast<size_t>(s.data[3])):std::nullopt);
            if (ui.files.size()!=1 || runtimes.size()!=1 || !draw) { throw std::runtime_error("Invalid marker draw identity"); }
            const auto& gpu=runtimes[0].gpuMesh; const uint32_t rank=id-1;
            const auto& d=s.draws[*draw]; event["draw"]=*draw;
            if (rank>=gpu.pointVertexIndices.size() || rank<d.begin || rank-d.begin>=d.count) { throw std::runtime_error("Marker identity out of range"); }
            const auto& local=ui.files[0].mesh.vertices[gpu.pointVertexIndices[rank]].position;
            std::array<float,3> world{};
            const float w=d.model[3]*local[0]+d.model[7]*local[1]+d.model[11]*local[2]+d.model[15];
            for (size_t k=0;k<3;++k) { world[k]=(d.model[k]*local[0]+d.model[4+k]*local[1]+d.model[8+k]*local[2]+d.model[12+k])/w; }
            hit=woby::HoveredVertex{local,world,s.data[2],0}; event["local"]=local; event["world"]=world;
        }
        if (acceptResult(s.data,s.frame,r.latest,s.scenario,capture.scenario,s.draws.size())) { r.last=hit; r.latest=s.frame; }
        r.completions.push_back(std::move(event));
    }
    if (capture.done) { return; }
    const auto* stats=bgfx::getStats();
    Json event={{"kind","gpu"},{"scenario",capture.scenario},{"measured",capture.measuring},{"frame",frame}};
    for (uint16_t i=0;i<stats->numViews;++i) {
        const auto& v=stats->viewStats[i];
        if ((v.view>=1 && v.view<=4) || v.view==6) {
            event["view_"+std::to_string(v.view)]=1000.0*static_cast<double>(v.gpuTimeEnd-v.gpuTimeBegin)/static_cast<double>(stats->gpuTimerFreq);
            event["view_frame_"+std::to_string(v.view)]=v.gpuFrameNum;
        }
    }
    r.events.push_back(std::move(event));
}
inline void finish(Runtime& r,const render_fps::Capture& capture,const woby::UiState& ui,
    const std::vector<woby::LoadedModelRuntime>& runtimes) {
    for (int i=0;i<9;++i) { poll(r,bgfx::frame(),capture,ui,runtimes); }
    r.metadata["skipped_requests"]=r.skipped; r.metadata["validated_requests"]=r.checked;
    std::ofstream(capture.output.string()+".marker.json")<<Json{{"metadata",r.metadata},{"events",r.events},{"completions",r.completions}}.dump();
    if (!r.initialized) { return; }
    destroyTargets(r);
    for (auto h:{r.point,r.mesh,r.line,r.composite,r.lookup[0],r.lookup[1],r.highlight[0],r.highlight[1]}) { bgfx::destroy(h); }
    for (auto h:{r.tag,r.queryUniform,r.optionsUniform,r.colorSampler,r.idSampler,r.resultSampler}) { bgfx::destroy(h); }
    bgfx::destroy(r.result); bgfx::destroy(r.auditTexture);
    for (const auto& s:r.requests) { bgfx::destroy(s.staging); if (bgfx::isValid(s.auditStaging)) { bgfx::destroy(s.auditStaging); } }
}
}
