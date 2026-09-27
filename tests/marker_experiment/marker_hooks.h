#pragma once
#include <bgfx/bgfx.h>
#include <algorithm>
#include <array>
#include <vector>
#include "marker_logic.h"
namespace marker_experiment {
struct Hooks {
    bool active=false;
    bgfx::UniformHandle tag=BGFX_INVALID_HANDLE;
    std::vector<Draw>* draws=nullptr;
    float largestPoint=0;
};
inline Hooks hooks;
inline void recordDraw(const float* model, uint32_t begin, uint32_t count, float size) {
    if (!hooks.active) { return; }
    const std::array<float,4> tag={static_cast<float>(hooks.draws->size()),0,0,0};
    bgfx::setUniform(hooks.tag,tag.data());
    Draw d; std::copy_n(model,16,d.model.begin()); d.begin=begin; d.count=count;
    hooks.draws->push_back(d); hooks.largestPoint=std::max(hooks.largestPoint,size);
}
inline void setDrawState(uint64_t state) {
    if (hooks.active) {
        bgfx::setState(state|BGFX_STATE_BLEND_INDEPENDENT,
            BGFX_STATE_BLEND_FUNC_RT_1(BGFX_STATE_BLEND_ONE,BGFX_STATE_BLEND_ZERO));
    } else { bgfx::setState(state); }
}
}
