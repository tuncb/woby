#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>
struct WobyRoot {
    float4x4 modelViewProj;
    float4x4 model;
    float4 color;
    float4 pointParams; // Size, viewport width/height, compact-point flag.
    uint32 pointOffset;
    uint32 pointSourceFirst;
    uint32 markerBase;
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
    float2 triangleEdges; // Half width, fill surface.
    uint32 triangleOffset;
    float4 transparency; // Reversed depth, near/far distance, perspective flag; graphics adaptor owns these.
    float* analysisFaces;
    uint32 faceKind; // 1 quality, 2 UV, 3 distance, 4 procedural wireframe.
    uint32 firstFace;
    uint32* vertexColors;
    uint32 vertexColorCount;
};

#ifndef __SLANG__
static_assert(offsetof(WobyRoot, modelViewProj) == 0);
static_assert(offsetof(WobyRoot, model) == 64);
static_assert(offsetof(WobyRoot, color) == 128);
static_assert(offsetof(WobyRoot, pointParams) == 144);
static_assert(offsetof(WobyRoot, pointOffset) == 160);
static_assert(offsetof(WobyRoot, pointSourceFirst) == 164);
static_assert(offsetof(WobyRoot, markerBase) == 168);
static_assert(offsetof(WobyRoot, markerQuery) == 172);
static_assert(offsetof(WobyRoot, markerOptions) == 188);
static_assert(offsetof(WobyRoot, comparison) == 204);
static_assert(offsetof(WobyRoot, uvGrid) == 220);
static_assert(offsetof(WobyRoot, vertices) == 240);
static_assert(offsetof(WobyRoot, pointIds) == 248);
static_assert(offsetof(WobyRoot, stride) == 256);
static_assert(offsetof(WobyRoot, texture0) == 260);
static_assert(offsetof(WobyRoot, texture1) == 264);
static_assert(offsetof(WobyRoot, image1) == 268);
static_assert(offsetof(WobyRoot, sampler) == 272);
static_assert(offsetof(WobyRoot, triangleEdges) == 276);
static_assert(offsetof(WobyRoot, triangleOffset) == 284);
static_assert(offsetof(WobyRoot, transparency) == 288);
static_assert(offsetof(WobyRoot, analysisFaces) == 304);
static_assert(offsetof(WobyRoot, faceKind) == 312);
static_assert(offsetof(WobyRoot, firstFace) == 316);
static_assert(alignof(WobyRoot) == 8);
static_assert(offsetof(WobyRoot, vertexColors) == 320);
static_assert(offsetof(WobyRoot, vertexColorCount) == 328);
static_assert(sizeof(WobyRoot) == 336);
#endif
