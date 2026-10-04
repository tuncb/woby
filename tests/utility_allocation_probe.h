#pragma once

#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

struct UtilityAllocationProbe
{
    int step = 0, failAt = 0;
    int arrays = 0, heaps = 0, timelines = 0, pools = 0, textures = 0;
    int textureAttempts = 0, unexpectedCalls = 0;
    gpu::uint64 lastTextureOffset = 0;
    int commandStep = 0, failCommandAt = 0, copies = 0, submissions = 0;
    gpu::uint64 submittedValue = 0, waitedValue = 0;
};

extern UtilityAllocationProbe utilityProbe;
gpu::Device* utilityTestDevice() noexcept;
gpu::TextureHeap utilityTestTextureHeap() noexcept;
gpu::TimelineSemaphore* utilityTestTimeline() noexcept;
