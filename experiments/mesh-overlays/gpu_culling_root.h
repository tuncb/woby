#pragma once
#include <NoGraphicsAPIUtility/shader_types.h>

struct CullMip { uint32 offset, width, height, reserved; };
struct CullGroup {
    float4x4 modelViewProj;
    uint32 pointOffset, pointCount, outputOffset, firstBlock, blockCount, enabled;
};
struct CullBlock { uint32 group, localOffset; };
struct CullRoot {
    float* vertices;
    uint32* points;
    CullGroup* groups;
    CullBlock* blocks;
    float* depth;
    uint32* ranks;
    uint32* counts;
    uint32* offsets;
    uint32* selected;
    uint32* arguments;
    uint32* scanInput;
    uint32* scanOutput;
    uint32* scanSums;
    uint32* scanParent;
    CullMip mips[12];
    uint32 width, height, samples, depthTexture;
    uint32 stride, blockCount, groupCount, readDepth;
    uint32 count, dispatchWidth, level, reserved;
    float pointSize;
};
#ifndef __SLANG__
static_assert(sizeof(CullMip)==16);
static_assert(sizeof(CullGroup)==88);
static_assert(sizeof(CullBlock)==8);
static_assert(offsetof(CullRoot,mips)==112);
static_assert(offsetof(CullRoot,pointSize)==352);
static_assert(sizeof(CullRoot)==360);
#endif
