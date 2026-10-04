// Reuse the production mesh, marker, and picking shaders for the controls.
#include "woby.slang"

// This portable comparison reads the existing triangle indices rather than
// allocating an expanded vertex buffer. It does sacrifice indexed vertex reuse.
struct PullOutput {
    VertexOutput mesh;
    noperspective float3 bary : TEXCOORD2;
};
PullOutput vs_overlay_pull(uint corner : SV_VertexID) {
    PullOutput v;
    v.mesh = vs_mesh(root.pointIds[corner]);
    uint k = corner % 3;
    v.bary = float3(k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0);
    return v;
}
float4 overlayColor(VertexOutput v, float3 bary) {
    float3 dx = ddx(bary), dy = ddy(bary);
    float3 distance = bary / max(sqrt(dx*dx + dy*dy), float3(1e-8));
    float d = min(distance.x, min(distance.y, distance.z));
    float coverage = 1-smoothstep(max(0.0,root.markerOptions.y-.5), root.markerOptions.y+.5, d);
    if (root.markerOptions.z < .5) {
        if (coverage <= 0) discard;
        return float4(saturate(root.color.rgb*1.25),root.color.a*coverage);
    }
    return float4(lerp(fs_mesh(v).rgb, saturate(root.color.rgb*1.25),coverage), root.color.a);
}
float4 fs_overlay_native(VertexOutput v, noperspective float3 bary : SV_Barycentrics) : SV_Target0 {
    return overlayColor(v,bary);
}
float4 fs_overlay_pull(PullOutput v) : SV_Target0 { return overlayColor(v.mesh,v.bary); }
MarkerOutput fs_overlay_native_ids(VertexOutput v, noperspective float3 bary : SV_Barycentrics) {
    return MarkerOutput(overlayColor(v,bary),float4(0));
}
MarkerOutput fs_overlay_pull_ids(PullOutput v) { return MarkerOutput(overlayColor(v.mesh,v.bary),float4(0)); }

// Capture every sample's provenance without averaging packed IDs.
// Only runs outside measured frames. The production sample-aware picker uses
// the same attachment and packed ID convention.
[numthreads(8,8,1)] void cs_capture_ids(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= uint2(root.markerQuery.zw))) return;
    for (uint sampleIndex=0;sampleIndex<uint(root.markerOptions.x);++sampleIndex) {
        uint offset=(thread.y*uint(root.markerQuery.z)+thread.x)*uint(root.markerOptions.x)+sampleIndex;
        root.pointIds[offset]=readId(int2(thread.xy),sampleIndex,root.markerOptions.x>1);
    }
}

[numthreads(8,8,1)] void cs_capture_depth(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= uint2(root.markerQuery.zw))) return;
    float depth=1;
    for (uint sampleIndex=0;sampleIndex<uint(root.markerOptions.x);++sampleIndex) {
        float candidate=root.markerOptions.x>1
            ? gpu_texture<Texture2DMS<float>>(root.texture0).Load(int2(thread.xy),sampleIndex)
            : gpu_texture<Texture2D<float>>(root.texture0).Load(int3(thread.xy,0));
        depth=min(depth,candidate); // Reversed depth: furthest MSAA sample.
    }
    root.pointIds[thread.y*uint(root.markerQuery.z)+thread.x]=asuint(depth);
}

VertexOutput vs_point_compacted(uint vertex : SV_VertexID,uint instance : SV_InstanceID) {
    VertexOutput v = {};
    uint base = uint(root.pointParams[1].x) + uint(root.pointParams[1].y)*65536u;
    uint original = root.pointIds[base+instance];
    uint index = ((uint*)root.freeform)[original];
    // Keep the production quad math and full circular fragment coverage.
    float2 corner = float2(1.0-float((vertex>>1)&1),float(vertex&1))-0.5;
    float4 center = mul(root.modelViewProj,float4(positionAt(index),1));
    center.xy += corner * root.pointParams[0].x * float2(2.0/root.pointParams[0].y,2.0/root.pointParams[0].z) * center.w;
    v.position = clipPosition(center);
    v.uv = corner;
    v.id = original+1;
    return v;
}
