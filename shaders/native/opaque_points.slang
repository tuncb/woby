#include <NoGraphicsAPI/shader.slang>
#include "opaque_points.h"
GPU_ROOT(OpaquePointRoot, cloud);
// Slang's generic InterlockedMax currently emits the value-returning Metal
// overload even when its result is unused. Metal's ulong max returns void.
void pointAtomicMax64(uint64* address, uint64 value) {
    __target_switch {
    case metal:
        __intrinsic_asm "atomic_max_explicit((device atomic_ulong*)($0), $1, memory_order_relaxed)";
    default:
        InterlockedMax(*address,value);
    }
}
void pointAtomicMax32(__ref uint destination, uint value) {
    __target_switch {
    case metal:
        __intrinsic_asm "(void)atomic_fetch_max_explicit((device atomic_uint*)(&($0)), $1, memory_order_relaxed)";
    default:
        InterlockedMax(destination,value);
    }
}
float2 samplePosition(uint sample) {
    if (cloud.samples==1) return float2(.5,.5);
    const float2 positions[4]={float2(.375,.125),float2(.875,.375),float2(.125,.625),float2(.625,.875)};
    return positions[sample];
}
[numthreads(8,8,1)] void cs_opaque_clear(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    for (uint sample=0;sample<cloud.samples;++sample)
        cloud.winners[(thread.y*cloud.width+thread.x)*cloud.samples+min(sample,cloud.samples-1)]=0;
}
[numthreads(8,8,1)] void cs_opaque_batch_clear(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    for (uint sample=0;sample<cloud.samples;++sample) {
        uint index=(thread.y*cloud.width+thread.x)*cloud.samples+sample;
        cloud.batch[index].depth=0;
        cloud.batch[index].id=0;
    }
}
[numthreads(8,8,1)] void cs_opaque_merge(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    for (uint sample=0;sample<cloud.samples;++sample) {
        uint index=(thread.y*cloud.width+thread.x)*cloud.samples+sample;
        OpaquePointBatchSample winner=cloud.batch[index];
        // One invocation owns this sample. No 64-bit atomic is needed to merge
        // a completed batch into the persistent lexicographic depth/ID winner.
        uint64 key=(uint64(winner.depth)<<32)|uint64(winner.id);
        cloud.winners[index]=max(cloud.winners[index],key);
    }
}
[ForceInline] void rasterPoints(uint3 dispatch,uint lane,bool ids) {
    uint taskIndex=dispatch.y*cloud.dispatchWidth+dispatch.x;
    if (taskIndex>=cloud.taskCount) return;
    OpaquePointTaskData task=cloud.tasks[taskIndex];
    OpaquePointGroupData group=cloud.groups[task.group];
    for (uint i=lane;i<task.count;i+=128) {
        OpaquePoint p=task.points[i];
        float4 clip=mul(group.matrix,float4(p.position,1));
        if (!all(isfinite(clip)) || clip.w<=0 || clip.z<0 || clip.z>clip.w) continue;
        float2 center=float2(clip.x/clip.w*.5+.5,.5-clip.y/clip.w*.5)*float2(cloud.width,cloud.height);
        float radius=group.pointSize*.5;
        if (any(center+radius<0) || any(center-radius>=float2(cloud.width,cloud.height))) continue;
        int2 lo=int2(max(float2(0),floor(center-radius)));
        int2 hi=int2(min(float2(cloud.width-1,cloud.height-1),floor(center+radius)));
        if (task.query!=0) { lo=max(lo,int2(cloud.query.xy)); hi=min(hi,int2(cloud.query.zw)-1); }
        uint id=group.firstId+p.id-group.sourceFirstId;
        uint depth=asuint(max(clip.z/clip.w,0.0));
        for (int y=lo.y;y<=hi.y;++y) for (int x=lo.x;x<=hi.x;++x) for (uint sample=0;sample<cloud.samples;++sample) {
            float2 delta=float2(x,y)+samplePosition(sample)-center;
            if (dot(delta,delta)>radius*radius) continue;
            uint index=(uint(y)*cloud.width+uint(x))*cloud.samples+min(sample,cloud.samples-1);
#ifdef WOBY_POINT_ATOMIC64
            pointAtomicMax64(cloud.winners+index,(uint64(depth)<<32)|uint64(id));
#else
            if (!ids) pointAtomicMax32(cloud.batch[index].depth,depth);
            else if (cloud.batch[index].depth==depth) pointAtomicMax32(cloud.batch[index].id,id);
#endif
        }
    }
}
#ifdef WOBY_POINT_ATOMIC64
[numthreads(128,1,1)] void cs_opaque_raster(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    rasterPoints(dispatch,lane,false);
}
#else
[numthreads(128,1,1)] void cs_opaque_depth(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    rasterPoints(dispatch,lane,false);
}
[numthreads(128,1,1)] void cs_opaque_ids(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    rasterPoints(dispatch,lane,true);
}
#endif
struct ResolveVertex { float4 position:SV_Position; };
ResolveVertex vs_opaque_resolve(uint vertex:SV_VertexID) {
    ResolveVertex v; v.position=float4(vertex==1?3.0:-1.0,vertex==2?3.0:-1.0,0,1); return v;
}
struct ResolveOutput { float4 color:SV_Target0; float4 id:SV_Target1; float depth:SV_Depth; };
ResolveOutput fs_opaque_resolve(ResolveVertex input,uint sample:SV_SampleIndex) {
    uint2 relative=uint2(input.position.xy)-uint2(cloud.x,cloud.y);
    uint2 pixel=min(relative,uint2(cloud.width-1,cloud.height-1));
    // Helper lanes may execute outside a non-aligned viewport. Bound the address
    // even for discarded lanes; a discard alone does not guard a physical load.
    if (any(relative>=uint2(cloud.width,cloud.height))) { discard; return (ResolveOutput)0; }
    uint64 key=cloud.winners[(pixel.y*cloud.width+pixel.x)*cloud.samples+min(sample,cloud.samples-1)];
    uint id=uint(key);
    if (id==0) { discard; return (ResolveOutput)0; }
    uint lo=0,hi=cloud.groupCount;
    while (lo<hi) { uint mid=lo+(hi-lo)/2; if (cloud.groups[mid].endId<=id) lo=mid+1; else hi=mid; }
    if (lo>=cloud.groupCount) { discard; return (ResolveOutput)0; }
    ResolveOutput o;
    o.color=cloud.groups[min(lo,cloud.groupCount-1)].color;
    o.id=float4(float(id&255),float((id>>8)&255),float((id>>16)&255),float(id>>24))/255;
    o.depth=asfloat(uint(key>>32)); return o;
}
struct ColorResolveOutput { float4 color:SV_Target0; float depth:SV_Depth; };
ColorResolveOutput fs_opaque_color(ResolveVertex input,uint sample:SV_SampleIndex) {
    const ResolveOutput resolved=fs_opaque_resolve(input,sample);
    ColorResolveOutput output; output.color=resolved.color; output.depth=resolved.depth; return output;
}
