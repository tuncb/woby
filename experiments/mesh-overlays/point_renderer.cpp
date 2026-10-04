#include "point_renderer.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace woby::points {
namespace {
using Clock=std::chrono::steady_clock;
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
double elapsed(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
void matrix(float4x4& out,const Matrix& in) { for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c) out.rows[r][c]=in[c*4+r]; }
std::vector<uint32_t> shader(const char* entry) {
    std::ifstream stream(std::filesystem::path(WOBY_OVERLAY_SHADER_DIR)/(std::string(entry)+".spv"),std::ios::binary|std::ios::ate);
    require(bool(stream),"Cannot open point shader"); const auto size=static_cast<size_t>(stream.tellg());
    require(size && size%4==0,"Invalid point shader"); std::vector<uint32_t> result(size/4); stream.seekg(0);
    stream.read(reinterpret_cast<char*>(result.data()),static_cast<std::streamsize>(size)); require(bool(stream),"Cannot read point shader"); return result;
}
gpu::ShaderStage stage(const std::vector<uint32_t>& code,const char* entry) { return {{reinterpret_cast<const gpu::byte*>(code.data()),code.size()*4},entry}; }
gpu::PSO* compute(gpu::Device* device,const char* entry) {
    const auto code=shader(entry); auto* pso=gpu::create_compute_pso(device,stage(code,entry)); require(pso!=nullptr,"Point compute pipeline failed"); return pso;
}
gpu::PSO* graphics(gpu::Device* device,const char* vs,const char* fs,uint32_t samples) {
    const auto vertex=shader(vs),fragment=shader(fs);
    const std::array<gpu::ColorTargetDesc,2> targets{{{.format=gpu::Format::rgba8_unorm},{.format=gpu::Format::rgba8_unorm}}};
    auto* pso=gpu::create_graphics_pso(device,{.vertex=stage(vertex,vs),.fragment=stage(fragment,fs),.color_targets=targets,
        .depth_format=gpu::Format::d32_float,.sample_count=samples}); require(pso!=nullptr,"Point graphics pipeline failed"); return pso;
}
void allocate(gpu::Device* device,gpu::GpuHeap& heap,uint64_t bytes,gpu::MemoryType type=gpu::MemoryType::gpu_only) {
    require(!heap.owner,"Point buffer already allocated"); heap=gpu::create_gpu_heap(device,std::max(uint64_t{16},(bytes+15)&~uint64_t{15}),type);
    require(heap.owner!=nullptr,heap.error);
}
void upload(overlay::Renderer& r,gpu::GpuHeap& heap,const void* data,uint64_t bytes) {
    if (!bytes) return;
    allocate(r.device,heap,bytes);
    // Bound staging and outstanding CPU upload ownership independently of source size.
    constexpr uint64_t chunk=16ull*1024*1024;
    for (uint64_t offset=0;offset<bytes;offset+=chunk) {
        const auto count=std::min(chunk,bytes-offset);
        r.uploads->upload_buffer({heap.range.gpu+offset,count},{static_cast<const gpu::byte*>(data)+offset,count}); r.uploads->wait();
    }
}
void barrier(gpu::CommandBuffer* commands) {
    gpu::barrier(commands,gpu::Stage::all_commands,gpu::Access::shader_write|gpu::Access::color_write|gpu::Access::depth_stencil_write|gpu::Access::transfer_write|gpu::Access::transfer_read,
        gpu::Stage::all_commands,gpu::Access::shader_read|gpu::Access::shader_write|gpu::Access::color_read|gpu::Access::color_write
            |gpu::Access::depth_stencil_read|gpu::Access::depth_stencil_write|gpu::Access::transfer_read|gpu::Access::index_read);
}
auto root(overlay::Renderer& r,const PointRoot& value) {
    auto allocation=r.arena->allocate<PointRoot>(); require(allocation.cpu!=nullptr,"Point root arena exhausted"); *allocation.cpu=value; return allocation;
}
void imageDispatch(overlay::Renderer& r,gpu::CommandBuffer* commands,gpu::PSO* pso,const PointRoot& value) {
    auto data=root(r,value); gpu::bind_pso(commands,pso); gpu::dispatch(commands,data.gpu,{(value.width+7)/8,(value.height+7)/8,1});
}
} // namespace
PointRenderer::~PointRenderer() {
    if (!gpu.device) return;
    gpu::wait_idle(gpu.device);
    for (auto* pso:{clear,raster,resolve,surface,capture}) gpu::destroy_pso(pso);
    for (auto& heap:points) gpu::destroy_gpu_heap(heap);
    for (auto& heap:proxies) gpu::destroy_gpu_heap(heap);
    for (auto* heap:{&winners,&tasks,&groups}) gpu::destroy_gpu_heap(*heap);
}
void initialize(PointRenderer& r,const Cloud& cloud,const Mesh& mesh,overlay::Options options,uint32_t chunkPoints) {
    require(chunkPoints>0 && chunkPoints<=1u<<24,"Invalid compact allocation size"); r.chunkPoints=chunkPoints;
    options.ids=true; overlay::initialize(r.gpu,options); auto& g=r.gpu;
    require(gpu::get_device_caps(g.device).point_buffer_int64_atomics,"Point prototype requires Vulkan 64-bit buffer atomics");
    r.clear=compute(g.device,"cs_point_clear"); r.raster=compute(g.device,"cs_point_raster"); r.capture=compute(g.device,"cs_point_ids");
    r.resolve=graphics(g.device,"vs_point_resolve","fs_point_resolve",options.samples);
    r.surface=graphics(g.device,"vs_mesh","fs_marker_mesh",options.samples);
    for (const bool proxies:{false,true}) {
        const auto& input=proxies?cloud.proxies:cloud.points; auto& buffers=proxies?r.proxies:r.points;
        for (size_t i=0;i<input.size();i+=chunkPoints) {
            const auto count=std::min<size_t>(chunkPoints,input.size()-i); buffers.emplace_back();
            upload(g,buffers.back(),input.data()+i,count*sizeof(Point)); r.geometryBytes+=count*sizeof(Point);
        }
    }
    if (!mesh.indices.empty()) {
        upload(g,g.scene.vertices,mesh.vertices.data(),mesh.vertices.size()*sizeof(Vertex));
        upload(g,g.scene.triangles,mesh.indices.data(),mesh.indices.size()*4);
        r.geometryBytes+=mesh.vertices.size()*sizeof(Vertex)+mesh.indices.size()*4;
    }
    for (size_t i=0;i<mesh.nodes.size();++i) {
        overlay::Group group; group.range.triangleIndexOffset=mesh.nodes[i].indexOffset; group.range.triangleIndexCount=mesh.nodes[i].indexCount;
        group.color={.22f+float((i*37)%53)/100,.28f+float((i*19)%47)/100,.3f+float((i*31)%43)/100,1}; g.scene.groups.push_back(group);
    }
    r.visibilityBytes=uint64_t(options.width)*options.height*options.samples*8;
    allocate(g.device,r.winners,r.visibilityBytes);
    allocate(g.device,r.groups,cloud.groups.size()*sizeof(CloudGroup),gpu::MemoryType::cpu_visible);
}
Timing render(PointRenderer& r,const Cloud& cloud,const Selection& selection,const Matrix& projection,float size,bool solid,bool reset,overlay::Capture* capture) {
    auto& g=r.gpu; overlay::validate(g.options,{.pointSize=size});
    const auto start=Clock::now();
    require(cloud.groups.size()==g.scene.groups.size(),"Point scene groups changed without an upload");
    const bool groupsChanged=!r.valid || cloud.groups!=r.previousGroups;
    const bool matricesChanged=groupsChanged || projection!=r.previousProjection;
    reset=reset || matricesChanged || size!=r.previousSize || solid!=r.previousSolid;
    if (groupsChanged || selection.ranges!=r.previousRanges) {
        r.taskList.clear(); r.submitted=0;
        for (const auto range:selection.ranges) {
            const auto& buffers=range.proxy?r.proxies:r.points; const auto count=range.proxy?cloud.proxies.size():cloud.points.size();
            require(range.group<cloud.groups.size() && uint64_t(range.begin)+range.count<=count,"Invalid point selection");
            if (!cloud.groups[range.group].enabled) continue;
            for (uint32_t used=0;used<range.count;) {
                const uint32_t index=range.begin+used,chunk=index/r.chunkPoints,offset=index%r.chunkPoints;
                const uint32_t n=std::min({range.count-used,r.chunkPoints-offset,4096u});
                r.taskList.push_back({reinterpret_cast<CloudPoint*>(buffers[chunk].range.gpu)+offset,n,range.group}); used+=n; r.submitted+=n;
            }
        }
        require(r.taskList.size()<=std::numeric_limits<uint32_t>::max(),"Too many point tasks");
        if (r.taskList.size()>r.taskCapacity) {
            gpu::destroy_gpu_heap(r.tasks); r.tasks={}; r.taskCapacity=static_cast<uint32_t>(r.taskList.size());
            allocate(g.device,r.tasks,r.taskList.size()*sizeof(CloudTask),gpu::MemoryType::cpu_visible);
        }
        if (!r.taskList.empty()) std::memcpy(r.tasks.range.cpu,r.taskList.data(),r.taskList.size()*sizeof(CloudTask));
        r.previousRanges=selection.ranges;
    }
    if (matricesChanged) for (size_t i=0;i<cloud.groups.size();++i) {
        const auto& group=cloud.groups[i]; CloudGroup value{}; Matrix m; bx::mtxMul(m.data(),group.model.data(),projection.data()); matrix(value.matrix,m);
        value.color={group.color[0],group.color[1],group.color[2],1}; value.firstId=group.firstId; value.endId=group.endId; value.enabled=group.enabled?1u:0u;
        std::memcpy(r.groups.range.cpu+i*sizeof(CloudGroup),&value,sizeof(value));
    }
    r.previousProjection=projection; r.previousSize=size; r.previousSolid=solid;
    if (groupsChanged) r.previousGroups=cloud.groups;
    r.valid=true;
    gpu::reset_command_pool(g.pool); g.arena->reset(); std::array<uint64_t,4> timestamps{};
    auto* commands=gpu::begin_commands(g.pool); gpu::set_texture_descriptor_heap(commands,g.descriptors); barrier(commands);
    gpu::write_timestamp(commands,&timestamps[0]);
    PointRoot value{}; value.winners=reinterpret_cast<uint64*>(r.winners.range.gpu); value.tasks=reinterpret_cast<CloudTask*>(r.tasks.range.gpu);
    value.groups=reinterpret_cast<CloudGroup*>(r.groups.range.gpu); value.width=g.options.width; value.height=g.options.height;
    value.samples=g.options.samples; value.depthTexture=1; value.readDepth=solid?1u:0u; value.pointSize=size;
    value.groupCount=static_cast<uint32_t>(cloud.groups.size()); value.taskCount=static_cast<uint32_t>(r.taskList.size()); value.dispatchWidth=std::min(value.taskCount,1024u);
    std::array<gpu::ColorAttachment,2> colors{{{.render_view=g.color.view,.load=gpu::LoadOp::clear,.clear={.125f,.141f,.165f,1}},
        {.render_view=g.ids.view,.load=gpu::LoadOp::clear,.clear={0,0,0,0}}}};
    if (reset) {
        gpu::begin_render_pass(commands,{.colors=colors,.depth={.render_view=g.depth.view,.load=gpu::LoadOp::clear,.clear=0}});
        if (solid) for (size_t i=0;i<g.scene.groups.size();++i) {
            const auto& group=g.scene.groups[i]; if (!group.range.triangleIndexCount || !cloud.groups[i].enabled) continue;
            auto data=g.arena->allocate<WobyRoot>(); require(data.cpu!=nullptr,"Surface root arena exhausted"); *data.cpu={};
            Matrix m; bx::mtxMul(m.data(),cloud.groups[i].model.data(),projection.data()); matrix(data.cpu->modelViewProj,m); matrix(data.cpu->model,cloud.groups[i].model);
            data.cpu->vertices=reinterpret_cast<float*>(g.scene.vertices.range.gpu); data.cpu->stride=sizeof(Vertex)/4;
            data.cpu->color={group.color[0],group.color[1],group.color[2],1};
            gpu::set_depth_stencil(commands,{.depth_test=true,.depth_write=true,.depth_compare=gpu::CompareOp::greater}); gpu::bind_pso(commands,r.surface);
            gpu::draw_indexed(commands,data.gpu,{g.scene.triangles.range.gpu+uint64_t(group.range.triangleIndexOffset)*4,uint64_t(group.range.triangleIndexCount)*4},gpu::IndexType::uint32,group.range.triangleIndexCount);
        }
        gpu::end_render_pass(commands); barrier(commands); imageDispatch(g,commands,r.clear,value); barrier(commands);
    }
    gpu::write_timestamp(commands,&timestamps[1]);
    if (value.taskCount) {
        auto data=root(g,value); gpu::bind_pso(commands,r.raster);
        gpu::dispatch(commands,data.gpu,{value.dispatchWidth,(value.taskCount+value.dispatchWidth-1)/value.dispatchWidth,1});
    }
    barrier(commands); gpu::write_timestamp(commands,&timestamps[2]);
    colors[0].load=colors[1].load=gpu::LoadOp::load; colors[0].resolve_view=g.resolved.view;
    gpu::begin_render_pass(commands,{.colors=colors,.depth={.render_view=g.depth.view,.load=gpu::LoadOp::load}});
    gpu::set_depth_stencil(commands,{.depth_test=false,.depth_write=true}); gpu::bind_pso(commands,r.resolve);
    const auto data=root(g,value); gpu::draw(commands,data.gpu,3); gpu::end_render_pass(commands); gpu::write_timestamp(commands,&timestamps[3]);
    if (capture) {
        barrier(commands); const uint64_t bytes=uint64_t(value.width)*value.height*4;
        gpu::copy_texture_to_memory(commands,g.resolved.allocation.texture?g.resolved.allocation.texture:g.color.allocation.texture,{g.readback.range.gpu,bytes});
        value.ids=reinterpret_cast<uint32_t*>(g.captureIds.range.gpu); imageDispatch(g,commands,r.capture,value); barrier(commands);
        gpu::copy_memory(commands,{g.captureIds.range.gpu,bytes*value.samples},{g.readback.range.gpu+bytes,bytes*value.samples});
        gpu::barrier(commands,gpu::Stage::transfer,gpu::Access::transfer_write,gpu::Stage::host,gpu::Access::host_read);
    }
    gpu::end_commands(commands); const std::array list{commands}; gpu::submit(g.device,{.commands=list,.completion={g.timeline,++g.sequence}});
    Timing timing; timing.cpuMs=elapsed(start); timing.submitted=r.submitted; timing.tasks=value.taskCount; timing.reset=reset;
    gpu::wait_timeline({g.timeline,g.sequence}); timing.wallMs=elapsed(start); gpu::read_timestamps(g.pool);
    const double period=gpu::get_device_caps(g.device).timestamp_period_ns/1e6;
    timing.gpuMs=double(timestamps[3]-timestamps[0])*period; timing.clearMs=double(timestamps[1]-timestamps[0])*period;
    timing.rasterMs=double(timestamps[2]-timestamps[1])*period; timing.resolveMs=double(timestamps[3]-timestamps[2])*period;
    if (capture) {
        capture->width=value.width; capture->height=value.height; const size_t bytes=size_t(value.width)*value.height*4;
        capture->rgba.resize(bytes); std::memcpy(capture->rgba.data(),g.readback.range.cpu,bytes);
        capture->sampleIds.resize(bytes/4*value.samples); std::memcpy(capture->sampleIds.data(),g.readback.range.cpu+bytes,bytes*value.samples);
        capture->ids.resize(bytes/4);
        for (size_t i=0;i<bytes/4;++i) {
            capture->ids[i]=0; for (uint32_t sample=0;sample<value.samples;++sample) if (const auto id=capture->sampleIds[i*value.samples+sample]) { capture->ids[i]=id; break; }
        }
    }
    return timing;
}
} // namespace woby::points
