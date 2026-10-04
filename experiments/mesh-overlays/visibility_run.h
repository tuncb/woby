#pragma once
#include "visibility.h"
namespace woby::overlay {
struct VisibilityRun {
    std::filesystem::path output;
    std::string model;
    int rounds = 3, solid = -1;
    double seconds = 2, warmup = 1;
    float pointSize = 0, zoom = 1;
    bool orthographic = false;
};
void measureVisibility(Renderer& renderer,const Mesh& mesh,
    const std::array<float,16>& projection,const VisibilityRun& options);
void measureGpuCulling(Renderer& renderer,const std::array<float,16>& projection,const VisibilityRun& options);
}
