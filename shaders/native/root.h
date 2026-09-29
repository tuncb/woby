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
    float* vertices;
    uint32* pointIds;
    uint32 stride;
    uint32 texture0;
    uint32 texture1;
    uint32 image1;
    uint32 sampler;
};

#ifndef __SLANG__
static_assert(offsetof(WobyRoot, vertices) == 240);
static_assert(offsetof(WobyRoot, sampler) == 272);
static_assert(sizeof(WobyRoot) == 280);
#endif
