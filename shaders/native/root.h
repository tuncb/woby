#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>
struct WobyRoot {
    float4x4 modelViewProj;
    float4x4 model;
    float4 color;
    float4 pointParams[2];
    float4 markerBase;
    float4 markerQuery;
    float4 markerOptions;
    float4 comparison;
    float4 uvGrid; // U/V cells per UV unit, enabled, reserved.
    float* vertices;
    uint32* pointIds;
    uint32 stride;
    uint32 texture0;
    uint32 texture1;
    uint32 image1;
    uint32 sampler;
    float4 triangleEdges; // Half width, fill surface, triangle-index offset low/high.
    float4 transparency; // Reversed depth, near/far distance, perspective flag; graphics adaptor owns these.
    float* analysisFaces;
    uint32 faceKind; // 1 quality, 2 UV, 3 distance, 4 procedural wireframe.
    uint32 firstFace;
};

#ifndef __SLANG__
static_assert(offsetof(WobyRoot, vertices) == 256);
static_assert(offsetof(WobyRoot, sampler) == 288);
static_assert(offsetof(WobyRoot, triangleEdges) == 292);
static_assert(offsetof(WobyRoot, transparency) == 308);
static_assert(offsetof(WobyRoot, analysisFaces) == 328);
static_assert(offsetof(WobyRoot, faceKind) == 336);
static_assert(sizeof(WobyRoot) == 344);
#endif
