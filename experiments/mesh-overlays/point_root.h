#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>
struct CloudPoint { float3 position; uint32 id; };
struct CloudGroup { float4x4 matrix; float4 color; uint32 firstId,endId,enabled,reserved; };
struct CloudTask { CloudPoint* points; uint32 count,group; };
struct PointRoot {
    uint64* winners;
    CloudTask* tasks;
    CloudGroup* groups;
    uint32* ids;
    uint32 width,height,samples,depthTexture;
    uint32 taskCount,dispatchWidth,groupCount,readDepth;
    float pointSize;
};
#ifndef __SLANG__
static_assert(sizeof(CloudPoint)==16);
static_assert(sizeof(CloudGroup)==96);
static_assert(sizeof(CloudTask)==16);
static_assert(offsetof(PointRoot,width)==32);
static_assert(sizeof(PointRoot)==72);
#endif
