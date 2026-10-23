#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>
struct OpaquePoint { float3 position; uint32 id; };
struct OpaquePointGroupData {
    float4x4 matrix;
    float4 color;
    uint32 firstId, endId, sourceFirstId;
    float pointSize;
    uint32* colors;
    uint32 colorCount;
};
struct OpaquePointTaskData { OpaquePoint* points; uint32 count, group, query, padding; };
// Metal atomics require addressable scalar fields, not uint2 vector components.
struct OpaquePointBatchSample { uint32 depth, id; };
struct OpaquePointRoot {
    uint64* winners;
    OpaquePointTaskData* tasks;
    OpaquePointGroupData* groups;
    uint32 width, height, samples, taskCount, dispatchWidth, groupCount, x, y;
    uint4 query;
    OpaquePointBatchSample* batch; // Per-batch depth bits and original ID; 32-bit atomics.
    uint64* coverage; // A depth/ID lower bound for every sample in a tile.
    uint32 tileSize, tileShift;
};
#ifndef __SLANG__
#include <cstddef>
static_assert(sizeof(OpaquePoint)==16);
static_assert(sizeof(OpaquePointGroupData)==112);
static_assert(sizeof(OpaquePointTaskData)==24);
static_assert(sizeof(OpaquePointBatchSample)==8);
static_assert(sizeof(OpaquePointRoot)==96);
static_assert(offsetof(OpaquePointRoot,coverage)==80);
static_assert(offsetof(OpaquePointRoot,tileSize)==88);
static_assert(offsetof(OpaquePointRoot,tileShift)==92);
#endif
