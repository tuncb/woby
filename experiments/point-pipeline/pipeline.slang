#include <NoGraphicsAPI/shader.slang>
#include "root.h"
GPU_ROOT(PipelineRoot, experiment);

// Share the production shader functions. The surface root remains the prefix.
#undef GPU_ROOT
#define GPU_ROOT(type,name) static type name = experiment.surface
#include "../../shaders/native/woby.slang"
#undef GPU_ROOT
#define GPU_ROOT(type,name) static type name = experiment.points
#include "../../shaders/native/opaque_points.slang"

VertexOutput vs_pipeline_combined(uint index:SV_VertexID) {
    VertexOutput output=vs_mesh(index);
    uint id=experiment.vertexIds[index];
    if (id!=0) {
        uint previous=0;
        if (experiment.claimOnce!=0) InterlockedExchange(experiment.claimed[id-1],1,previous);
        if (previous==0) {
            // Undo the Vulkan Y conversion. Reuse the mesh vertex transform.
            rasterProjectedPoint(clipPosition(output.position),cloud.groups[0].pointSize,id,false,false);
        }
    }
    return output;
}
[numthreads(128,1,1)] void cs_pipeline_claim_clear(uint3 thread:SV_DispatchThreadID) {
    uint index=thread.y*65535*128+thread.x;
    if (index<experiment.pointCount) experiment.claimed[index]=0;
}
float surfaceDepth(uint2 pixel,uint sample) {
    pixel+=uint2(cloud.x,cloud.y);
    if (cloud.samples==1) return gpu_texture<Texture2D<float>>(experiment.depthTexture).Load(int3(pixel,0));
    return gpu_texture<Texture2DMS<float>>(experiment.depthTexture).Load(pixel,sample);
}
[numthreads(8,8,1)] void cs_pipeline_seed(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    for (uint sample=0;sample<cloud.samples;++sample) {
        // Native depth is D32_FLOAT. ID zero denotes a surface. Equal-depth
        // markers still win because their IDs are positive.
        uint depth=asuint(max(surfaceDepth(thread.xy,sample),0.0));
        cloud.winners[(thread.y*cloud.width+thread.x)*cloud.samples+sample]=uint64(depth)<<32;
    }
    if (cloud.tileSize==0) return;
    uint columns=(cloud.width+cloud.tileSize-1)>>cloud.tileShift;
    uint rows=(cloud.height+cloud.tileSize-1)>>cloud.tileShift;
    uint index=thread.y*cloud.width+thread.x;
    if (index>=columns*rows) return;
    uint2 first=uint2(index%columns,index/columns)*cloud.tileSize;
    uint2 end=min(first+cloud.tileSize,uint2(cloud.width,cloud.height));
    uint nearestLowerBound=asuint(1.0);
    for (uint y=first.y;y<end.y;++y) for (uint x=first.x;x<end.x;++x)
        for (uint sample=0;sample<cloud.samples;++sample)
            nearestLowerBound=min(nearestLowerBound,asuint(max(surfaceDepth(uint2(x,y),sample),0.0)));
    cloud.coverage[index]=uint64(nearestLowerBound)<<32;
}
uint packColor(float4 value) {
    uint4 bytes=uint4(round(saturate(value)*255.0));
    return bytes.x|(bytes.y<<8)|(bytes.z<<16)|(bytes.w<<24);
}
[numthreads(8,8,1)] void cs_pipeline_capture(uint3 thread:SV_DispatchThreadID) {
    if (any(thread.xy>=uint2(cloud.width,cloud.height))) return;
    uint2 pixel=thread.xy+uint2(cloud.x,cloud.y);
    for (uint sample=0;sample<cloud.samples;++sample) {
        float4 color,id;
        if (cloud.samples==1) {
            color=gpu_texture<Texture2D<float4>>(experiment.colorTexture).Load(int3(pixel,0));
            id=gpu_texture<Texture2D<float4>>(experiment.idTexture).Load(int3(pixel,0));
        } else {
            color=gpu_texture<Texture2DMS<float4>>(experiment.colorTexture).Load(pixel,sample);
            id=gpu_texture<Texture2DMS<float4>>(experiment.idTexture).Load(pixel,sample);
        }
        PipelineSample value;
        value.color=packColor(color); value.id=packColor(id); value.depth=asuint(surfaceDepth(thread.xy,sample));
        experiment.captured[(thread.y*cloud.width+thread.x)*cloud.samples+sample]=value;
    }
}
