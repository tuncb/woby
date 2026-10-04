#include "renderer.h"
#include "scene_buffer_size.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace woby::overlay {
namespace {
using Clock = std::chrono::steady_clock;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
double milliseconds(Clock::duration elapsed) { return std::chrono::duration<double,std::milli>(elapsed).count(); }
void shaderMatrix(float4x4& destination,const float* columnMajor) {
    // Match graphics.cpp: bx matrices are column-major, WobyRoot is row-major.
    for (size_t row=0;row<4;++row)
        for (size_t column=0;column<4;++column) destination.rows[row][column]=columnMajor[column*4+row];
}
std::vector<uint32_t> shader(const std::string& entry) {
    const auto path = std::filesystem::path(WOBY_OVERLAY_SHADER_DIR)/(entry+".spv");
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    require(bool(file),"Cannot open overlay shader");
    const auto bytes = static_cast<size_t>(file.tellg());
    require(bytes && bytes%4==0,"Invalid overlay shader");
    std::vector<uint32_t> code(bytes/4);
    file.seekg(0); file.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(bytes));
    require(bool(file),"Cannot read overlay shader");
    return code;
}
gpu::ShaderStage stage(const std::vector<uint32_t>& code,const char* entry) {
    return {.code={reinterpret_cast<const gpu::byte*>(code.data()),code.size()*4},.entry_point=entry};
}
gpu::PSO* pipeline(Renderer& r,const std::string& kind,bool blend) {
    const auto key=std::make_pair(kind,blend);
    if (const auto found=r.pipelines.find(key);found!=r.pipelines.end()) return found->second;
    std::string vs="vs_mesh",fs=r.options.ids?"fs_marker_mesh":"fs_mesh";
    auto topology=gpu::PrimitiveTopology::triangles;
    if (kind=="point") { vs="vs_point_sprite"; fs=r.options.ids?"fs_marker_point":"fs_point_sprite"; topology=gpu::PrimitiveTopology::triangle_strip; }
    if (kind=="line") { vs="vs_color"; fs=r.options.ids?"fs_marker_line":"fs_color"; topology=gpu::PrimitiveTopology::lines; }
    if (kind=="barycentric") fs=r.options.ids?"fs_overlay_native_ids":"fs_overlay_native";
    if (kind=="pulled") { vs="vs_overlay_pull"; fs=r.options.ids?"fs_overlay_pull_ids":"fs_overlay_pull"; }
    if (kind=="depth") { vs="vs_color"; fs.clear(); }
    const auto vertex=shader(vs),fragment=fs.empty()?std::vector<uint32_t>{}:shader(fs);
    std::array<gpu::ColorTargetDesc,2> targets{};
    targets[0].format=targets[1].format=gpu::Format::rgba8_unorm;
    if (blend) targets[0].blend={.enabled=true,
        .color={gpu::BlendFactor::source_alpha,gpu::BlendFactor::one_minus_source_alpha},
        .alpha={gpu::BlendFactor::one,gpu::BlendFactor::one_minus_source_alpha}};
    if (kind=="depth") targets[0].write_mask=targets[1].write_mask=0;
    auto* result=gpu::create_graphics_pso(r.device,{.vertex=stage(vertex,vs.c_str()),.fragment=fs.empty()?gpu::ShaderStage{}:stage(fragment,fs.c_str()),
        .color_targets={targets.data(),r.options.ids?2u:1u},.depth_format=gpu::Format::d32_float,
        .sample_count=r.options.samples,.topology=topology});
    require(result!=nullptr,"Overlay graphics pipeline creation failed");
    try { r.pipelines.emplace(key,result); } catch (...) { gpu::destroy_pso(result); throw; }
    return result;
}
void release(Renderer& r,Image& image) {
    if (!image.allocation.texture) return;
    gpu::destroy_render_view(image.view); r.textures->free(image.allocation); image={};
}
Image image(Renderer& r,gpu::CommandBuffer* commands,gpu::Format format,uint32_t samples,bool sampled=false) {
    auto usage=format==gpu::Format::d32_float?gpu::TextureUsage::depth_stencil_attachment
        :gpu::TextureUsage::color_attachment|gpu::TextureUsage::transfer_source;
    if (sampled) usage=usage|gpu::TextureUsage::sampled;
    Image result;
    result.allocation=r.textures->allocate(commands,{.extent={r.options.width,r.options.height,1},
        .format=format,.usage=usage,.sample_count=samples});
    require(result.allocation.texture!=nullptr,"Overlay texture heap exhausted");
    result.view=gpu::create_render_view(result.allocation.texture);
    if (!result.view) { r.textures->free(result.allocation); throw std::runtime_error("Overlay render view allocation failed"); }
    return result;
}
void uploadBytes(Renderer& r,gpu::GpuHeap& buffer,const void* data,uint64_t bytes) {
    if (!bytes) return;
    require(bytes<=std::numeric_limits<SceneBufferSize>::max(),"Overlay input exceeds production buffer limit");
    buffer=gpu::create_gpu_heap(r.device,(bytes+15)&~uint64_t{15},gpu::MemoryType::gpu_only);
    require(buffer.owner!=nullptr,buffer.error);
    r.uploads->upload_buffer({buffer.range.gpu,bytes},{static_cast<const gpu::byte*>(data),bytes});
    r.scene.bytes+=bytes;
}
void barrier(gpu::CommandBuffer* commands) {
    gpu::barrier(commands,gpu::Stage::all_commands,
        gpu::Access::color_write|gpu::Access::depth_stencil_write|gpu::Access::transfer_write|gpu::Access::shader_write,
        gpu::Stage::all_commands,gpu::Access::shader_read|gpu::Access::index_read|gpu::Access::color_write
            |gpu::Access::depth_stencil_read|gpu::Access::depth_stencil_write|gpu::Access::transfer_read);
}
void complete(Renderer& r,gpu::CommandBuffer* commands) {
    gpu::end_commands(commands);
    const std::array list{commands};
    const gpu::SubmitDesc submission{.commands=list,.completion={r.timeline,++r.sequence}};
    gpu::submit(r.device,submission);
    gpu::wait_timeline({r.timeline,r.sequence});
}
} // namespace

const char* methodName(Method method) {
    switch (method) {
    case Method::legacy:return "legacy";
    case Method::ordered:return "ordered";
    case Method::barycentric:return "barycentric";
    case Method::pulled:return "pulled";
    }
    throw std::invalid_argument("Invalid overlay method");
}
void validate(const Options& options,const Display& display) {
    require(options.width>=1 && options.height>=1 && options.width<=1920 && options.height<=1080,"Viewport must fit 1920x1080");
    require(options.samples==1 || options.samples==4,"Sample count must be 1 or 4");
    require(std::isfinite(display.pointSize) && display.pointSize>=1 && display.pointSize<=40,"Point size must be 1..40 pixels");
    require(std::isfinite(display.opacity) && display.opacity>=0 && display.opacity<=1,"Opacity must be 0..1");
    require(std::isfinite(display.edgeHalfWidth) && display.edgeHalfWidth>0 && display.edgeHalfWidth<=4,"Edge half width must be positive and at most 4");
    (void)methodName(display.method);
}
Renderer::~Renderer() {
    if (!device) return;
    gpu::wait_idle(device);
    uploads.reset();
    for (auto& [key,pso]:pipelines) { (void)key; gpu::destroy_pso(pso); }
    gpu::destroy_pso(capture);
    release(*this,color); release(*this,ids); release(*this,depth); release(*this,resolved);
    textures.reset(); gpu::destroy_texture_heap(textureHeap);
    gpu::destroy_texture_descriptor_heap(descriptors);
    arena.reset(); gpu::destroy_gpu_heap(roots); gpu::destroy_gpu_heap(readback); gpu::destroy_gpu_heap(captureIds);
    for (auto* buffer:{&scene.vertices,&scene.triangles,&scene.edges,&scene.points}) gpu::destroy_gpu_heap(*buffer);
    gpu::destroy_command_pool(pool); gpu::destroy_timeline_semaphore(timeline); gpu::destroy_device(device);
}
void initialize(Renderer& r,Options options) {
    validate(options,{}); require(r.device==nullptr,"Renderer already initialized"); r.options=options;
    r.device=gpu::create_device({}).device;
    require(r.device!=nullptr,"No compatible NoGraphicsAPI Vulkan device");
    require(gpu::get_device_caps(r.device).timestamp_period_ns>0,"GPU timestamps unavailable");
    r.timeline=gpu::create_timeline_semaphore(r.device); r.pool=gpu::create_command_pool(r.device);
    require(r.timeline && r.pool,"Overlay synchronization allocation failed");
    r.textureHeap=gpu::create_texture_heap(r.device,256ull*1024*1024);
    const auto allocator=gpu::create_utility<gpu::TextureAllocator>(r.device,r.textureHeap,16u);
    r.textures.reset(allocator.value); require(bool(r.textures),allocator.error);
    const auto uploader=gpu::create_utility<gpu::UploadQueue>(r.device,32ull*1024*1024);
    r.uploads.reset(uploader.value); require(bool(r.uploads),uploader.error);
    r.roots=gpu::create_gpu_heap(r.device,16ull*1024*1024);
    require(r.roots.range.cpu!=nullptr,r.roots.error);
    r.arena=std::make_unique<gpu::BumpAllocator>(r.roots.range);
    const uint64_t imageBytes=uint64_t(options.width)*options.height*4;
    r.readback=gpu::create_gpu_heap(r.device,imageBytes*2,gpu::MemoryType::readback);
    require(r.readback.range.cpu!=nullptr,r.readback.error);
    auto* commands=gpu::begin_commands(r.pool);
    r.color=image(r,commands,gpu::Format::rgba8_unorm,options.samples);
    r.depth=image(r,commands,gpu::Format::d32_float,options.samples);
    if (options.samples>1) r.resolved=image(r,commands,gpu::Format::rgba8_unorm,1);
    if (options.ids) {
        r.ids=image(r,commands,gpu::Format::rgba8_unorm,options.samples,true);
        r.descriptors=gpu::create_texture_descriptor_heap(r.device,1);
        require(r.descriptors!=nullptr,"Overlay descriptor allocation failed");
        gpu::write_texture_descriptor(r.descriptors,0,r.ids.allocation.texture,gpu::TextureDescriptorType::sampled);
        r.captureIds=gpu::create_gpu_heap(r.device,imageBytes,gpu::MemoryType::gpu_only);
        require(r.captureIds.owner!=nullptr,r.captureIds.error);
        const auto code=shader("cs_capture_ids");
        r.capture=gpu::create_compute_pso(r.device,stage(code,"cs_capture_ids"));
        require(r.capture!=nullptr,"Overlay capture pipeline creation failed");
    }
    complete(r,commands);
}
void upload(Renderer& r,const Mesh& mesh) {
    require(r.scene.vertices.owner==nullptr,"One immutable scene per experiment renderer");
    auto prepared=prepareSceneMesh(mesh,gpuMeshPoints|gpuMeshEdges);
    require(prepared.has_value(),"Overlay mesh preparation canceled");
    r.scene.vertexCount=mesh.vertices.size(); r.scene.triangleCount=mesh.indices.size()/3;
    r.scene.markerCount=prepared->pointVertexIndices.size();
    for (size_t i=0;i<prepared->nodeRanges.size();++i) {
        Group group; group.range=prepared->nodeRanges[i];
        // Stable varied colors expose incorrectly removed overlaps between groups.
        group.color={.22f+float((i*37)%53)/100,.28f+float((i*19)%47)/100,.3f+float((i*31)%43)/100,1};
        r.scene.groups.push_back(group);
    }
    uploadBytes(r,r.scene.vertices,mesh.vertices.data(),mesh.vertices.size()*sizeof(Vertex));
    uploadBytes(r,r.scene.triangles,mesh.indices.data(),mesh.indices.size()*sizeof(uint32_t));
    uploadBytes(r,r.scene.points,prepared->pointVertexIndices.data(),prepared->pointVertexIndices.size()*sizeof(uint32_t));
    uploadBytes(r,r.scene.edges,prepared->edgeIndices.data(),prepared->edgeIndices.size()*sizeof(uint32_t));
    r.uploads->wait();
}

Measurement render(Renderer& r,const Display& d,const std::array<float,16>& viewProjection,Capture* capture) {
    validate(r.options,d);
    if (d.method==Method::barycentric) require(gpu::get_device_caps(r.device).fragment_barycentric,
        "Native barycentric shaders unavailable; use pulled or ordered explicitly");
    const bool shaderEdges=(d.method==Method::barycentric || d.method==Method::pulled) && !d.xray;
    const auto edgeKind=d.method==Method::pulled?"pulled":"barycentric";
    // Create pipelines before the measured command recording interval.
    auto* solidPso=pipeline(r,"mesh",d.opacity<.999f);
    auto* depthPso=pipeline(r,"depth",false);
    auto* edgePso=pipeline(r,"line",d.opacity<.999f);
    auto* pointPso=pipeline(r,"point",d.opacity<.999f);
    auto* surfacePso=shaderEdges?pipeline(r,edgeKind,d.opacity<.999f || !d.solid):solidPso;
    gpu::reset_command_pool(r.pool); r.arena->reset(); r.timestamps={};
    const auto start=Clock::now();
    auto* commands=gpu::begin_commands(r.pool);
    if (r.descriptors) gpu::set_texture_descriptor_heap(commands,r.descriptors);
    barrier(commands);
    std::array<gpu::ColorAttachment,2> colors{};
    colors[0]={.render_view=r.color.view,.load=gpu::LoadOp::clear,.clear={.125f,.141f,.165f,1},.resolve_view=r.resolved.view};
    colors[1]={.render_view=r.ids.view,.load=gpu::LoadOp::clear,.clear={0,0,0,0}};
    gpu::begin_render_pass(commands,{.colors={colors.data(),r.options.ids?2u:1u},
        .depth={.render_view=r.depth.view,.load=gpu::LoadOp::clear,.clear=0}});
    gpu::set_viewport(commands,{0,0,float(r.options.width),float(r.options.height)});
    gpu::set_scissor(commands,{0,0,r.options.width,r.options.height});
    gpu::write_timestamp(commands,&r.timestamps[0]);
    Measurement measurement;
    const auto draw=[&](const Group& group,const char* kind) {
        if (d.opacity<=0) return;
        const auto& range=group.range;
        const bool isPoint=std::strcmp(kind,"point")==0,isLine=std::strcmp(kind,"line")==0;
        const bool isDepth=std::strcmp(kind,"depth")==0,isOverlay=std::strcmp(kind,"overlay")==0;
        if ((isPoint && range.pointIndexCount==0) || (!isPoint && range.triangleIndexCount==0)) return;
        auto root=r.arena->allocate<WobyRoot>(); require(root.cpu!=nullptr,"Overlay root arena exhausted");
        *root.cpu={};
        float mvp[16]; bx::mtxMul(mvp,group.model.data(),viewProjection.data());
        shaderMatrix(root.cpu->modelViewProj,mvp);
        shaderMatrix(root.cpu->model,group.model.data());
        const float scale=isPoint?1.5f:isLine?1.25f:1;
        root.cpu->color={std::min(1.0f,group.color[0]*scale),std::min(1.0f,group.color[1]*scale),
            std::min(1.0f,group.color[2]*scale),d.opacity};
        root.cpu->vertices=reinterpret_cast<float*>(r.scene.vertices.range.gpu); root.cpu->stride=sizeof(Vertex)/4;
        root.cpu->pointIds=reinterpret_cast<uint32_t*>(r.scene.points.range.gpu);
        root.cpu->pointParams[0]={d.pointSize,float(r.options.width),float(r.options.height),0};
        root.cpu->pointParams[1]={float(range.pointIndexOffset&65535u),float(range.pointIndexOffset>>16),0,0};
        const uint32_t first=range.pointIndexOffset+1;
        root.cpu->markerBase={float(first&65535u),float(first>>16),0,0};
        root.cpu->markerOptions={0,d.edgeHalfWidth,d.solid && group.solid?1.0f:0.0f,0};
        const bool opaque=d.opacity>=.999f;
        const bool depthTest=!(isLine && (d.method==Method::legacy || d.xray));
        gpu::set_depth_stencil(commands,{.depth_test=depthTest,
            .depth_write=opaque && (!isLine || depthTest) && !(isOverlay && !(d.solid && group.solid)),
            .depth_compare=isLine || isPoint || (isOverlay && !(d.solid && group.solid))
                ?gpu::CompareOp::greater_equal:gpu::CompareOp::greater});
        gpu::bind_pso(commands,isPoint?pointPso:isLine?edgePso:isDepth?depthPso:isOverlay?surfacePso:solidPso);
        if (isPoint) gpu::draw(commands,root.gpu,4,range.pointIndexCount);
        else if (isOverlay && d.method==Method::pulled) {
            root.cpu->pointIds=reinterpret_cast<uint32_t*>(r.scene.triangles.range.gpu)+range.triangleIndexOffset;
            gpu::draw(commands,root.gpu,range.triangleIndexCount);
        } else {
            const auto& buffer=isLine?r.scene.edges:r.scene.triangles;
            const uint32_t offset=isLine?range.lineIndexOffset:range.triangleIndexOffset;
            const uint32_t count=isLine?range.lineIndexCount:range.triangleIndexCount;
            gpu::draw_indexed(commands,root.gpu,{buffer.range.gpu+uint64_t(offset)*4,uint64_t(count)*4},gpu::IndexType::uint32,count);
        }
        ++measurement.draws;
    };
    if (d.method==Method::legacy) {
        for (const auto& group:r.scene.groups) {
            if (d.solid && group.solid) draw(group,"mesh");
            if (d.edges && group.edges) draw(group,"line");
            if (d.points && group.points) draw(group,"point");
        }
        measurement.separated=false;
        gpu::write_timestamp(commands,&r.timestamps[1]); gpu::write_timestamp(commands,&r.timestamps[2]);
    } else {
        for (const auto& group:r.scene.groups) {
            if (d.solid && group.solid) draw(group,shaderEdges && d.edges && group.edges?"overlay":"mesh");
            else if (!d.xray && d.edges && group.edges && d.opacity>=.999f) draw(group,"depth");
        }
        gpu::write_timestamp(commands,&r.timestamps[1]);
        for (const auto& group:r.scene.groups) {
            if (!d.edges || !group.edges) continue;
            if (shaderEdges) { if (!(d.solid && group.solid)) draw(group,"overlay"); }
            else draw(group,"line");
        }
        gpu::write_timestamp(commands,&r.timestamps[2]);
        for (const auto& group:r.scene.groups) if (d.points && group.points) draw(group,"point");
    }
    gpu::end_render_pass(commands);
    gpu::write_timestamp(commands,&r.timestamps[3]);
    if (capture) {
        barrier(commands);
        const uint64_t bytes=uint64_t(r.options.width)*r.options.height*4;
        gpu::copy_texture_to_memory(commands,r.resolved.allocation.texture?r.resolved.allocation.texture:r.color.allocation.texture,
            {r.readback.range.gpu,bytes});
        if (r.options.ids) {
            auto root=r.arena->allocate<WobyRoot>(); require(root.cpu!=nullptr,"Overlay capture arena exhausted");
            *root.cpu={}; root.cpu->texture0=0; root.cpu->markerQuery={0,0,float(r.options.width),float(r.options.height)};
            root.cpu->markerOptions={float(r.options.samples),0,0,0};
            root.cpu->pointIds=reinterpret_cast<uint32_t*>(r.captureIds.range.gpu);
            gpu::bind_pso(commands,r.capture);
            gpu::dispatch(commands,root.gpu,{(r.options.width+7)/8,(r.options.height+7)/8,1});
            barrier(commands);
            gpu::copy_memory(commands,{r.captureIds.range.gpu,bytes},{r.readback.range.gpu+bytes,bytes});
        }
        gpu::barrier(commands,gpu::Stage::transfer,gpu::Access::transfer_write,gpu::Stage::host,gpu::Access::host_read);
    }
    gpu::end_commands(commands);
    const std::array list{commands};
    gpu::submit(r.device,{.commands=list,.completion={r.timeline,++r.sequence}});
    measurement.cpuSubmitMs=milliseconds(Clock::now()-start);
    gpu::wait_timeline({r.timeline,r.sequence}); gpu::read_timestamps(r.pool);
    const double period=gpu::get_device_caps(r.device).timestamp_period_ns/1e6;
    measurement.totalMs=double(r.timestamps[3]-r.timestamps[0])*period;
    measurement.surfaceMs=double(r.timestamps[1]-r.timestamps[0])*period;
    measurement.edgeMs=double(r.timestamps[2]-r.timestamps[1])*period;
    measurement.pointMs=double(r.timestamps[3]-r.timestamps[2])*period;
    if (capture) {
        capture->width=r.options.width; capture->height=r.options.height;
        const size_t bytes=size_t(r.options.width)*r.options.height*4;
        capture->rgba.resize(bytes); std::memcpy(capture->rgba.data(),r.readback.range.cpu,bytes);
        capture->ids.clear();
        if (r.options.ids) { capture->ids.resize(bytes/4); std::memcpy(capture->ids.data(),r.readback.range.cpu+bytes,bytes); }
    }
    return measurement;
}
std::array<float,16> fittedProjection(const Bounds& bounds,uint32_t width,uint32_t height,float distanceScale,bool orthographic) {
    require(width && height && std::isfinite(distanceScale) && distanceScale>0,"Invalid camera dimensions or distance");
    auto camera=frameCameraBounds(bounds); camera.distance*=distanceScale;
    const auto range=cameraDepthRange(camera,bounds);
    float view[16],projection[16];
    bx::mtxLookAt(view,cameraEye(camera),cameraLookAt(camera),cameraUp(camera));
    const float aspect=float(width)/float(height);
    if (orthographic) {
        const float radius=bounds.radius*1.2f*distanceScale;
        bx::mtxOrtho(projection,-radius*aspect,radius*aspect,-radius,radius,range.nearPlane,range.farPlane,0,false);
    } else bx::mtxProj(projection,cameraViewportFov(camera,aspect),aspect,range.nearPlane,range.farPlane,false);
    // Same forward-to-reversed [0,1] transformation used by scenePickView.
    for (size_t column=0;column<4;++column) projection[column*4+2]=projection[column*4+3]-projection[column*4+2];
    std::array<float,16> result{}; bx::mtxMul(result.data(),view,projection); return result;
}
void savePng(const Capture& capture,const std::filesystem::path& path) {
    auto* surface=SDL_CreateSurfaceFrom(static_cast<int>(capture.width),static_cast<int>(capture.height),
        SDL_PIXELFORMAT_RGBA32,const_cast<uint8_t*>(capture.rgba.data()),static_cast<int>(capture.width*4));
    require(surface!=nullptr,SDL_GetError());
    const auto utf8=path.u8string(); const bool saved=SDL_SavePNG(surface,reinterpret_cast<const char*>(utf8.c_str()));
    SDL_DestroySurface(surface); require(saved,SDL_GetError());
}
Mesh fixtureMesh() {
    Mesh mesh;
    for (const auto& p:std::array<std::array<float,3>,7>{{{-.85f,-.7f,.3f},{.85f,-.7f,.3f},{0,.85f,.3f},
            {-.42f,-.35f,.6f},{.42f,-.35f,.6f},{.42f,.4f,.6f},{-.42f,.4f,.6f}}}) {
        Vertex vertex; vertex.position=p; vertex.normal={0,0,1}; mesh.vertices.push_back(vertex);
    }
    mesh.indices={0,1,2,3,4,5,3,5,6};
    mesh.nodes={{"rear",0,3},{"front",3,6}}; mesh.bounds=calculateBounds(mesh.vertices);
    return mesh;
}
} // namespace woby::overlay
