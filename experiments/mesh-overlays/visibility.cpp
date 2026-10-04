#include "visibility.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace woby::overlay {
namespace {
struct DepthLevel { uint32_t width = 0, height = 0; std::vector<float> values; };
std::vector<DepthLevel> pyramid(uint32_t width, uint32_t height, std::span<const float> depth) {
    if (!width || !height || depth.size() != size_t(width)*height)
        throw std::invalid_argument("Depth dimensions do not match visibility image");
    for (const auto value : depth) if (!std::isfinite(value) || value < 0 || value > 1)
        throw std::invalid_argument("Visibility requires finite reversed depth in [0,1]");
    std::vector<DepthLevel> levels{{width,height,{depth.begin(),depth.end()}}};
    while (width > 1 || height > 1) {
        DepthLevel next{(width+1)/2,(height+1)/2,{}};
        next.values.resize(size_t(next.width)*next.height);
        for (uint32_t y=0;y<next.height;++y) for (uint32_t x=0;x<next.width;++x) {
            float value=1;
            for (uint32_t dy=0;dy<2;++dy) for (uint32_t dx=0;dx<2;++dx)
                value=std::min(value,2*x+dx<width && 2*y+dy<height
                    ? levels.back().values[size_t(2*y+dy)*width+2*x+dx] : 0.0f);
            next.values[size_t(y)*next.width+x]=value;
        }
        width=next.width; height=next.height; levels.push_back(std::move(next));
    }
    return levels;
}
float rectangleDepth(const std::vector<DepthLevel>& levels, uint32_t left, uint32_t top,
    uint32_t right, uint32_t bottom) {
    const auto extent=std::max(right-left+1,bottom-top+1);
    uint32_t level=0;
    while ((1u<<level)<extent) ++level;
    const auto& data=levels[level];
    float minimum=1;
    // An extent at most one cell wide intersects at most two cells per axis.
    for (const auto y : {top>>level,bottom>>level}) for (const auto x : {left>>level,right>>level})
        minimum=std::min(minimum,data.values[size_t(y)*data.width+x]);
    return minimum;
}
void beginGroup(MarkerSelection& selection) {
    selection.groups.push_back({static_cast<uint32_t>(selection.markers.size()),0});
}
void retain(MarkerSelection& selection,uint32_t marker) {
    selection.markers.push_back(marker); ++selection.groups.back().count;
}
}

VisibilityAudit inspectVisibility(const Mesh& mesh,const Scene& scene,
    const std::array<float,16>& viewProjection,uint32_t width,uint32_t height,
    float pointSize,std::span<const float> minimumSampleDepth) {
    if (!std::isfinite(pointSize) || pointSize<1 || pointSize>40)
        throw std::invalid_argument("Visibility point size must be 1..40 pixels");
    const auto levels=pyramid(width,height,minimumSampleDepth);
    VisibilityAudit result;
    for (const auto& group : scene.groups) {
        beginGroup(result.frustum); beginGroup(result.conservative); beginGroup(result.center);
        if (!group.points) continue;
        std::array<float,16> matrix{}; bx::mtxMul(matrix.data(),group.model.data(),viewProjection.data());
        for (uint32_t i=0;i<group.range.pointIndexCount;++i) {
            const auto marker=group.range.pointIndexOffset+i;
            const auto& p=mesh.vertices.at(scene.markerVertices.at(marker)).position;
            std::array<float,4> clip{};
            for (size_t row=0;row<4;++row)
                clip[row]=matrix[row]*p[0]+matrix[4+row]*p[1]+matrix[8+row]*p[2]+matrix[12+row];
            ++result.submitted;
            if (!std::all_of(clip.begin(),clip.end(),[](float value){return std::isfinite(value);})
                || clip[3]<=0 || clip[2]<0 || clip[2]>clip[3]) { ++result.depthClipped; continue; }
            const float x=(clip[0]/clip[3]*.5f+.5f)*float(width);
            const float y=(.5f-clip[1]/clip[3]*.5f)*float(height);
            const float z=clip[2]/clip[3], radius=pointSize*.5f;
            // Include the complete sprite and an extra half-pixel margin. A
            // background MSAA sample also prevents conservative rejection.
            const float margin=radius+.5f;
            if (x+margin<0 || y+margin<0 || x-margin>=float(width) || y-margin>=float(height)) {
                ++result.offscreen; continue;
            }
            retain(result.frustum,marker);
            result.projectedQuadPixels += double(std::max(0.0f,std::min(x+radius,float(width))-std::max(x-radius,0.0f)))
                * double(std::max(0.0f,std::min(y+radius,float(height))-std::max(y-radius,0.0f)));
            const float tolerance=std::max(1e-6f,std::abs(z)*1e-5f);
            if (x<0 || y<0 || x>=float(width) || y>=float(height)) ++result.centerOutside;
            else if (z+tolerance<minimumSampleDepth[size_t(y)*width+size_t(x)]) ++result.centerOccluded;
            else retain(result.center,marker);
            const auto left=static_cast<uint32_t>(std::max(0.0f,std::floor(x-margin)));
            const auto top=static_cast<uint32_t>(std::max(0.0f,std::floor(y-margin)));
            const auto right=static_cast<uint32_t>(std::clamp(std::ceil(x+margin),0.0f,float(width-1)));
            const auto bottom=static_cast<uint32_t>(std::clamp(std::ceil(y+margin),0.0f,float(height-1)));
            if (z+tolerance<rectangleDepth(levels,left,top,right,bottom)) ++result.footprintOccluded;
            else retain(result.conservative,marker);
        }
    }
    return result;
}

MarkerSelection finalMarkerOracle(const Scene& scene,std::span<const uint32_t> sampleIds) {
    std::vector<uint8_t> visible(scene.markerCount);
    for (const auto id : sampleIds) {
        if (id>visible.size()) throw std::invalid_argument("Captured marker ID exceeds original provenance table");
        if (id) visible[id-1]=1;
    }
    MarkerSelection result;
    for (const auto& group : scene.groups) {
        beginGroup(result);
        if (!group.points) continue;
        for (uint32_t i=0;i<group.range.pointIndexCount;++i) {
            const auto marker=group.range.pointIndexOffset+i;
            if (visible.at(marker)) retain(result,marker);
        }
    }
    return result;
}
} // namespace woby::overlay
