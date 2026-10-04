#pragma once
#include "gpu_culling_root.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <array>
#include <vector>

namespace woby::overlay {
struct Renderer;
struct Display;
struct Capture;
enum class Culling { none, split, frustum, footprint };
struct CullScanLevel { uint32_t count=0; gpu::GpuHeap values{}, offsets{}; };
struct CullGroupInput {
    std::array<float,16> model{};
    bool enabled=false;
    bool operator==(const CullGroupInput&) const = default;
};
struct GpuCulling {
    gpu::PSO *depthPass=nullptr, *reduce=nullptr, *classify=nullptr, *scan=nullptr,
        *add=nullptr, *argumentsPass=nullptr, *scatter=nullptr;
    gpu::GpuHeap depth{}, groups{}, blocks{}, ranks{}, selected{}, arguments{},
        argumentReadback{}, selectionReadback{}, finalSum{};
    std::vector<CullScanLevel> levels;
    std::vector<CullGroup> layout;
    std::vector<std::array<uint32_t,2>> ranges;
    std::vector<CullGroupInput> inputs;
    std::array<float,16> projection{};
    std::array<CullMip,12> mips{};
    uint32_t width=0,height=0,mipCount=0,blockCount=0,capacity=0;
    uint64_t residentBytes=0;
    std::array<uint64_t,5> timestamps{};
    bool inputValid=false;
};
// The harness drains each frame before reuse. No visibility results survive a frame.
void prepareGpuCulling(Renderer& renderer,bool readSelection=false);
void updateGpuCullInputs(Renderer& renderer,const std::array<float,16>& projection);
void submitGpuCulling(Renderer& renderer,gpu::CommandBuffer* commands,const Display& display,bool opaqueDepth);
void captureGpuCulling(Renderer& renderer,gpu::CommandBuffer* commands,bool fullSelection);
void readGpuCulling(Renderer& renderer,Capture& capture);
void destroyGpuCulling(GpuCulling& culling);
} // namespace woby::overlay
