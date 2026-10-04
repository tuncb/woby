#include "gpu_culling.h"
#include "renderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace woby::overlay {
namespace {
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
uint32_t blocksFor(uint32_t count) { return count/256+(count%256!=0); }
void allocate(Renderer& r,gpu::GpuHeap& heap,uint64_t bytes,gpu::MemoryType type=gpu::MemoryType::gpu_only) {
    heap=gpu::create_gpu_heap(r.device,std::max(uint64_t{16},bytes),type);
    require(heap.owner!=nullptr,heap.error);
    r.culling.residentBytes+=std::max(uint64_t{16},bytes);
}
void releaseBuffers(GpuCulling& c) {
    for (auto* heap:{&c.depth,&c.groups,&c.blocks,&c.ranks,&c.selected,&c.arguments,
            &c.argumentReadback,&c.selectionReadback,&c.finalSum}) {
        gpu::destroy_gpu_heap(*heap); *heap={};
    }
    for (auto& level:c.levels) { gpu::destroy_gpu_heap(level.values); gpu::destroy_gpu_heap(level.offsets); }
    c.levels.clear(); c.residentBytes=0; c.inputValid=false;
}
gpu::PSO* pipeline(Renderer& r,const char* entry) {
    std::ifstream stream(std::filesystem::path(WOBY_OVERLAY_SHADER_DIR)/(std::string(entry)+".spv"),std::ios::binary|std::ios::ate);
    require(bool(stream),"Cannot open culling shader");
    const auto size=static_cast<size_t>(stream.tellg());
    require(size && size%4==0,"Invalid culling shader");
    std::vector<uint32_t> code(size/4); stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()),static_cast<std::streamsize>(size));
    require(bool(stream),"Cannot read culling shader");
    auto* pso=gpu::create_compute_pso(r.device,{.code={reinterpret_cast<const gpu::byte*>(code.data()),size},.entry_point=entry});
    require(pso!=nullptr,"Cannot create culling pipeline"); return pso;
}
void computeBarrier(gpu::CommandBuffer* commands) {
    gpu::barrier(commands,gpu::Stage::compute,gpu::Access::shader_write,
        gpu::Stage::compute,gpu::Access::shader_read|gpu::Access::shader_write);
}
void dispatch(Renderer& r,gpu::CommandBuffer* commands,gpu::PSO* pipeline,CullRoot root,uint32_t blocks) {
    if (!blocks) return;
    root.dispatchWidth=std::min(blocks,1024u);
    auto argument=r.arena->allocate<CullRoot>(); require(argument.cpu!=nullptr,"Culling root arena exhausted");
    *argument.cpu=root; gpu::bind_pso(commands,pipeline);
    gpu::dispatch(commands,argument.gpu,{root.dispatchWidth,(blocks+root.dispatchWidth-1)/root.dispatchWidth,1});
}
void dispatchImage(Renderer& r,gpu::CommandBuffer* commands,gpu::PSO* pipeline,const CullRoot& root) {
    const auto& mip=root.mips[root.level];
    auto argument=r.arena->allocate<CullRoot>(); require(argument.cpu!=nullptr,"Culling root arena exhausted");
    *argument.cpu=root; gpu::bind_pso(commands,pipeline);
    gpu::dispatch(commands,argument.gpu,{(mip.width+7)/8,(mip.height+7)/8,1});
}
} // namespace

void prepareGpuCulling(Renderer& r,bool readSelection) {
    auto& c=r.culling;
    if (!c.depthPass) c.depthPass=pipeline(r,"cs_cull_depth");
    if (!c.reduce) c.reduce=pipeline(r,"cs_cull_reduce");
    if (!c.classify) c.classify=pipeline(r,"cs_cull_classify");
    if (!c.scan) c.scan=pipeline(r,"cs_cull_scan");
    if (!c.add) c.add=pipeline(r,"cs_cull_add");
    if (!c.argumentsPass) c.argumentsPass=pipeline(r,"cs_cull_arguments");
    if (!c.scatter) c.scatter=pipeline(r,"cs_cull_scatter");
    bool changed=c.width!=r.options.width || c.height!=r.options.height || c.ranges.size()!=r.scene.groups.size();
    if (!changed) for (size_t i=0;i<c.ranges.size();++i) {
        const auto& range=r.scene.groups[i].range;
        if (c.ranges[i]!=std::array{range.pointIndexOffset,range.pointIndexCount}) { changed=true; break; }
    }
    if (changed) {
        releaseBuffers(c); c.ranges.clear(); c.layout.clear(); c.inputs.clear();
        c.width=r.options.width; c.height=r.options.height; c.capacity=0; c.blockCount=0;
        std::vector<CullBlock> blocks;
        for (const auto& group:r.scene.groups) {
            const auto& range=group.range;
            require(uint64_t(range.pointIndexOffset)+range.pointIndexCount<=r.scene.markerCount,"Invalid GPU marker range");
            require(uint64_t(c.capacity)+range.pointIndexCount<=std::numeric_limits<uint32_t>::max(),"GPU marker capacity exceeds uint32");
            CullGroup input{};
            input.pointOffset=range.pointIndexOffset; input.pointCount=range.pointIndexCount;
            input.outputOffset=c.capacity; input.firstBlock=c.blockCount; input.blockCount=blocksFor(input.pointCount);
            const auto groupIndex=static_cast<uint32_t>(c.layout.size());
            for (uint32_t i=0;i<input.blockCount;++i) blocks.push_back({groupIndex,i*256});
            c.capacity+=input.pointCount; c.blockCount+=input.blockCount;
            c.ranges.push_back({input.pointOffset,input.pointCount}); c.layout.push_back(input);
        }
        uint32_t width=c.width,height=c.height,offset=0; c.mipCount=0;
        do {
            require(c.mipCount<c.mips.size(),"Too many culling depth mip levels");
            c.mips[c.mipCount++]={offset,width,height,0}; offset+=width*height;
            if (width==1 && height==1) break;
            width=(width+1)/2; height=(height+1)/2;
        } while (true);
        allocate(r,c.depth,uint64_t(offset)*4);
        // Mapped immutable block descriptors and cached group matrices; no CPU per-marker work per frame.
        allocate(r,c.groups,c.layout.size()*sizeof(CullGroup),gpu::MemoryType::cpu_visible);
        allocate(r,c.blocks,blocks.size()*sizeof(CullBlock),gpu::MemoryType::cpu_visible);
        require(c.groups.range.cpu && c.blocks.range.cpu,"Culling metadata is not mapped");
        if (!blocks.empty()) std::memcpy(c.blocks.range.cpu,blocks.data(),blocks.size()*sizeof(CullBlock));
        allocate(r,c.ranks,uint64_t(c.capacity)*4); allocate(r,c.selected,uint64_t(c.capacity)*4);
        allocate(r,c.arguments,c.layout.size()*16); allocate(r,c.argumentReadback,c.layout.size()*16,gpu::MemoryType::readback);
        allocate(r,c.finalSum,4);
        for (uint32_t count=c.blockCount;count;) {
            c.levels.push_back({count,{},{}}); auto& level=c.levels.back();
            allocate(r,level.values,uint64_t(count)*4); allocate(r,level.offsets,uint64_t(count)*4);
            if (count<=256) break;
            count=blocksFor(count);
        }
    }
    if (readSelection && !c.selectionReadback.owner) allocate(r,c.selectionReadback,uint64_t(c.capacity)*4,gpu::MemoryType::readback);
}
void updateGpuCullInputs(Renderer& r,const std::array<float,16>& projection) {
    auto& c=r.culling;
    bool changed=!c.inputValid || c.projection!=projection || c.inputs.size()!=r.scene.groups.size();
    if (!changed) for (size_t i=0;i<c.inputs.size();++i)
        if (c.inputs[i]!=CullGroupInput{r.scene.groups[i].model,r.scene.groups[i].points}) { changed=true; break; }
    if (!changed) return;
    c.inputs.clear(); c.projection=projection;
    for (size_t i=0;i<c.layout.size();++i) {
        const auto& group=r.scene.groups[i]; auto& input=c.layout[i];
        float mvp[16]; bx::mtxMul(mvp,group.model.data(),projection.data());
        for (size_t row=0;row<4;++row) for (size_t col=0;col<4;++col) input.modelViewProj.rows[row][col]=mvp[col*4+row];
        input.enabled=group.points?1u:0u; c.inputs.push_back({group.model,group.points});
    }
    if (!c.layout.empty()) std::memcpy(c.groups.range.cpu,c.layout.data(),c.layout.size()*sizeof(CullGroup));
    c.inputValid=true;
}
void submitGpuCulling(Renderer& r,gpu::CommandBuffer* commands,const Display& display,bool opaqueDepth) {
    auto& c=r.culling; c.timestamps={};
    gpu::barrier(commands,gpu::Stage::all_commands,gpu::Access::depth_stencil_write|gpu::Access::color_write|gpu::Access::shader_write,
        gpu::Stage::compute|gpu::Stage::color_output|gpu::Stage::depth_stencil_tests,
        gpu::Access::shader_read|gpu::Access::shader_write|gpu::Access::color_read|gpu::Access::color_write|gpu::Access::depth_stencil_read|gpu::Access::depth_stencil_write);
    gpu::write_timestamp(commands,&c.timestamps[0]);
    CullRoot root{};
    root.vertices=reinterpret_cast<float*>(r.scene.vertices.range.gpu); root.points=reinterpret_cast<uint32_t*>(r.scene.points.range.gpu);
    root.groups=reinterpret_cast<CullGroup*>(c.groups.range.gpu); root.blocks=reinterpret_cast<CullBlock*>(c.blocks.range.gpu);
    root.depth=reinterpret_cast<float*>(c.depth.range.gpu); root.ranks=reinterpret_cast<uint32_t*>(c.ranks.range.gpu);
    root.selected=reinterpret_cast<uint32_t*>(c.selected.range.gpu); root.arguments=reinterpret_cast<uint32_t*>(c.arguments.range.gpu);
    if (!c.levels.empty()) {
        root.counts=reinterpret_cast<uint32_t*>(c.levels[0].values.range.gpu);
        root.offsets=reinterpret_cast<uint32_t*>(c.levels[0].offsets.range.gpu);
    }
    std::copy(c.mips.begin(),c.mips.end(),root.mips);
    root.width=c.width; root.height=c.height; root.samples=r.options.samples; root.depthTexture=1;
    root.stride=sizeof(Vertex)/4; root.blockCount=c.blockCount; root.groupCount=static_cast<uint32_t>(c.layout.size());
    root.readDepth=display.culling==Culling::footprint && opaqueDepth?1u:0u; root.pointSize=display.pointSize;
    if (root.readDepth) {
        dispatchImage(r,commands,c.depthPass,root); computeBarrier(commands);
        const auto maxLevel=std::min(c.mipCount-1,static_cast<uint32_t>(std::ceil(std::log2(display.pointSize+3))));
        for (uint32_t level=1;level<=maxLevel;++level) {
            root.level=level; dispatchImage(r,commands,c.reduce,root); computeBarrier(commands);
        }
    }
    gpu::write_timestamp(commands,&c.timestamps[1]);
    dispatch(r,commands,c.classify,root,c.blockCount); computeBarrier(commands);
    gpu::write_timestamp(commands,&c.timestamps[2]);
    for (size_t i=0;i<c.levels.size();++i) {
        const auto& level=c.levels[i]; root.count=level.count;
        root.scanInput=reinterpret_cast<uint32_t*>(level.values.range.gpu);
        root.scanOutput=reinterpret_cast<uint32_t*>(level.offsets.range.gpu);
        root.scanSums=reinterpret_cast<uint32_t*>((i+1<c.levels.size()?c.levels[i+1].values:c.finalSum).range.gpu);
        dispatch(r,commands,c.scan,root,blocksFor(level.count)); computeBarrier(commands);
    }
    for (size_t i=c.levels.size();i>1;--i) {
        const auto& level=c.levels[i-2]; root.count=level.count;
        root.scanOutput=reinterpret_cast<uint32_t*>(level.offsets.range.gpu);
        root.scanParent=reinterpret_cast<uint32_t*>(c.levels[i-1].offsets.range.gpu);
        dispatch(r,commands,c.add,root,blocksFor(level.count)); computeBarrier(commands);
    }
    dispatch(r,commands,c.argumentsPass,root,blocksFor(root.groupCount));
    gpu::write_timestamp(commands,&c.timestamps[3]);
    dispatch(r,commands,c.scatter,root,c.blockCount);
    gpu::barrier(commands,gpu::Stage::compute,gpu::Access::shader_write|gpu::Access::shader_read,
        gpu::Stage::indirect|gpu::Stage::vertex|gpu::Stage::depth_stencil_tests,
        gpu::Access::indirect_read|gpu::Access::shader_read|gpu::Access::depth_stencil_read|gpu::Access::depth_stencil_write);
    gpu::write_timestamp(commands,&c.timestamps[4]);
}
void captureGpuCulling(Renderer& r,gpu::CommandBuffer* commands,bool fullSelection) {
    const auto& c=r.culling;
    gpu::barrier(commands,gpu::Stage::compute,gpu::Access::shader_write,gpu::Stage::transfer,gpu::Access::transfer_read);
    const uint64_t bytes=c.layout.size()*16;
    if (bytes) gpu::copy_memory(commands,{c.arguments.range.gpu,bytes},{c.argumentReadback.range.gpu,bytes});
    if (fullSelection && c.capacity) gpu::copy_memory(commands,{c.selected.range.gpu,uint64_t(c.capacity)*4},
        {c.selectionReadback.range.gpu,uint64_t(c.capacity)*4});
}
void readGpuCulling(Renderer& r,Capture& capture) {
    const auto& c=r.culling;
    capture.cullingCounts.clear(); capture.cullingSelection.clear();
    for (size_t i=0;i<c.layout.size();++i) {
        uint32_t count=0; std::memcpy(&count,c.argumentReadback.range.cpu+i*16+4,4);
        require(count<=c.layout[i].pointCount,"GPU compaction count exceeds input range");
        capture.cullingCounts.push_back(count);
        if (capture.cullingSelectionRequested && count) {
            const auto begin=capture.cullingSelection.size(); capture.cullingSelection.resize(begin+count);
            std::memcpy(capture.cullingSelection.data()+begin,c.selectionReadback.range.cpu+uint64_t(c.layout[i].outputOffset)*4,uint64_t(count)*4);
        }
    }
}
void destroyGpuCulling(GpuCulling& c) {
    releaseBuffers(c);
    for (auto* pso:{c.depthPass,c.reduce,c.classify,c.scan,c.add,c.argumentsPass,c.scatter}) gpu::destroy_pso(pso);
    c={};
}
} // namespace woby::overlay
