#pragma once
#include "scene_renderer.h"
#include <future>

namespace woby {
struct AdaptivePointKey {
    std::shared_ptr<const points::Cloud> cloud;
    graphics::OpaquePointGroup group;
    uint32_t sourceGroup=0;
};
struct AdaptivePointDraw {
    AdaptivePointKey key;
    const GpuMesh* mesh=nullptr; // Submission-local, cleared before returning.
};
struct AdaptivePointCut {
    uint64_t epoch=0;
    std::vector<points::Selection> selections;
};
struct AdaptivePointRuntime {
    graphics::VertexBufferHandle winners=WOBY_GPU_INVALID_HANDLE;
    graphics::ProgramHandle clear=WOBY_GPU_INVALID_HANDLE, raster=WOBY_GPU_INVALID_HANDLE, resolve=WOBY_GPU_INVALID_HANDLE;
    graphics::ProgramHandle resolveIds=WOBY_GPU_INVALID_HANDLE;
    std::vector<AdaptivePointDraw> draws;
    std::vector<AdaptivePointKey> keys;
    std::vector<points::Refinement> cursors;
    std::vector<points::Selection> navigation, full;
    std::future<AdaptivePointCut> pending;
    points::Matrix projection{}, previousProjection{};
    std::array<float,2> query{}, previousQuery{};
    bool queryValid=false;
    uint32_t width=0,height=0,previousWidth=0,previousHeight=0,budget=2000000,lastTiming=0;
    uint32_t navigationBudget=0;
    uint64_t epoch=0, total=0, refined=0, submitted=0;
    double now=0,lastChange=0;
    bool enabled=false,adaptive=true,previousAdaptive=true,queryEnabled=false,unavailable=false,active=false;
    std::string error;
};
void prepareAdaptivePoints(AdaptivePointRuntime&,const std::filesystem::path& assets,
    const SceneDrawPlan&,const ScenePickView&,bool adaptive,double now,bool queryEnabled=false,
    std::array<float,2> query={});
bool queueAdaptivePoints(AdaptivePointRuntime&,const GpuMesh&,const SceneDrawItem&,uint32_t firstId);
void submitAdaptivePoints(AdaptivePointRuntime&,graphics::ViewId,bool markerIds=false);
void destroyAdaptivePoints(AdaptivePointRuntime&);
// Attach borrowed acceleration only for the immediate full-source CPU query.
void attachPointHierarchies(std::span<ScenePickPart>,const std::vector<LoadedModelRuntime>&);
} // namespace woby
