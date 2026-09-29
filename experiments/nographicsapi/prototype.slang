#include <NoGraphicsAPI/shader.slang>
#include "shared.h"

#if defined(NG_SCENE)
GPU_ROOT(DrawRoot, scene);
struct SceneVarying {
    float4 position : SV_Position;
    sample float2 circle : TEXCOORD0;
    nointerpolation uint id : TEXCOORD1;
};
SceneVarying sceneVertex(uint vertex : SV_VertexID) {
    SceneVarying v;
    v.position = float4(scene.vertices[vertex].position, 1);
    v.position.y = -v.position.y;
    v.circle = 0;
    v.id = 0; // Surfaces/edges clear IDs when they occlude markers.
    return v;
}
SceneVarying markerVertex(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    const float2 corners[6] = {float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};
    SceneVarying v;
    v.position = float4(scene.vertices[instance].position, 1);
    v.position.y = -v.position.y;
    v.circle = corners[vertex];
    v.position.xy += v.circle * scene.pointSize * scene.inverseExtent;
    v.id = scene.firstId + instance;
    return v;
}
struct SceneOutput { float4 color : SV_Target0; uint id : SV_Target1; };
SceneOutput sceneFragment(SceneVarying v) {
    if (scene.color.a <= 0 || (scene.marker != 0 && dot(v.circle, v.circle) > 1)) discard;
    SceneOutput o;
    o.color = scene.color;
    o.id = v.id;
    return o;
}
#endif
#if defined(NG_PICK) || defined(NG_COMPOSITE)
uint loadId(uint texture, int2 pixel, uint sample, bool msaa) {
    if (msaa) return gpu_texture<Texture2DMS<uint>>(texture).Load(pixel, sample);
    return gpu_texture<Texture2D<uint>>(texture).Load(int3(pixel, 0));
}
#endif
#if defined(NG_PICK)
GPU_ROOT(PickRoot, picker);
void lookup(bool msaa) {
    PickResult result = {0,0,0,0};
    int best = 1000;
    for (int y = -3; y <= 3; ++y) for (int x = -3; x <= 3; ++x) {
        int distance = x*x + y*y;
        int2 pixel = picker.cursor + int2(x,y);
        if (distance > 9 || any(pixel < 0) || any(pixel >= int2(picker.extent))) continue;
        uint first = loadId(picker.idTexture, pixel, 0, msaa);
        bool mixed = false;
        for (uint sample = 0; sample < picker.samples; ++sample) {
            uint id = loadId(picker.idTexture, pixel, sample, msaa);
            mixed = mixed || id != first;
            if (id == 0) continue;
            result.coveredSamples++;
            if (distance < best || (distance == best && id < result.id)) { result.id = id; best = distance; }
        }
        if (mixed) result.mixedPixels++;
    }
    *picker.result = result;
}
[numthreads(1,1,1)] void pickSingle() { lookup(false); }
[numthreads(1,1,1)] void pickMsaa() { lookup(true); }
#endif
#if defined(NG_COMPOSITE)
GPU_ROOT(CompositeRoot, composite);
struct Fullscreen { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Fullscreen compositeVertex(uint vertex : SV_VertexID) {
    Fullscreen v;
    v.uv = float2((vertex << 1) & 2, vertex & 2);
    v.position = float4(v.uv * 2 - 1, 0, 1);
    return v;
}
float4 compositeFragment(Fullscreen v) : SV_Target {
    float4 color = gpu_texture<Texture2D<float4>>(composite.colorTexture).Sample(gpu_sampler(0), v.uv);
    if (composite.highlight != 0 && composite.result->id != 0) {
        for (uint s = 0; s < composite.samples; ++s) {
            if (loadId(composite.idTexture, int2(v.position.xy), s, composite.samples > 1) == composite.result->id)
                return float4(1, 0.85, 0.12, 1);
        }
    }
    return color;
}
#endif
#if defined(NG_UI)
GPU_ROOT(UiRoot, ui);
struct UiVarying { float4 position : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };
UiVarying uiVertex(uint index : SV_VertexID) {
    UiVertex vertex = ui.vertices[index];
    UiVarying v;
    v.position = float4(vertex.position * ui.scale + ui.translate, 0, 1);
    v.uv = vertex.uv;
    v.color = float4(vertex.color & 255, (vertex.color >> 8) & 255, (vertex.color >> 16) & 255, vertex.color >> 24) / 255.0;
    return v;
}
float4 uiFragment(UiVarying v) : SV_Target {
    return v.color * gpu_texture<Texture2D<float4>>(ui.texture).Sample(gpu_sampler(0), v.uv);
}
#endif
