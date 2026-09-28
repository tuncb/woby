#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>

// Deliberately matches woby::Vertex (32 bytes) and ImDrawVert (20 bytes).
struct MeshVertex { float3 position; float3 normal; float2 uv; };
struct UiVertex { float2 position; float2 uv; uint32 color; };
struct PickResult { uint32 id; uint32 coveredSamples; uint32 mixedPixels; uint32 padding; };
struct DrawRoot {
    MeshVertex* vertices;
    float4 color;
    float2 inverseExtent;
    uint32 firstId;
    uint32 marker;
    float pointSize;
    uint32 padding;
};
struct PickRoot {
    PickResult* result;
    int2 cursor;
    uint2 extent;
    uint32 idTexture;
    uint32 samples;
};
struct CompositeRoot {
    PickResult* result;
    uint32 colorTexture;
    uint32 idTexture;
    uint32 samples;
    uint32 highlight;
};
struct UiRoot {
    UiVertex* vertices;
    float2 scale;
    float2 translate;
    uint32 texture;
};
