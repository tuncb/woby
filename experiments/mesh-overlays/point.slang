#include <NoGraphicsAPI/shader.slang>
#include "point_root.h"
GPU_ROOT(PointRoot,cloud);

// Vulkan standard sample locations. This experiment supports 1x and 4x only.
float2 samplePosition(uint sample) {
    if (cloud.samples==1) return float2(.5,.5);
    const float2 positions[4]={float2(.375,.125),float2(.875,.375),float2(.125,.625),float2(.625,.875)};
    return positions[sample];
}
[numthreads(8,8,1)] void cs_point_clear(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    for (uint sample=0;sample<cloud.samples;++sample) {
        float z=0;
        if (cloud.readDepth!=0) z=cloud.samples==1
            ?gpu_texture<Texture2D<float>>(cloud.depthTexture).Load(int3(thread.xy,0))
            :gpu_texture<Texture2DMS<float>>(cloud.depthTexture).Load(int2(thread.xy),sample);
        cloud.winners[(thread.y*cloud.width+thread.x)*cloud.samples+sample]=uint64(asuint(z))<<32;
    }
}
[numthreads(128,1,1)] void cs_point_raster(uint3 dispatch:SV_GroupID,uint lane:SV_GroupIndex) {
    uint taskIndex=dispatch.y*cloud.dispatchWidth+dispatch.x;
    if (taskIndex>=cloud.taskCount) return;
    CloudTask task=cloud.tasks[taskIndex]; CloudGroup group=cloud.groups[task.group];
    if (group.enabled==0) return;
    for (uint i=lane;i<task.count;i+=128) {
        CloudPoint p=task.points[i];
        float4 clip=mul(group.matrix,float4(p.position,1));
        if (!all(isfinite(clip)) || clip.w<=0 || clip.z<0 || clip.z>clip.w) continue;
        float z=clip.z/clip.w;
        float2 center=float2(clip.x/clip.w*.5+.5,.5-clip.y/clip.w*.5)*float2(cloud.width,cloud.height);
        float radius=cloud.pointSize*.5;
        if (any(center+radius<0) || any(center-radius>=float2(cloud.width,cloud.height))) continue;
        int2 lo=int2(max(float2(0),floor(center-radius))),hi=int2(min(float2(cloud.width-1,cloud.height-1),floor(center+radius)));
        uint64 key=(uint64(asuint(max(z,0.0)))<<32)|uint64(p.id);
        for (int y=lo.y;y<=hi.y;++y) for (int x=lo.x;x<=hi.x;++x) for (uint sample=0;sample<cloud.samples;++sample) {
            float2 delta=float2(x,y)+samplePosition(sample)-center;
            if (dot(delta,delta)>radius*radius) continue;
            uint64 ignored;
            // One indivisible depth+source-ID winner, deterministic under reordering.
            // No racy non-atomic early-depth loads and no center-only rejection.
            InterlockedMax(cloud.winners[(uint(y)*cloud.width+uint(x))*cloud.samples+sample],key,ignored);
        }
    }
}
struct ResolveVertex { float4 position:SV_Position; };
ResolveVertex vs_point_resolve(uint vertex:SV_VertexID) {
    ResolveVertex v; v.position=float4(vertex==1?3.0:-1.0,vertex==2?3.0:-1.0,0,1); return v;
}
struct ResolveOutput { float4 color:SV_Target0; float4 id:SV_Target1; float depth:SV_Depth; };
ResolveOutput fs_point_resolve(ResolveVertex input,uint sample:SV_SampleIndex) {
    uint2 pixel=uint2(input.position.xy);
    uint64 key=cloud.winners[(pixel.y*cloud.width+pixel.x)*cloud.samples+sample];
    uint id=uint(key);
    if (id==0) discard;
    uint lo=0,hi=cloud.groupCount;
    while (lo<hi) { uint mid=lo+(hi-lo)/2; if (cloud.groups[mid].endId<=id) lo=mid+1; else hi=mid; }
    ResolveOutput o;
    o.color=cloud.groups[lo].color;
    o.id=float4(float(id&255),float((id>>8)&255),float((id>>16)&255),float(id>>24))/255;
    o.depth=asfloat(uint(key>>32)); return o;
}
[numthreads(8,8,1)] void cs_point_ids(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    for (uint sample=0;sample<cloud.samples;++sample) {
        uint index=(thread.y*cloud.width+thread.x)*cloud.samples+sample;
        cloud.ids[index]=uint(cloud.winners[index]);
    }
}
