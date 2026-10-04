#pragma once
#include "model_mesh.h"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace woby { struct GpuNodeRange; }
namespace woby::points {
using Matrix = std::array<float,16>;
struct Point { std::array<float,3> position; uint32_t id; };
static_assert(sizeof(Point)==16);
struct Group {
    uint32_t firstId=0, endId=0;
    Matrix model{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::array<float,4> color{.33f,.42f,.45f,1};
    bool enabled=true;
    friend bool operator==(const Group&,const Group&)=default;
};
struct Node {
    std::array<float,3> low{}, high{};
    uint32_t begin=0, count=0, left=0, right=0, proxyBegin=0, proxyCount=0, group=0;
    std::array<uint32_t,6> extrema{};
};
struct NavigationNode { uint32_t node=0; bool full=false; };
struct Cloud {
    std::vector<Point> points, proxies;
    std::vector<Node> nodes;
    std::vector<uint32_t> roots;
    std::vector<Group> groups;
    // Original marker ID -> render vertex -> precise/source position in Mesh.
    // This cold CPU mapping is never traversed to draw a frame.
    std::vector<uint32_t> sourceVertices;
    // Camera-independent cuts are prepared on the loading worker. They retain
    // coverage during navigation while a view-specific cut is being computed.
    std::vector<std::vector<NavigationNode>> navigationCuts;
};
struct Range { uint32_t begin=0,count=0,group=0; bool proxy=false; friend bool operator==(const Range&,const Range&)=default; };
struct Selection { std::vector<Range> ranges; uint64_t points=0; uint32_t visited=0; };
// Positions are unquantized; Morton keys only order them.
Cloud buildCloud(const Mesh& mesh,uint32_t leafSize=4096,uint32_t proxySize=128);
std::optional<Cloud> buildCloud(const Mesh& mesh, std::vector<uint32_t> sourceVertices,
    std::span<const GpuNodeRange> ranges, const std::function<bool()>& shouldCancel,
    uint32_t leafSize=4096, uint32_t proxySize=128);
Selection selectDetail(const Cloud& cloud,const Matrix& projection,uint32_t width,uint32_t height,
    float pointSize,uint32_t pointBudget,float spacing=1);
Selection selectDetail(const Cloud& cloud, std::span<const Group> groups, const Matrix& projection,
    uint32_t width,uint32_t height,float pointSize,uint32_t pointBudget,float spacing=1);
Selection navigationDetail(const Cloud& cloud, uint32_t pointBudget, uint32_t group=UINT32_MAX);
// Full source leaves whose circle footprints can intersect a screen rectangle.
Selection queryFootprints(const Cloud& cloud, uint32_t group, const Matrix& modelProjection,
    uint32_t width, uint32_t height, float pointSize, std::array<float,4> rectangle);
Selection allPoints(const Cloud& cloud);
struct Refinement { size_t range=0; uint32_t offset=0; uint64_t processed=0; };
Selection nextRefinement(const Selection& full,Refinement& cursor,uint32_t pointBudget);
uint32_t adjustedBudget(uint32_t previous,double gpuMs,double targetMs,uint32_t maximum);
// Exact full-source footprint query; hierarchy prunes work but never substitutes proxies.
struct Pick { uint32_t id=0; float depth=0; uint64_t tested=0; };
Pick pick(const Cloud& cloud,const Matrix& projection,uint32_t width,uint32_t height,
    float pointSize,float pixelX,float pixelY,float surfaceDepth=0);
} // namespace woby::points
