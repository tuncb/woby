#pragma once
#include "point_cloud.h"
#include "point_root.h"
#include "renderer.h"

namespace woby::points {
struct PointRenderer {
    overlay::Renderer gpu;
    std::vector<gpu::GpuHeap> points,proxies;
    gpu::GpuHeap winners{},tasks{},groups{};
    gpu::PSO *clear=nullptr,*raster=nullptr,*resolve=nullptr,*surface=nullptr,*capture=nullptr;
    uint64_t geometryBytes=0,visibilityBytes=0;
    uint32_t chunkPoints=1u<<20,taskCapacity=0;
    std::vector<CloudTask> taskList;
    std::vector<Range> previousRanges;
    uint64_t submitted=0;
    Matrix previousProjection{};
    std::vector<Group> previousGroups;
    float previousSize=0;
    bool previousSolid=false,valid=false;
    PointRenderer()=default;
    PointRenderer(const PointRenderer&)=delete;
    PointRenderer& operator=(const PointRenderer&)=delete;
    ~PointRenderer();
};
struct Timing { double gpuMs=0,clearMs=0,rasterMs=0,resolveMs=0,cpuMs=0,wallMs=0; uint64_t submitted=0; uint32_t tasks=0; bool reset=false; };
void initialize(PointRenderer& renderer,const Cloud& cloud,const Mesh& mesh,overlay::Options options,uint32_t chunkPoints=1u<<20);
// A false reset is allowed only when every view/geometry-style input still matches.
Timing render(PointRenderer& renderer,const Cloud& cloud,const Selection& selection,
    const Matrix& projection,float pointSize,bool solid,bool reset,overlay::Capture* capture=nullptr);
} // namespace woby::points
