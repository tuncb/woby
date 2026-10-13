#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace woby {
namespace {

// Compare and select retain the first value on ties and unordered comparisons,
// as std::min/std::max do. Hardware min/max alone can change NaNs and signed zero.
struct FloatBoundsReduction {
#if defined(__AVX2__)
    __m128 low, high;
#elif defined(__aarch64__) || defined(_M_ARM64)
    float32x4_t low, high;
#else
    std::array<float, 3> low, high;
#endif
};

inline FloatBoundsReduction floatBoundsReduction(const std::array<float, 3>& low,
    const std::array<float, 3>& high)
{
#if defined(__AVX2__)
    return {_mm_setr_ps(low[0], low[1], low[2], 0), _mm_setr_ps(high[0], high[1], high[2], 0)};
#elif defined(__aarch64__) || defined(_M_ARM64)
    const float lo[4]{low[0], low[1], low[2], 0}, hi[4]{high[0], high[1], high[2], 0};
    return {vld1q_f32(lo), vld1q_f32(hi)};
#else
    return {low, high};
#endif
}

inline void expand(FloatBoundsReduction& bounds, const std::array<float, 3>& point)
{
#if defined(__AVX2__)
    const auto p = _mm_setr_ps(point[0], point[1], point[2], 0);
    bounds.low = _mm_blendv_ps(bounds.low, p, _mm_cmplt_ps(p, bounds.low));
    bounds.high = _mm_blendv_ps(bounds.high, p, _mm_cmplt_ps(bounds.high, p));
#elif defined(__aarch64__) || defined(_M_ARM64)
    const float values[4]{point[0], point[1], point[2], 0};
    const auto p = vld1q_f32(values);
    bounds.low = vbslq_f32(vcltq_f32(p, bounds.low), p, bounds.low);
    bounds.high = vbslq_f32(vcltq_f32(bounds.high, p), p, bounds.high);
#else
    for (size_t k = 0; k < 3; ++k) {
        bounds.low[k] = std::min(bounds.low[k], point[k]);
        bounds.high[k] = std::max(bounds.high[k], point[k]);
    }
#endif
}

inline std::array<std::array<float, 3>, 2> boundsResult(const FloatBoundsReduction& bounds)
{
#if defined(__AVX2__) || defined(__aarch64__) || defined(_M_ARM64)
    float low[4], high[4];
#if defined(__AVX2__)
    _mm_storeu_ps(low, bounds.low); _mm_storeu_ps(high, bounds.high);
#else
    vst1q_f32(low, bounds.low); vst1q_f32(high, bounds.high);
#endif
    return {{{low[0], low[1], low[2]}, {high[0], high[1], high[2]}}};
#else
    return {bounds.low, bounds.high};
#endif
}

struct DoubleBoundsReduction {
#if defined(__AVX2__)
    __m256d low, high;
#elif defined(__aarch64__) || defined(_M_ARM64)
    float64x2_t low, high;
    double lowZ, highZ;
#else
    std::array<double, 3> low, high;
#endif
};

inline DoubleBoundsReduction doubleBoundsReduction(const std::array<double, 3>& point)
{
#if defined(__AVX2__)
    const auto p = _mm256_setr_pd(point[0], point[1], point[2], 0);
    return {p, p};
#elif defined(__aarch64__) || defined(_M_ARM64)
    const auto p = vld1q_f64(point.data());
    return {p, p, point[2], point[2]};
#else
    return {point, point};
#endif
}

inline void expand(DoubleBoundsReduction& bounds, const std::array<double, 3>& point)
{
#if defined(__AVX2__)
    const auto p = _mm256_setr_pd(point[0], point[1], point[2], 0);
    bounds.low = _mm256_blendv_pd(bounds.low, p, _mm256_cmp_pd(p, bounds.low, _CMP_LT_OQ));
    bounds.high = _mm256_blendv_pd(bounds.high, p, _mm256_cmp_pd(bounds.high, p, _CMP_LT_OQ));
#elif defined(__aarch64__) || defined(_M_ARM64)
    const auto p = vld1q_f64(point.data());
    bounds.low = vbslq_f64(vcltq_f64(p, bounds.low), p, bounds.low);
    bounds.high = vbslq_f64(vcltq_f64(bounds.high, p), p, bounds.high);
    bounds.lowZ = std::min(bounds.lowZ, point[2]);
    bounds.highZ = std::max(bounds.highZ, point[2]);
#else
    for (size_t k = 0; k < 3; ++k) {
        bounds.low[k] = std::min(bounds.low[k], point[k]);
        bounds.high[k] = std::max(bounds.high[k], point[k]);
    }
#endif
}

inline std::array<std::array<double, 3>, 2> boundsResult(const DoubleBoundsReduction& bounds)
{
#if defined(__AVX2__)
    double low[4], high[4];
    _mm256_storeu_pd(low, bounds.low); _mm256_storeu_pd(high, bounds.high);
    return {{{low[0], low[1], low[2]}, {high[0], high[1], high[2]}}};
#elif defined(__aarch64__) || defined(_M_ARM64)
    std::array<std::array<double, 3>, 2> result{};
    vst1q_f64(result[0].data(), bounds.low); vst1q_f64(result[1].data(), bounds.high);
    result[0][2] = bounds.lowZ; result[1][2] = bounds.highZ;
    return result;
#else
    return {bounds.low, bounds.high};
#endif
}

} // namespace
} // namespace woby
