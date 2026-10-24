#pragma once
#include "../../shaders/native/root.h"
#include "../../shaders/native/opaque_points.h"

struct PipelineSample { uint32 color, id, depth; };
struct PipelineRoot {
    WobyRoot surface;
    OpaquePointRoot points;
    uint32* vertexIds;
    uint32* claimed;
    PipelineSample* captured;
    uint32 pointCount, claimOnce, colorTexture, idTexture, depthTexture;
};
#ifndef __SLANG__
static_assert(offsetof(PipelineRoot,surface)==0);
static_assert(offsetof(PipelineRoot,points)==336);
static_assert(sizeof(PipelineSample)==12);
#endif
