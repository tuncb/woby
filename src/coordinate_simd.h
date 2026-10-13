#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif
#if defined(_M_X64)
#include <intrin.h>
#endif

namespace woby {

inline bool avx512RebaseFeatures(uint32_t leaf1Ecx, uint32_t leaf7Ebx, uint64_t xcr0)
{
    // Check the complete ISA enabled by MSVC /arch:AVX512, plus OS register state.
    constexpr uint32_t cpu = (1u << 26) | (1u << 27) | (1u << 28);
    constexpr uint32_t wide = (1u << 5) | (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31);
    return (leaf1Ecx & cpu) == cpu && (leaf7Ebx & wide) == wide && (xcr0 & 0xe6) == 0xe6;
}

inline bool avx512CoordinateRebaseAvailable()
{
#if defined(WOBY_ENABLE_AVX512_REBASE) && defined(_M_X64)
    static const bool supported = [] {
        int leaf[4]{};
        __cpuid(leaf, 0);
        if (leaf[0] < 7) { return false; }
        __cpuidex(leaf, 1, 0);
        const auto ecx = static_cast<uint32_t>(leaf[2]);
        if ((ecx & (1u << 27)) == 0) { return false; }
        const auto state = _xgetbv(0);
        __cpuidex(leaf, 7, 0);
        return avx512RebaseFeatures(ecx, static_cast<uint32_t>(leaf[1]), state);
    }();
    return supported;
#elif defined(WOBY_ENABLE_AVX512_REBASE) && defined(__x86_64__)
    // GCC and Clang include OS XSAVE state in their CPU feature checks.
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("avx512f")
        && __builtin_cpu_supports("avx512dq") && __builtin_cpu_supports("avx512cd")
        && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512vl");
#else
    return false;
#endif
}

#ifdef WOBY_ENABLE_AVX512_REBASE
void rebaseCoordinatesAvx512(std::span<std::array<double, 3>> points, const std::array<double, 3>& origin);
#endif

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__)
__attribute__((noinline))
#endif
static inline void rebaseCoordinatesNative(std::span<std::array<double, 3>> points, const std::array<double, 3>& origin)
{
    size_t i = 0;
#if defined(__AVX2__) || defined(__aarch64__) || defined(_M_ARM64)
#if defined(__AVX2__)
    constexpr size_t width = 4;
    using Vector = __m256d;
    const Vector offsets[3]{_mm256_setr_pd(origin[0], origin[1], origin[2], origin[0]),
        _mm256_setr_pd(origin[1], origin[2], origin[0], origin[1]),
        _mm256_setr_pd(origin[2], origin[0], origin[1], origin[2])};
#else
    constexpr size_t width = 2;
    using Vector = float64x2_t;
    const double values[6]{origin[0], origin[1], origin[2], origin[0], origin[1], origin[2]};
    const Vector offsets[3]{vld1q_f64(values), vld1q_f64(values + 2), vld1q_f64(values + 4)};
#endif
    static_assert(sizeof(std::array<double, 3>) == 3 * sizeof(double));
    auto bytes = std::as_writable_bytes(points);
    for (; points.size() - i >= width; i += width) {
        for (size_t block = 0; block < 3; ++block) {
            auto* destination = bytes.data() + i * sizeof(points[0]) + block * sizeof(Vector);
            Vector value;
            std::memcpy(&value, destination, sizeof(value));
#if defined(__AVX2__)
            value = _mm256_sub_pd(value, offsets[block]);
#else
            value = vsubq_f64(value, offsets[block]);
#endif
            std::memcpy(destination, &value, sizeof(value));
        }
    }
#endif
    for (; i < points.size(); ++i) {
        for (size_t k = 0; k < 3; ++k) { points[i][k] -= origin[k]; }
    }
}

static inline void rebaseCoordinates(std::span<std::array<double, 3>> points, const std::array<double, 3>& origin)
{
#ifdef WOBY_ENABLE_AVX512_REBASE
    if (points.size() >= 8 && avx512CoordinateRebaseAvailable()) {
        rebaseCoordinatesAvx512(points, origin);
        return;
    }
#endif
    rebaseCoordinatesNative(points, origin);
}

} // namespace woby
