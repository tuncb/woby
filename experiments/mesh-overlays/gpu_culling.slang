#include <NoGraphicsAPI/shader.slang>
#include "gpu_culling_root.h"
GPU_ROOT(CullRoot,cull);

uint linearGroup(uint3 group) { return group.y*cull.dispatchWidth+group.x; }
groupshared uint prefix[256];
// Inclusive ordered scan; no subgroup-width assumption or global append atomic.
uint inclusivePrefix(uint value,uint lane) {
    prefix[lane]=value;
    GroupMemoryBarrierWithGroupSync();
    for (uint step=1;step<256;step*=2) {
        uint earlier=0;
        if (lane>=step) earlier=prefix[lane-step];
        GroupMemoryBarrierWithGroupSync();
        prefix[lane]+=earlier;
        GroupMemoryBarrierWithGroupSync();
    }
    return prefix[lane];
}

[numthreads(8,8,1)] void cs_cull_depth(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cull.width,cull.height))) return;
    float minimum=1;
    for (uint sample=0;sample<cull.samples;++sample) {
        float depth=cull.samples>1
            ? gpu_texture<Texture2DMS<float>>(cull.depthTexture).Load(int2(thread.xy),sample)
            : gpu_texture<Texture2D<float>>(cull.depthTexture).Load(int3(thread.xy,0));
        minimum=min(minimum,depth);
    }
    cull.depth[thread.y*cull.width+thread.x]=minimum;
}
[numthreads(8,8,1)] void cs_cull_reduce(uint3 thread:SV_DispatchThreadID) {
    CullMip source=cull.mips[cull.level-1], target=cull.mips[cull.level];
    if (any(thread.xy>=uint2(target.width,target.height))) return;
    float minimum=1;
    for (uint y=0;y<2;++y) for (uint x=0;x<2;++x) {
        uint2 p=thread.xy*2+uint2(x,y);
        float value=0;
        if (all(p<uint2(source.width,source.height))) value=cull.depth[source.offset+p.y*source.width+p.x];
        minimum=min(minimum,value);
    }
    cull.depth[target.offset+thread.y*target.width+thread.x]=minimum;
}
bool keepMarker(CullGroup group,uint local) {
    uint vertex=cull.points[group.pointOffset+local]*cull.stride;
    float3 position=float3(cull.vertices[vertex],cull.vertices[vertex+1],cull.vertices[vertex+2]);
    float4 clip=mul(group.modelViewProj,float4(position,1));
    // Invalid/uncertain math is retained rather than becoming an occlusion claim.
    if (!all(isfinite(clip))) return true;
    if (clip.w<=0) return false;
    float z=clip.z/clip.w;
    float tolerance=max(1e-6,abs(z)*1e-5);
    if (z < -tolerance || z > 1+tolerance) return false;
    float2 center=float2(clip.x/clip.w*.5+.5,.5-clip.y/clip.w*.5)*float2(cull.width,cull.height);
    if (!all(isfinite(center)) || !isfinite(z)) return true;
    float margin=cull.pointSize*.5+.5;
    if (any(center+margin<0) || any(center-margin>=float2(cull.width,cull.height))) return false;
    if (cull.readDepth==0) return true;
    uint2 low=uint2(max(float2(0),floor(center-margin)));
    uint2 high=uint2(clamp(ceil(center+margin),float2(0),float2(cull.width-1,cull.height-1)));
    uint extent=max(high.x-low.x+1,high.y-low.y+1), level=0;
    while ((1u<<level)<extent) ++level;
    CullMip mip=cull.mips[level];
    low>>=level; high>>=level;
    float minimum=min(min(cull.depth[mip.offset+low.y*mip.width+low.x],cull.depth[mip.offset+low.y*mip.width+high.x]),
        min(cull.depth[mip.offset+high.y*mip.width+low.x],cull.depth[mip.offset+high.y*mip.width+high.x]));
    return z+tolerance>=minimum;
}
[numthreads(256,1,1)] void cs_cull_classify(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    uint index=linearGroup(dispatch);
    if (index>=cull.blockCount) return;
    CullBlock block=cull.blocks[index]; CullGroup group=cull.groups[block.group];
    if (group.enabled==0) { if (lane==0) cull.counts[index]=0; return; }
    uint local=block.localOffset+lane;
    bool retained=false;
    if (local<group.pointCount) retained=keepMarker(group,local);
    uint sum=inclusivePrefix(retained?1:0,lane);
    if (local<group.pointCount) cull.ranks[group.outputOffset+local]=retained?sum:0;
    if (lane==255) cull.counts[index]=sum;
}
[numthreads(256,1,1)] void cs_cull_scan(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    uint block=linearGroup(dispatch), index=block*256+lane;
    if (block*256>=cull.count) return;
    uint value=0;
    if (index<cull.count) value=cull.scanInput[index];
    uint sum=inclusivePrefix(value,lane);
    if (index<cull.count) cull.scanOutput[index]=sum-value;
    if (lane==255) cull.scanSums[block]=sum;
}
[numthreads(256,1,1)] void cs_cull_add(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    uint index=linearGroup(dispatch)*256+lane;
    if (index<cull.count) cull.scanOutput[index]+=cull.scanParent[index/256];
}
[numthreads(256,1,1)] void cs_cull_arguments(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    uint index=linearGroup(dispatch)*256+lane;
    if (index>=cull.groupCount) return;
    CullGroup group=cull.groups[index]; uint count=0;
    if (group.blockCount>0) {
        uint last=group.firstBlock+group.blockCount-1;
        count=cull.offsets[last]+cull.counts[last]-cull.offsets[group.firstBlock];
    }
    cull.arguments[index*4]=4;
    cull.arguments[index*4+1]=count;
    cull.arguments[index*4+2]=0;
    cull.arguments[index*4+3]=0;
}
[numthreads(256,1,1)] void cs_cull_scatter(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    uint index=linearGroup(dispatch);
    if (index>=cull.blockCount) return;
    CullBlock block=cull.blocks[index]; CullGroup group=cull.groups[block.group];
    uint local=block.localOffset+lane;
    if (group.enabled==0 || local>=group.pointCount) return;
    uint rank=cull.ranks[group.outputOffset+local];
    if (rank!=0) {
        uint offset=cull.offsets[index]-cull.offsets[group.firstBlock]+rank-1;
        cull.selected[group.outputOffset+offset]=group.pointOffset+local;
    }
}
