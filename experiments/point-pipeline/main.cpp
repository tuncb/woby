#include "root.h"
#include "point_cloud.h"
#include "scene_mesh_preparation.h"
#include "marker_pick.h"
#include "math_utils.h"
#include "obj_mesh.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using Matrix=std::array<float,16>;
using Json=nlohmann::json;
// NoGraphicsAPI requires every root address to have 16-byte alignment.
constexpr uint64_t rootStride=(sizeof(PipelineRoot)+15u)&~uint64_t{15};
enum class Mode { reference, precise, depth, depthPrecise, combined, combinedOnce, production };
constexpr std::array modes{Mode::reference,Mode::precise,Mode::depth,Mode::depthPrecise,Mode::combined,Mode::combinedOnce,Mode::production};
const char* name(Mode mode) {
    switch (mode) {
        case Mode::reference:return "reference";
        case Mode::precise:return "precise";
        case Mode::depth:return "depth";
        case Mode::depthPrecise:return "depth-precise";
        case Mode::combined:return "combined";
        case Mode::combinedOnce:return "combined-once";
        case Mode::production:return "production";
    }
    throw std::runtime_error("Unknown mode");
}
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void check(const char* error) { require(error==nullptr,error); }
void matrix(float4x4& to,const Matrix& from) {
    for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c) to.rows[r][c]=from[c*4+r];
}
Matrix identity() { Matrix value; woby::identityMatrix(value.data()); return value; }
struct Image { gpu::PlacedTexture placed{}; gpu::RenderView* view=nullptr; };
struct Renderer {
    gpu::Device* device=nullptr;
    gpu::CommandPool* pool=nullptr;
    gpu::TimelineSemaphore* timeline=nullptr;
    gpu::TextureDescriptorHeap* descriptors=nullptr;
    gpu::TextureHeap textureHeap{};
    std::unique_ptr<gpu::TextureAllocator> textures;
    std::unique_ptr<gpu::UploadQueue> uploads;
    gpu::GpuHeap vertices{},indices{},points{},colors{},vertexColors{},vertexIds{},claims{},winners{},coverage{},tasks{},groups{},root{},capture{},readback{},auditBounds{};
    Image color,ids,depth,accumulation,revealage;
    gpu::PSO *surface=nullptr,*combined=nullptr,*clear=nullptr,*raster=nullptr,*resolve=nullptr,*seed=nullptr,*claimClear=nullptr,*captureProgram=nullptr;
    gpu::PSO *productionClear=nullptr,*productionRaster=nullptr;
    std::vector<gpu::PSO*> auditPrograms;
    gpu::GpuHeap auditVertices{},auditUvs{};
    gpu::GpuHeap auditPackedPoints{},auditPackedTasks{},auditEncoding{};
    uint64_t packedPointCount=0;
    std::array<uint64_t,4> packedFormats{};
    uint32_t width=1280,height=687,samples=4,x=0,y=0,pointCount=0,taskCount=0;
    uint64_t sequence=0,geometryBytes=0,combinedBytes=0;
    std::vector<woby::GpuNodeRange> ranges;
    std::vector<OpaquePointGroupData> groupData;
    bool combineSupported=false;
    ~Renderer() {
        if (!device) return;
        gpu::wait_idle(device);
        for (auto* pso:{surface,combined,clear,raster,resolve,seed,claimClear,captureProgram,productionClear,productionRaster}) gpu::destroy_pso(pso);
        for (auto* pso:auditPrograms) gpu::destroy_pso(pso);
        uploads.reset();
        for (auto* image:{&color,&ids,&depth,&accumulation,&revealage}) { gpu::destroy_render_view(image->view); if (image->placed.texture) textures->free(image->placed); }
        textures.reset(); gpu::destroy_texture_heap(textureHeap); gpu::destroy_texture_descriptor_heap(descriptors);
        for (auto* heap:{&vertices,&indices,&points,&colors,&vertexColors,&vertexIds,&claims,&winners,&coverage,&tasks,&groups,&root,&capture,&readback,&auditBounds,&auditVertices,&auditUvs,&auditPackedPoints,&auditPackedTasks,&auditEncoding}) gpu::destroy_gpu_heap(*heap);
        gpu::destroy_command_pool(pool); gpu::destroy_timeline_semaphore(timeline); gpu::destroy_device(device);
    }
};
void allocate(Renderer& r,gpu::GpuHeap& heap,uint64_t bytes,gpu::MemoryType type=gpu::MemoryType::gpu_only) {
    heap=gpu::create_gpu_heap(r.device,std::max(uint64_t{16},(bytes+15)&~uint64_t{15}),type); require(heap.owner!=nullptr,heap.error);
}
void upload(Renderer& r,gpu::GpuHeap& heap,const void* data,uint64_t bytes) {
    if (!bytes) return;
    allocate(r,heap,bytes);
    constexpr uint64_t chunk=16ull*1024*1024;
    for (uint64_t offset=0;offset<bytes;offset+=chunk) {
        const auto count=std::min(chunk,bytes-offset);
        r.uploads->upload_buffer({heap.range.gpu+offset,count},{static_cast<const gpu::byte*>(data)+offset,count});
        r.uploads->wait(); check(r.uploads->error);
    }
    r.geometryBytes+=bytes;
}
std::vector<uint32_t> shader(const std::filesystem::path& directory,const char* entry) {
    std::ifstream file(directory/(std::string(entry)+".bin"),std::ios::binary|std::ios::ate);
    require(bool(file),"Cannot open shader"); const auto bytes=static_cast<size_t>(file.tellg());
    require(bytes && bytes%4==0,"Invalid shader size"); std::vector<uint32_t> code(bytes/4); file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(bytes)); require(bool(file),"Cannot read shader"); return code;
}
gpu::ShaderStage stage(const std::vector<uint32_t>& code,const char* entry) {
    return {.code={reinterpret_cast<const gpu::byte*>(code.data()),code.size()*4},.entry_point=entry};
}
gpu::PSO* compute(Renderer& r,const std::filesystem::path& directory,const char* entry) {
    const auto code=shader(directory,entry); auto* pso=gpu::create_compute_pso(r.device,stage(code,entry));
    require(pso!=nullptr,"Compute pipeline creation failed"); return pso;
}
gpu::PSO* graphics(Renderer& r,const std::filesystem::path& vsDirectory,const char* vs,
    const std::filesystem::path& fsDirectory,const char* fs,gpu::PrimitiveTopology topology=gpu::PrimitiveTopology::triangles,int transparency=0) {
    const auto vertex=shader(vsDirectory,vs),fragment=shader(fsDirectory,fs);
    std::array<gpu::ColorTargetDesc,2> targets{{{.format=gpu::Format::rgba8_unorm},{.format=gpu::Format::rgba8_unorm}}};
    if (transparency==1) {
        targets[0].format=gpu::Format::rgba16_float; targets[1].format=gpu::Format::r16_float;
        targets[0].blend={.enabled=true,.color={gpu::BlendFactor::one,gpu::BlendFactor::one},.alpha={gpu::BlendFactor::one,gpu::BlendFactor::one}};
        targets[1].blend={.enabled=true,.color={gpu::BlendFactor::zero,gpu::BlendFactor::one_minus_source_color},
            .alpha={gpu::BlendFactor::zero,gpu::BlendFactor::one_minus_source_alpha}}; targets[1].write_mask=1;
    } else if (transparency==2) {
        targets[0].blend={.enabled=true,.color={gpu::BlendFactor::source_alpha,gpu::BlendFactor::one_minus_source_alpha},
            .alpha={gpu::BlendFactor::one,gpu::BlendFactor::one_minus_source_alpha}}; targets[1].write_mask=0;
    }
    auto* pso=gpu::create_graphics_pso(r.device,{.vertex=stage(vertex,vs),.fragment=stage(fragment,fs),
        .color_targets=targets,.depth_format=gpu::Format::d32_float,.sample_count=r.samples,.topology=topology});
    require(pso!=nullptr,"Graphics pipeline creation failed"); return pso;
}
void complete(Renderer& r,gpu::CommandBuffer* commands) {
    check(gpu::end_commands(commands)); const std::array list{commands};
    gpu::submit(r.device,{.commands=list,.completion={r.timeline,++r.sequence}});
    gpu::wait_timeline({r.timeline,r.sequence}); gpu::read_timestamps(r.pool);
}
void broad(gpu::CommandBuffer* commands) {
    using enum gpu::Access;
    gpu::barrier(commands,gpu::Stage::all_commands,transfer_write|shader_write|color_write|depth_stencil_write,
        gpu::Stage::all_commands,transfer_read|transfer_write|shader_read|shader_write|color_read|color_write|depth_stencil_read|depth_stencil_write|index_read);
}
void initialize(Renderer& r,const woby::Mesh& mesh,const woby::points::Cloud& cloud,
    const std::vector<woby::GpuNodeRange>& ranges,const std::filesystem::path& reference,const std::filesystem::path& production,bool audit=false,bool splitVertices=false,bool packedPoints=false,bool packed21=false) {
    r.device=gpu::create_device({}).device; require(r.device!=nullptr,"No compatible Vulkan device");
    const auto caps=gpu::get_device_caps(r.device);
    require(caps.point_buffer_int64_atomics && caps.fragment_barycentric && caps.timestamp_period_ns>0,"Required GPU features are unavailable");
    r.pool=gpu::create_command_pool(r.device); r.timeline=gpu::create_timeline_semaphore(r.device);
    require(r.pool && r.timeline,"Cannot create command resources");
    const auto uploader=gpu::create_utility<gpu::UploadQueue>(r.device,32ull*1024*1024);
    r.uploads.reset(uploader.value); require(bool(r.uploads),uploader.error);
    const auto& geometry=woby::meshGeometry(mesh); r.ranges=ranges;
    require(cloud.points.size()<=UINT32_MAX && geometry.indices.size()<=UINT32_MAX,"Input is too large");
    r.pointCount=static_cast<uint32_t>(cloud.points.size());
    if (!geometry.indices.empty()) {
        upload(r,r.vertices,geometry.vertices.data(),geometry.vertices.size()*sizeof(woby::Vertex));
        upload(r,r.indices,geometry.indices.data(),geometry.indices.size()*sizeof(uint32_t));
        upload(r,r.vertexColors,geometry.vertexColors.data(),geometry.vertexColors.size()*sizeof(uint32_t));
        if (splitVertices) {
            std::vector<std::array<float,6>> positionsAndNormals; positionsAndNormals.reserve(geometry.vertices.size());
            std::vector<std::array<float,2>> uvs; uvs.reserve(geometry.vertices.size());
            for (const auto& vertex:geometry.vertices) {
                positionsAndNormals.push_back({vertex.position[0],vertex.position[1],vertex.position[2],vertex.normal[0],vertex.normal[1],vertex.normal[2]});
                uvs.push_back(vertex.texcoord);
            }
            upload(r,r.auditVertices,positionsAndNormals.data(),positionsAndNormals.size()*sizeof(positionsAndNormals[0]));
            upload(r,r.auditUvs,uvs.data(),uvs.size()*sizeof(uvs[0]));
        }
    }
    upload(r,r.points,cloud.points.data(),cloud.points.size()*sizeof(woby::points::Point));
    upload(r,r.colors,cloud.colors.data(),cloud.colors.size()*sizeof(uint32_t));
    r.combineSupported=ranges.size()==1 && ranges[0].triangleIndexCount!=0 && !geometry.indices.empty();
    if (r.combineSupported && !audit) {
        std::vector<uint32_t> mapping(geometry.vertices.size());
        for (uint32_t i=0;i<cloud.sourceVertices.size();++i) mapping[cloud.sourceVertices[i]]=i+1;
        upload(r,r.vertexIds,mapping.data(),mapping.size()*sizeof(uint32_t));
        allocate(r,r.claims,cloud.points.size()*sizeof(uint32_t));
        r.combinedBytes=(mapping.size()+cloud.points.size())*sizeof(uint32_t);
    }
    std::vector<OpaquePointTaskData> taskData;
    std::vector<std::array<float,6>> boundsData;
    std::vector<OpaquePointTaskData> packedTasks;
    std::vector<std::array<uint32_t,4>> encodings;
    std::vector<uint32_t> packedWords;
    std::vector<size_t> packedOffsets;
    for (const auto nodeIndex:cloud.roots) {
        const auto& node=cloud.nodes[nodeIndex];
        for (uint32_t used=0;used<node.count;) {
            const auto count=std::min(4096u,node.count-used);
            taskData.push_back({reinterpret_cast<OpaquePoint*>(r.points.range.gpu)+node.begin+used,count,node.group,0,0});
            if (packedPoints) {
                std::array<uint32_t,3> low{UINT32_MAX,UINT32_MAX,UINT32_MAX},high{};
                for (uint32_t i=0;i<count;++i) for (size_t axis=0;axis<3;++axis) {
                    const auto bits=std::bit_cast<uint32_t>(cloud.points[node.begin+used+i].position[axis]);
                    low[axis]=std::min(low[axis],bits); high[axis]=std::max(high[axis],bits);
                }
                const bool xFits=high[0]-low[0]<=65535,yFits=high[1]-low[1]<=65535,zFits=high[2]-low[2]<=65535;
                const uint32_t format=packed21
                    ?(high[0]-low[0]<=0x1fffff && high[1]-low[1]<=0x1fffff && high[2]-low[2]<=0x3fffff?3u:4u)
                    :xFits && yFits?3u:yFits && zFits?1u:xFits && zFits?2u:4u;
                const bool compact=format!=4;
                r.packedFormats[format-1]+=count;
                encodings.push_back({low[0],low[1],low[2],format});
                packedTasks.push_back(taskData.back()); packedOffsets.push_back(packedWords.size());
                if (compact) {
                    r.packedPointCount+=count;
                    for (uint32_t i=0;i<count;++i) {
                        const auto& point=cloud.points[node.begin+used+i];
                        const auto x=std::bit_cast<uint32_t>(point.position[0])-low[0];
                        const auto y=std::bit_cast<uint32_t>(point.position[1])-low[1];
                        const auto z=std::bit_cast<uint32_t>(point.position[2])-low[2];
                        const auto pair=packed21?x|((y&0x7ff)<<21):format==1?y|(z<<16):format==2?x|(z<<16):x|(y<<16);
                        const auto remaining=packed21?(y>>11)|(z<<10):std::bit_cast<uint32_t>(point.position[format-1]);
                        packedWords.insert(packedWords.end(),{pair,remaining,point.id});
                    }
                }
            }
            if (audit) {
                const auto first=cloud.points[node.begin+used].position;
                std::array<float,6> bounds{first[0],first[1],first[2],first[0],first[1],first[2]};
                for (uint32_t i=1;i<count;++i) for (size_t axis=0;axis<3;++axis) {
                    const auto coordinate=cloud.points[node.begin+used+i].position[axis];
                    bounds[axis]=std::min(bounds[axis],coordinate); bounds[axis+3]=std::max(bounds[axis+3],coordinate);
                }
                boundsData.push_back(bounds);
            }
            used+=count;
        }
    }
    upload(r,r.tasks,taskData.data(),taskData.size()*sizeof(OpaquePointTaskData)); r.taskCount=static_cast<uint32_t>(taskData.size());
    if (packedPoints) {
        upload(r,r.auditPackedPoints,packedWords.data(),packedWords.size()*sizeof(uint32_t));
        for (size_t i=0;i<packedTasks.size();++i) if (encodings[i][3]!=4)
            packedTasks[i].points=reinterpret_cast<OpaquePoint*>(r.auditPackedPoints.range.gpu+packedOffsets[i]*sizeof(uint32_t));
        upload(r,r.auditPackedTasks,packedTasks.data(),packedTasks.size()*sizeof(OpaquePointTaskData));
        upload(r,r.auditEncoding,encodings.data(),encodings.size()*sizeof(encodings[0]));
    }
    if (audit) upload(r,r.auditBounds,boundsData.data(),boundsData.size()*sizeof(boundsData[0]));
    r.groupData.resize(cloud.groups.size());
    for (size_t i=0;i<cloud.groups.size();++i) {
        const auto& from=cloud.groups[i]; auto& group=r.groupData[i];
        group.firstId=group.sourceFirstId=from.firstId; group.endId=from.endId;
        group.color={.495f,.63f,.675f,1}; group.colors=reinterpret_cast<uint32_t*>(r.colors.range.gpu);
        group.colorCount=static_cast<uint32_t>(cloud.colors.size());
    }
    allocate(r,r.groups,r.groupData.size()*sizeof(OpaquePointGroupData),gpu::MemoryType::cpu_visible);
    allocate(r,r.root,(r.ranges.size()+1)*rootStride,gpu::MemoryType::cpu_visible);
    const auto pixels=uint64_t(r.width)*r.height;
    allocate(r,r.winners,pixels*r.samples*8); allocate(r,r.coverage,pixels*8);
    allocate(r,r.capture,pixels*r.samples*sizeof(PipelineSample));
    allocate(r,r.readback,pixels*r.samples*sizeof(PipelineSample),gpu::MemoryType::readback);
    const uint64_t textureBytes=std::max(128ull*1024*1024,uint64_t(r.width+r.x+3)*(r.height+r.y+3)*r.samples*24+64ull*1024*1024);
    r.textureHeap=gpu::create_texture_heap(r.device,textureBytes); require(r.textureHeap.owner!=nullptr,r.textureHeap.error);
    const auto allocator=gpu::create_utility<gpu::TextureAllocator>(r.device,r.textureHeap,16u);
    r.textures.reset(allocator.value); require(bool(r.textures),allocator.error);
    r.descriptors=gpu::create_texture_descriptor_heap(r.device,audit?5u:3u); require(r.descriptors!=nullptr,"Cannot create descriptors");
    auto* commands=gpu::begin_commands(r.pool); require(commands!=nullptr,"Cannot begin commands");
    const std::array imageList{&r.color,&r.ids,&r.depth,&r.accumulation,&r.revealage};
    for (uint32_t i=0;i<(audit?5u:3u);++i) {
        auto& image=*imageList[i]; const bool depth=i==2;
        image.placed=r.textures->allocate(commands,{.extent={r.width+r.x+3,r.height+r.y+3,1},
            .format=depth?gpu::Format::d32_float:i==3?gpu::Format::rgba16_float:i==4?gpu::Format::r16_float:gpu::Format::rgba8_unorm,
            .usage=gpu::TextureUsage::sampled|(depth?gpu::TextureUsage::depth_stencil_attachment:gpu::TextureUsage::color_attachment),.sample_count=r.samples});
        require(image.placed.texture!=nullptr,"Cannot allocate image"); image.view=gpu::create_render_view(image.placed.texture);
        require(image.view!=nullptr,"Cannot allocate render view");
        gpu::write_texture_descriptor(r.descriptors,i,image.placed.texture,gpu::TextureDescriptorType::sampled);
    }
    complete(r,commands);
    const std::filesystem::path experiment=WOBY_PIPELINE_SHADER_DIR;
    if (!audit) {
    r.surface=graphics(r,reference,"vs_mesh",reference,"fs_marker_mesh_edges");
    r.combined=graphics(r,experiment,"vs_pipeline_combined",reference,"fs_marker_mesh_edges");
    r.resolve=graphics(r,reference,"vs_opaque_resolve",reference,"fs_opaque_resolve");
    r.clear=compute(r,reference,"cs_opaque_clear"); r.raster=compute(r,reference,"cs_opaque_raster");
    r.seed=compute(r,experiment,"cs_pipeline_seed"); r.claimClear=compute(r,experiment,"cs_pipeline_claim_clear");
    r.productionClear=compute(r,production,"cs_opaque_clear"); r.productionRaster=compute(r,production,"cs_opaque_raster");
    }
    r.captureProgram=compute(r,experiment,"cs_pipeline_capture");
}
struct Result { double gpuMs=0; std::vector<PipelineSample> samples; };
struct AuditDraw {
    std::string kind="combined";
    gpu::PSO *surface=nullptr,*clear=nullptr,*raster=nullptr,*resolve=nullptr;
    float nearPlane=.1f,farPlane=1000;
    bool splitVertices=false;
    bool packedPoints=false;
};
Result render(Renderer& r,Mode mode,const Matrix& projection,float size,bool capture,bool solid=true,const AuditDraw* audit=nullptr) {
    const bool points=!audit || audit->kind=="combined" || audit->kind=="points";
    const bool transparent=audit && audit->kind.starts_with("transparent");
    if (audit && (audit->kind=="points" || audit->kind=="quads")) solid=false;
    const bool combined=mode==Mode::combined || mode==Mode::combinedOnce;
    const bool precise=mode==Mode::precise || mode==Mode::depthPrecise || combined;
    const bool seed=(mode==Mode::depth || mode==Mode::depthPrecise) && solid;
    require(!combined || (r.combineSupported && solid),"Combined prototype requires one opaque triangle group");
    check(gpu::reset_command_pool(r.pool));
    auto* commands=gpu::begin_commands(r.pool); require(commands!=nullptr,"Cannot begin commands");
    gpu::set_texture_descriptor_heap(commands,r.descriptors);
    for (auto& group:r.groupData) { matrix(group.matrix,projection); group.pointSize=size; }
    std::memcpy(r.groups.range.cpu,r.groupData.data(),r.groupData.size()*sizeof(OpaquePointGroupData));
    PipelineRoot value{};
    auto& point=value.points;
    point.winners=reinterpret_cast<uint64_t*>(r.winners.range.gpu); point.coverage=reinterpret_cast<uint64_t*>(r.coverage.range.gpu);
    point.groups=reinterpret_cast<OpaquePointGroupData*>(r.groups.range.gpu); point.groupCount=static_cast<uint32_t>(r.groupData.size());
    point.tasks=reinterpret_cast<OpaquePointTaskData*>(r.tasks.range.gpu); point.taskCount=r.taskCount; point.dispatchWidth=std::min(r.taskCount,65535u);
    point.width=r.width; point.height=r.height; point.samples=r.samples; point.x=r.x; point.y=r.y; point.tileShift=16;
    point.depthTexture=2;
    // The audit's 64-bit point path does not use the portable batch buffer.
    // Experimental block rejection reads separate bounds through this spare slot.
    if (audit) point.batch=reinterpret_cast<OpaquePointBatchSample*>(r.auditBounds.range.gpu);
    if (audit && audit->packedPoints) {
        point.tasks=reinterpret_cast<OpaquePointTaskData*>(r.auditPackedTasks.range.gpu);
        point.batch=reinterpret_cast<OpaquePointBatchSample*>(r.auditEncoding.range.gpu);
    }
    point.seedDepth=mode==Mode::production && r.combineSupported && solid && size>=4 && r.pointCount>=1000000?1u:0u;
    if (size>=2 && r.pointCount>=4096) {
        point.tileSize=1; point.tileShift=0;
        while (point.tileSize<16 && float(point.tileSize*4)<=size) { point.tileSize*=2; ++point.tileShift; }
    }
    auto& surface=value.surface; matrix(surface.model,identity()); matrix(surface.modelViewProj,projection);
    surface.vertices=reinterpret_cast<float*>(r.vertices.range.gpu); surface.stride=sizeof(woby::Vertex)/4;
    surface.vertexColors=reinterpret_cast<uint32_t*>(r.vertexColors.range.gpu);
    surface.vertexColorCount=static_cast<uint32_t>(r.vertexColors.range.size/4);
    if (audit && audit->splitVertices) {
        surface.vertices=reinterpret_cast<float*>(r.auditVertices.range.gpu); surface.stride=6;
        surface.analysisFaces=reinterpret_cast<float*>(r.auditUvs.range.gpu);
    }
    surface.color={.33f,.42f,.45f,1}; surface.triangleEdges={.5f,1}; surface.transparency.x=1;
    if (audit && audit->kind=="uv") surface.uvGrid={8,8,1,0};
    if (audit && audit->kind=="comparison") surface.comparison={.1f,1,2,0};
    if (transparent) { surface.color.w=.4f; surface.transparency={1,audit->nearPlane,audit->farPlane,1}; }
    if (audit && audit->kind=="line-sprites") surface.pointParams={size,float(r.width),float(r.height),0};
    value.vertexIds=reinterpret_cast<uint32_t*>(r.vertexIds.range.gpu); value.claimed=reinterpret_cast<uint32_t*>(r.claims.range.gpu);
    value.captured=reinterpret_cast<PipelineSample*>(r.capture.range.gpu); value.pointCount=r.pointCount;
    value.claimOnce=mode==Mode::combinedOnce?1u:0u; value.colorTexture=0; value.idTexture=1; value.depthTexture=2;
    std::memcpy(r.root.range.cpu,&value,sizeof(value));
    const auto* pointRoot=r.root.range.gpu+offsetof(PipelineRoot,points);
    const gpu::uint32x3 pixels{(r.width+7)/8,(r.height+7)/8,1};
    const auto dispatch=[&](gpu::PSO* pso,const void* data,gpu::uint32x3 grid) {
        gpu::bind_pso(commands,pso); gpu::dispatch(commands,data,grid);
    };
    const auto computeBarrier=[&](gpu::Stage destination=gpu::Stage::compute) {
        if (!precise) broad(commands);
        else gpu::barrier(commands,gpu::Stage::compute,gpu::Access::shader_write,destination,gpu::Access::shader_read|gpu::Access::shader_write);
    };
    std::array<gpu::ColorAttachment,2> colors{{{.render_view=r.color.view,.load=gpu::LoadOp::clear,.clear={.125f,.141f,.165f,1}},
        {.render_view=r.ids.view,.load=gpu::LoadOp::clear,.clear={0,0,0,0}}}};
    const auto viewport=[&] {
        gpu::set_viewport(commands,{float(r.x),float(r.y),float(r.width),float(r.height)});
        gpu::set_scissor(commands,{static_cast<int32_t>(r.x),static_cast<int32_t>(r.y),r.width,r.height});
    };
    broad(commands); std::array<uint64_t,2> timestamps{}; gpu::write_timestamp(commands,&timestamps[0]);
    if (combined) {
        dispatch(r.clear,pointRoot,pixels);
        if (mode==Mode::combinedOnce) {
            const auto blocks=(r.pointCount+127)/128;
            dispatch(r.claimClear,r.root.range.gpu,{std::min(blocks,65535u),(blocks+65534)/65535,1});
        }
        computeBarrier(gpu::Stage::vertex);
    }
    gpu::begin_render_pass(commands,{.colors=colors,.depth={.render_view=r.depth.view,.load=gpu::LoadOp::clear,.clear=0}});
    viewport();
    if (transparent) {
        gpu::end_render_pass(commands); broad(commands);
        const std::array<gpu::ColorAttachment,2> weighted{{{.render_view=r.accumulation.view,.load=gpu::LoadOp::clear,.clear={0,0,0,0}},
            {.render_view=r.revealage.view,.load=gpu::LoadOp::clear,.clear={1,1,1,1}}}};
        gpu::begin_render_pass(commands,{.colors=weighted,.depth={.render_view=r.depth.view,.load=gpu::LoadOp::load}}); viewport();
    }
    if (solid) {
        gpu::set_depth_stencil(commands,{.depth_test=true,.depth_write=!transparent,.depth_compare=gpu::CompareOp::greater});
        gpu::bind_pso(commands,audit?audit->surface:combined?r.combined:r.surface);
        for (size_t rangeIndex=0;rangeIndex<r.ranges.size();++rangeIndex) {
            const auto& range=r.ranges[rangeIndex]; if (!range.triangleIndexCount) continue;
            if (audit && (audit->kind=="pulled" || audit->kind=="lines" || audit->kind=="line-sprites" || audit->kind=="transparent-pulled")) {
                value.surface.pointIds=reinterpret_cast<uint32_t*>(r.indices.range.gpu);
                value.surface.triangleOffset=range.triangleIndexOffset;
                value.surface.pointOffset=range.triangleIndexOffset;
                const auto offset=rangeIndex*rootStride;
                std::memcpy(r.root.range.cpu+offset,&value,sizeof(value));
                if (audit->kind=="line-sprites") gpu::draw(commands,r.root.range.gpu+offset,4,range.triangleIndexCount/2);
                else gpu::draw(commands,r.root.range.gpu+offset,range.triangleIndexCount*(audit->kind=="lines"?2u:1u));
            } else gpu::draw_indexed(commands,r.root.range.gpu,
                {r.indices.range.gpu+uint64_t(range.triangleIndexOffset)*4,uint64_t(range.triangleIndexCount)*4},gpu::IndexType::uint32,range.triangleIndexCount);
        }
    }
    if (audit && audit->kind=="quads") {
        value.surface.vertices=reinterpret_cast<float*>(r.points.range.gpu); value.surface.stride=4;
        value.surface.vertexColors=reinterpret_cast<uint32_t*>(r.colors.range.gpu);
        value.surface.vertexColorCount=static_cast<uint32_t>(r.colors.range.size/4);
        value.surface.pointParams={size,float(r.width),float(r.height),1};
        value.surface.pointSourceFirst=1; value.surface.markerBase=1;
        std::memcpy(r.root.range.cpu,&value,sizeof(value));
        gpu::set_depth_stencil(commands,{.depth_test=true,.depth_write=true,.depth_compare=gpu::CompareOp::greater_equal});
        gpu::bind_pso(commands,audit->surface); gpu::draw(commands,r.root.range.gpu,4,r.pointCount);
    }
    gpu::end_render_pass(commands);
    if (transparent) {
        broad(commands); colors[0].load=colors[1].load=gpu::LoadOp::load;
        gpu::begin_render_pass(commands,{.colors=colors,.depth={.render_view=r.depth.view,.load=gpu::LoadOp::load}}); viewport();
        // Earlier surface draws still read their roots when this list executes.
        // The resolve must not overwrite the first group's index offsets.
        const auto resolveOffset=r.ranges.size()*rootStride;
        value.surface.texture0=3; value.surface.texture1=4;
        std::memcpy(r.root.range.cpu+resolveOffset,&value,sizeof(value));
        gpu::set_depth_stencil(commands,{.depth_test=false,.depth_write=false});
        gpu::bind_pso(commands,audit->resolve); gpu::draw(commands,r.root.range.gpu+resolveOffset,4); gpu::end_render_pass(commands);
    }
    if (!combined && points) {
        if (!precise) broad(commands);
        else if (seed) gpu::barrier(commands,gpu::Stage::depth_stencil_tests,gpu::Access::depth_stencil_write,gpu::Stage::compute,gpu::Access::shader_read);
        if (seed) dispatch(r.seed,r.root.range.gpu,pixels);
        else dispatch(audit?audit->clear:mode==Mode::production?r.productionClear:r.clear,pointRoot,pixels);
        computeBarrier();
        dispatch(audit?audit->raster:mode==Mode::production?r.productionRaster:r.raster,pointRoot,{point.dispatchWidth,(point.taskCount+point.dispatchWidth-1)/point.dispatchWidth,1});
    }
    if (!precise) broad(commands);
    else gpu::barrier(commands,(combined?gpu::Stage::vertex:gpu::Stage::compute)|gpu::Stage::color_output|gpu::Stage::depth_stencil_tests,
        gpu::Access::shader_write|gpu::Access::color_write|gpu::Access::depth_stencil_write,
        gpu::Stage::fragment|gpu::Stage::color_output|gpu::Stage::depth_stencil_tests,
        gpu::Access::shader_read|gpu::Access::color_read|gpu::Access::color_write|gpu::Access::depth_stencil_read|gpu::Access::depth_stencil_write);
    if (points) {
    colors[0].load=colors[1].load=gpu::LoadOp::load;
    gpu::begin_render_pass(commands,{.colors=colors,.depth={.render_view=r.depth.view,.load=gpu::LoadOp::load}});
    viewport(); gpu::set_depth_stencil(commands,{.depth_test=true,.depth_write=true,.depth_compare=gpu::CompareOp::greater_equal});
    gpu::bind_pso(commands,audit?audit->resolve:r.resolve); gpu::draw(commands,pointRoot,3); gpu::end_render_pass(commands);
    }
    broad(commands); gpu::write_timestamp(commands,&timestamps[1]);
    const uint64_t bytes=uint64_t(r.width)*r.height*r.samples*sizeof(PipelineSample);
    if (capture) {
        dispatch(r.captureProgram,r.root.range.gpu,pixels); broad(commands);
        gpu::copy_memory(commands,{r.capture.range.gpu,bytes},{r.readback.range.gpu,bytes});
        gpu::barrier(commands,gpu::Stage::transfer,gpu::Access::transfer_write,gpu::Stage::host,gpu::Access::host_read);
    }
    complete(r,commands);
    require(timestamps[1]>timestamps[0],"GPU timestamps are unavailable");
    Result result; result.gpuMs=double(timestamps[1]-timestamps[0])*gpu::get_device_caps(r.device).timestamp_period_ns/1e6;
    if (capture) { result.samples.resize(static_cast<size_t>(bytes/sizeof(PipelineSample))); std::memcpy(result.samples.data(),r.readback.range.cpu,static_cast<size_t>(bytes)); }
    return result;
}
Json compare(const Result& reference,const Result& actual) {
    require(reference.samples.size()==actual.samples.size(),"Sample count differs");
    uint64_t colors=0,ids=0,depths=0,visible=0,filled=0; size_t first=reference.samples.size();
    for (size_t i=0;i<reference.samples.size();++i) {
        const auto& a=reference.samples[i]; const auto& b=actual.samples[i];
        colors+=a.color!=b.color; ids+=a.id!=b.id; depths+=a.depth!=b.depth; visible+=a.id!=0;
        filled+=a.color!=0xff2a2420u; // Packed render-pass clear color.
        if ((a.color!=b.color || a.id!=b.id || a.depth!=b.depth) && first==reference.samples.size()) first=i;
    }
    Json value={{"samples",reference.samples.size()},{"visible_marker_samples",visible},{"non_background_samples",filled},{"different_colors",colors},{"different_ids",ids},{"different_depths",depths}};
    if (first<reference.samples.size()) {
        const auto& a=reference.samples[first]; const auto& b=actual.samples[first];
        value["first_difference"]={{"index",first},{"reference",{a.color,a.id,a.depth}},{"actual",{b.color,b.id,b.depth}}};
    }
    return value;
}
woby::Mesh fixture(bool multipleGroups=false) {
    woby::Mesh mesh; auto& geometry=woby::editMeshGeometry(mesh);
    for (uint32_t y=0;y<67;++y) for (uint32_t x=0;x<67;++x) {
        woby::Vertex v; v.position={float(x)/30-1.1f,float(y)/30-1.1f,.15f+float((x*17+y*7)%19)/40}; v.normal={0,0,1};
        geometry.vertices.push_back(v); geometry.vertexColors.push_back((x+y)%3?0xff000000u|(x*3)|((y*3)<<8):0);
    }
    for (uint32_t y=0;y<66;++y) for (uint32_t x=0;x<66;++x) {
        const uint32_t a=y*67+x,b=a+1,c=a+67,d=c+1; geometry.indices.insert(geometry.indices.end(),{a,b,c,b,d,c});
    }
    // A large front surface, plus exact depth ties and near-plane crossings.
    for (const auto position:std::array<std::array<float,3>,6>{{{-.8f,-.9f,.7f},{.9f,-.9f,.7f},{0,.9f,.7f},
            {-.8f,-.9f,.7f},{-.4f,.4f,-.05f},{1.2f,.5f,.8f}}}) {
        woby::Vertex v; v.position=position; v.normal={0,0,1}; geometry.vertices.push_back(v); geometry.vertexColors.push_back(0);
    }
    const uint32_t start=67*67; geometry.indices.insert(geometry.indices.end(),{start,start+1,start+2,start+3,start+4,start+5});
    geometry.nodes={{"fixture",0,static_cast<uint32_t>(geometry.indices.size())}};
    if (multipleGroups) {
        const auto grid=static_cast<uint32_t>(geometry.indices.size())-6;
        geometry.nodes={{"grid",0,grid},{"front",grid,3},{"clipped",grid+3,3}};
        // Exercise exact float-bit compression as well as the wide-range
        // fallback. These points have distinct source IDs at nearly equal depth.
        const auto denseOffset=static_cast<uint32_t>(geometry.indices.size());
        for (uint32_t i=0;i<4098;++i) {
            woby::Vertex v; v.position={.2f+float(i%64)*1e-6f,.2f+float(i/64)*1e-6f,.8f+float(i%7)*1e-7f};
            v.normal={0,0,1}; geometry.indices.push_back(static_cast<uint32_t>(geometry.vertices.size()));
            geometry.vertices.push_back(v); geometry.vertexColors.push_back(0xff1a8ed3u);
        }
        geometry.nodes.push_back({"dense",denseOffset,4098});
    }
    mesh.bounds=woby::calculateBounds(geometry.vertices); return mesh;
}
Matrix cameraProjection(const woby::Bounds& bounds,uint32_t width,uint32_t height,float zoom,float yaw) {
    auto camera=woby::frameCameraBounds(bounds); camera.distance*=zoom; camera.yawRadians+=yaw;
    const auto view=woby::scenePickView(camera,woby::SceneUpAxis::z,bounds,width,height,false,1);
    Matrix result; woby::multiplyMatrices(result.data(),view.view.data(),view.renderProjection.data()); return result;
}
void save(const std::filesystem::path& output,const Json& report) {
    std::ofstream file(output); file<<report.dump(2)<<'\n'; require(bool(file),"Cannot save report");
}
int auditShaders(const std::filesystem::path& configuration,const std::filesystem::path& output) {
    require(configuration.is_absolute() && output.is_absolute(),"Audit paths must be absolute");
    require(!std::filesystem::exists(output),"Audit output already exists");
    std::ifstream input(configuration); const Json config=Json::parse(input);
    const auto frames=config.value("frames",12u),rounds=config.value("rounds",3u),samples=config.value("samples",4u);
    const float zoom=config.value("zoom",1.0f);
    require(frames>0 && frames<=120 && rounds>0 && rounds<=12 && (samples==1 || samples==4)
        && std::isfinite(zoom) && zoom>0,"Invalid audit settings");
    const auto model=config.at("model").get<std::string>();
    require(model=="fixture" || std::filesystem::path(model).is_absolute(),"Model path must be absolute");
    auto source=model=="fixture"?fixture(true):woby::loadObjMesh(model);
    auto prepared=woby::prepareSceneMesh(source,woby::gpuMeshPoints); require(prepared.has_value(),"Preparation canceled");
    if (!prepared->pointCloud) prepared->pointCloud=std::make_shared<const woby::points::Cloud>(woby::points::buildCloud(source,std::move(prepared->pointVertexIndices),prepared->nodeRanges,{}).value());
    const auto bounds=source.bounds;
    const auto variants=config.at("variants"); require(!variants.empty(),"No shader variants");
    const std::filesystem::path reference=variants[0].at("directory").get<std::string>();
    Renderer r; r.samples=samples; r.width=config.value("width",1280u); r.height=config.value("height",687u);
    require(r.width>=1 && r.width<=3840 && r.height>=1 && r.height<=2160,"Audit target must fit 3840 by 2160");
    if (model=="fixture") { r.width=79; r.height=63; r.x=3; r.y=5; }
    const auto splitVariant=[](const auto& v) { return v.at("name")=="split-vertices" || v.at("name")=="split-conditional"; };
    const bool splitVertices=std::any_of(variants.begin(),variants.end(),splitVariant);
    const bool packedPoints=std::any_of(variants.begin(),variants.end(),[](const auto& v) { return v.at("name")=="lossless-points"; });
    const bool packed21=std::any_of(variants.begin(),variants.end(),[](const auto& v) { return v.at("name")=="lossless21-points"; });
    require(!(packedPoints && packed21),"Compare point packing formats in separate campaigns");
    initialize(r,source,*prepared->pointCloud,prepared->nodeRanges,reference,reference,true,splitVertices,packedPoints || packed21,packed21);
    Json report={{"configuration",config},{"device",gpu::get_device_caps(r.device).device_name},
        {"points",r.pointCount},{"triangles",woby::meshGeometry(source).indices.size()/3},{"groups",r.ranges.size()},
        {"width",r.width},{"height",r.height},{"geometry_bytes",r.geometryBytes},{"packed_points",r.packedPointCount},{"packed_formats",r.packedFormats},
        {"validation",Json::array()},{"rounds",Json::array()},
        {"scope","Paired full-source rendering; GPU timestamps include all passes and barriers, exclude capture and readback"}};
    source={}; prepared.reset();
    using GraphicsKey=std::tuple<std::vector<uint32_t>,std::vector<uint32_t>,gpu::PrimitiveTopology,int>;
    std::map<GraphicsKey,gpu::PSO*> graphicsCache;
    std::map<std::vector<uint32_t>,gpu::PSO*> computeCache;
    const auto makeGraphics=[&](const std::filesystem::path& directory,const char* vs,const char* fs,gpu::PrimitiveTopology topology=gpu::PrimitiveTopology::triangles,int transparency=0) {
        auto key=GraphicsKey{shader(directory,vs),shader(directory,fs),topology,transparency};
        if (const auto found=graphicsCache.find(key);found!=graphicsCache.end()) return found->second;
        auto* result=graphics(r,directory,vs,directory,fs,topology,transparency);
        r.auditPrograms.push_back(result); graphicsCache.emplace(std::move(key),result); return result;
    };
    const auto makeCompute=[&](const std::filesystem::path& directory,const char* entry) {
        auto key=shader(directory,entry);
        if (const auto found=computeCache.find(key);found!=computeCache.end()) return found->second;
        auto* result=compute(r,directory,entry); r.auditPrograms.push_back(result); computeCache.emplace(std::move(key),result); return result;
    };
    bool valid=true;
    for (const auto& workload:config.at("workloads")) {
        const auto kind=workload.at("kind").get<std::string>(); const float size=workload.value("size",4.0f);
        require(size>=1 && size<=40,"Invalid marker size");
        require(kind=="solid" || kind=="edges" || kind=="combined" || kind=="points" || kind=="pulled"
            || kind=="lines" || kind=="quads" || kind=="uv" || kind=="comparison" || kind=="line-sprites"
            || kind=="transparent" || kind=="transparent-edges" || kind=="transparent-pulled","Unknown audit workload");
        std::vector<AuditDraw> draws;
        for (const auto& variant:variants) {
            std::cout<<"audit prepare "<<kind<<" "<<variant.at("name").get<std::string>()<<std::endl;
            const std::filesystem::path directory=variant.at("directory").get<std::string>();
            require(directory.is_absolute(),"Shader directory must be absolute");
            const bool transparent=kind.starts_with("transparent");
            const char* vs=kind=="pulled" || kind=="transparent-pulled"?"vs_mesh_edges":kind=="lines"?"vs_triangle_lines":kind=="quads"?"vs_marker_point"
                :kind=="line-sprites"?"vs_line_sprite"
                :kind=="comparison"?"vs_comparison":"vs_mesh";
            const char* fs=kind=="pulled"?"fs_marker_mesh_edges_pulled":kind=="lines"?"fs_marker_line":kind=="quads"?"fs_marker_point"
                :kind=="line-sprites"?"fs_marker_line":kind=="transparent"?"fs_transparent_mesh":kind=="transparent-edges"?"fs_transparent_edges"
                :kind=="transparent-pulled"?"fs_transparent_edges_pulled"
                :kind=="comparison"?"fs_marker_comparison":kind=="solid" || kind=="uv"?"fs_marker_mesh":"fs_marker_mesh_edges";
            const auto topology=kind=="lines"?gpu::PrimitiveTopology::lines:kind=="quads" || kind=="line-sprites"?gpu::PrimitiveTopology::triangle_strip:gpu::PrimitiveTopology::triangles;
            draws.push_back({kind,makeGraphics(directory,vs,fs,topology,transparent?1:0),makeCompute(directory,"cs_opaque_clear"),
                makeCompute(directory,"cs_opaque_raster"),transparent?makeGraphics(directory,"vs_marker_screen",samples==4?"fs_transparency_resolve_msaa":"fs_transparency_resolve_single",gpu::PrimitiveTopology::triangle_strip,2)
                    :makeGraphics(directory,"vs_opaque_resolve","fs_opaque_resolve")});
            draws.back().splitVertices=splitVariant(variant);
            draws.back().packedPoints=variant.at("name")=="lossless-points" || variant.at("name")=="lossless21-points";
        }
        const auto draw=[&](size_t variant,float angle,bool capture) {
            auto camera=woby::frameCameraBounds(bounds); camera.distance*=zoom; camera.yawRadians+=angle;
            const auto depth=woby::cameraDepthRange(camera,bounds,woby::SceneUpAxis::z);
            draws[variant].nearPlane=depth.nearPlane; draws[variant].farPlane=depth.farPlane;
            const auto projection=model=="fixture" && angle==0?identity():cameraProjection(bounds,r.width,r.height,zoom,angle);
            return render(r,Mode::production,projection,size,capture,true,&draws[variant]);
        };
        for (const float angle:{0.0f,.31f}) {
            std::cout<<"audit validate "<<kind<<" yaw="<<angle<<std::endl;
            const auto expected=draw(0,angle,true);
            for (size_t i=0;i<draws.size();++i) {
                std::cout<<"audit compare "<<variants[i].at("name").get<std::string>()<<std::endl;
                auto row=compare(expected,draw(i,angle,true));
                row.update({{"variant",variants[i].at("name")},{"kind",kind},{"point_size",size},{"yaw",angle}});
                valid=valid && row["different_colors"]==0 && row["different_ids"]==0 && row["different_depths"]==0;
                report["validation"].push_back(row);
                if (row["different_colors"]!=0 || row["different_ids"]!=0 || row["different_depths"]!=0) std::cout<<row.dump()<<std::endl;
            }
        }
        for (uint32_t round=0;round<rounds;++round) {
            std::vector<std::vector<double>> times(draws.size());
            for (int frame=-4;frame<static_cast<int>(frames);++frame) {
                const float angle=.105f*std::sin(float(frame)*2*std::numbers::pi_v<float>/12);
                for (size_t position=0;position<draws.size();++position) {
                    const auto i=(position+static_cast<uint32_t>(frame+4)+round)%draws.size();
                    const auto result=draw(i,angle,false);
                    if (frame>=0) times[i].push_back(result.gpuMs);
                }
            }
            for (size_t i=0;i<draws.size();++i) {
                auto sorted=times[i]; std::sort(sorted.begin(),sorted.end());
                Json row={{"variant",variants[i].at("name")},{"kind",kind},{"point_size",size},{"round",round},
                    {"gpu_ms",times[i]},{"median_gpu_ms",sorted[sorted.size()/2]}};
                report["rounds"].push_back(row); std::cout<<row.dump()<<std::endl;
            }
            save(output,report);
        }
    }
    report["exact_samples"]=valid; save(output,report);
    require(valid || config.value("allow_mismatch",false),"Audit changed color, depth, or picking samples");
    return 0;
}
} // namespace

int main(int argc,char** argv) {
    try {
        if (argc==4 && std::string(argv[1])=="--audit") return auditShaders(argv[2],argv[3]);
        const bool selfTest=argc==2 && std::string(argv[1])=="--self-test";
        require(selfTest || argc==8 || (argc==9 && std::string(argv[8])=="paired"),
            "Usage: woby_point_pipeline MODEL REFERENCE_SHADERS OUTPUT FRAMES ROUNDS SAMPLES ZOOM [paired]; or --self-test");
        const bool paired=argc==9;
        const auto methods=paired?std::vector{Mode::reference,Mode::production}:std::vector(modes.begin(),modes.end());
        const auto reference=selfTest?std::filesystem::absolute(argv[0]).parent_path()/"assets/shaders/spirv":std::filesystem::absolute(argv[2]);
        const auto production=std::filesystem::absolute(argv[0]).parent_path()/"assets/shaders/spirv";
        const auto output=selfTest?std::filesystem::path{}:std::filesystem::absolute(argv[3]);
        require(selfTest || !std::filesystem::exists(output),"Output already exists");
        const uint32_t frames=selfTest?1u:static_cast<uint32_t>(std::stoul(argv[4]));
        const uint32_t rounds=selfTest?1u:static_cast<uint32_t>(std::stoul(argv[5]));
        const uint32_t samples=selfTest?1u:static_cast<uint32_t>(std::stoul(argv[6]));
        const float zoom=selfTest?1.0f:std::stof(argv[7]);
        require(frames>=1 && frames<=120 && rounds>=1 && rounds<=12 && (samples==1 || samples==4) && std::isfinite(zoom) && zoom>0,"Invalid run settings");
        auto source=selfTest?fixture():woby::loadObjMesh(std::filesystem::absolute(argv[1]));
        auto prepared=woby::prepareSceneMesh(source,woby::gpuMeshPoints); require(prepared.has_value(),"Preparation canceled");
        if (!prepared->pointCloud) prepared->pointCloud=std::make_shared<const woby::points::Cloud>(woby::points::buildCloud(source,std::move(prepared->pointVertexIndices),prepared->nodeRanges,{}).value());
        const auto bounds=source.bounds;
        Json report={{"model",selfTest?"fixture":std::filesystem::absolute(argv[1]).string()},
            {"points",prepared->pointCloud->points.size()},{"triangles",woby::meshGeometry(source).indices.size()/3},
            {"reference_shaders",reference.string()},{"rounds",Json::array()},{"validation",Json::array()},
            {"paired_frames",paired},
            {"scope","Full-source opaque scene, no UI or presentation; timestamps bracket rendering only"}};
        bool valid=true;
        for (const auto sampleCount:selfTest?std::vector<uint32_t>{1,4}:std::vector<uint32_t>{samples}) {
            Renderer r; r.samples=sampleCount;
            if (selfTest) { r.width=79; r.height=63; r.x=3; r.y=5; }
            initialize(r,source,*prepared->pointCloud,prepared->nodeRanges,reference,production);
            report["device"]=gpu::get_device_caps(r.device).device_name; report["width"]=r.width; report["height"]=r.height;
            report["geometry_bytes"]=r.geometryBytes; report["combined_extra_bytes"]=r.combinedBytes;
            // Release CPU geometry for real workloads after GPU upload.
            if (!selfTest) { source={}; prepared.reset(); }
            for (const float size:{1.0f,4.0f,8.0f}) for (const float angle:{0.0f,.31f}) {
                const auto projection=selfTest && angle==0?identity():cameraProjection(bounds,r.width,r.height,zoom,angle);
                const auto expected=render(r,Mode::reference,projection,size,true);
                for (const auto mode:methods) {
                    if ((mode==Mode::combined || mode==Mode::combinedOnce) && !r.combineSupported) continue;
                    auto row=compare(expected,render(r,mode,projection,size,true));
                    row.update({{"mode",name(mode)},{"point_size",size},{"samples_per_pixel",r.samples},{"yaw",angle},{"zoom",zoom}});
                    valid=valid && row["different_colors"]==0 && row["different_ids"]==0 && row["different_depths"]==0;
                    report["validation"].push_back(row);
                    if (row["different_colors"]!=0 || row["different_ids"]!=0 || row["different_depths"]!=0) std::cout<<row.dump()<<std::endl;
                }
            }
            if (selfTest) continue;
            save(output,report);
            for (uint32_t round=0;round<rounds;++round) for (uint32_t s=0;s<3;++s) {
                const float size=std::array{1.0f,4.0f,8.0f}[(s+round)%3];
                if (paired) {
                    std::array<std::vector<double>,2> times;
                    for (int frame=-4;frame<static_cast<int>(frames);++frame) {
                        const float yaw=.105f*std::sin(float(frame)*2*std::numbers::pi_v<float>/12);
                        const auto projection=cameraProjection(bounds,r.width,r.height,zoom,yaw);
                        for (uint32_t index=0;index<2;++index) {
                            const auto method=(index+static_cast<uint32_t>(frame+4)+round)%2;
                            const auto measured=render(r,methods[method],projection,size,false);
                            if (frame>=0) times[method].push_back(measured.gpuMs);
                        }
                    }
                    for (uint32_t method=0;method<2;++method) {
                        auto sorted=times[method]; std::sort(sorted.begin(),sorted.end());
                        Json row={{"round",round},{"mode",name(methods[method])},{"point_size",size},{"samples_per_pixel",r.samples},
                            {"zoom",zoom},{"gpu_ms",times[method]},{"median_gpu_ms",sorted[sorted.size()/2]}};
                        report["rounds"].push_back(row); std::cout<<row.dump()<<std::endl;
                    }
                    save(output,report); continue;
                }
                for (uint32_t m=0;m<methods.size();++m) {
                    const auto mode=methods[(m+round+s)%methods.size()];
                    if ((mode==Mode::combined || mode==Mode::combinedOnce) && !r.combineSupported) continue;
                    std::vector<double> times;
                    for (int frame=-4;frame<static_cast<int>(frames);++frame) {
                        const float yaw=.105f*std::sin(float(frame)*2*std::numbers::pi_v<float>/12);
                        const auto projection=cameraProjection(bounds,r.width,r.height,zoom,yaw);
                        const auto measured=render(r,mode,projection,size,false);
                        if (frame>=0) times.push_back(measured.gpuMs);
                    }
                    auto sorted=times; std::sort(sorted.begin(),sorted.end()); const auto median=sorted[sorted.size()/2];
                    Json row={{"round",round},{"mode",name(mode)},{"point_size",size},{"samples_per_pixel",r.samples},
                        {"zoom",zoom},{"gpu_ms",times},{"median_gpu_ms",median}};
                    report["rounds"].push_back(row); std::cout<<row.dump()<<std::endl; save(output,report);
                }
            }
        }
        report["exact_samples"]=valid;
        if (!selfTest) save(output,report);
        require(valid,"At least one experimental path changed a color, depth, or picking sample");
        std::cout<<"All sample comparisons passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
