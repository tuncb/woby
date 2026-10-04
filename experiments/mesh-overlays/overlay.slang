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

// Capture the first nonzero sample's provenance without averaging packed IDs.
// Only runs outside measured frames. The production sample-aware picker uses
// the same attachment and packed ID convention.
[numthreads(8,8,1)] void cs_capture_ids(uint3 thread : SV_DispatchThreadID) {
    if (any(thread.xy >= uint2(root.markerQuery.zw))) return;
    uint id = 0;
    for (uint sampleIndex=0;sampleIndex<uint(root.markerOptions.x);++sampleIndex) {
        uint candidate = readId(int2(thread.xy),sampleIndex,root.markerOptions.x>1);
        if (candidate != 0) { id=candidate; break; }
    }
    root.pointIds[thread.y*uint(root.markerQuery.z)+thread.x] = id;
}
