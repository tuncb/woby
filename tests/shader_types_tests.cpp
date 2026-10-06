#include <cstddef>
#include <cstdint>
#include <type_traits>

#if defined(__APPLE__) && defined(__aarch64__)
#include <arm_neon.h>
#else
// Reproduce a platform-owned float16_t even on non-ARM development machines.
using float16_t = std::uint16_t;
#endif

#include "root.h"
#include <doctest/doctest.h>

TEST_CASE("shared shader types coexist with platform half types and preserve storage bits")
{
    static_assert(sizeof(float16_t) == 2);
    static_assert(std::is_trivially_copyable_v<WobyFloat16Storage>);
    static_assert(sizeof(float16_t2) == 4);
    static_assert(sizeof(float16_t3) == 6);
    static_assert(sizeof(float16_t4) == 8);
    static_assert(alignof(float16_t4) == alignof(std::uint16_t));
    // Half NaN payloads remain raw bits across the CPU/GPU ABI.
    const float16_t4 source{{0x7e01}, {0xfc00}, {0x8000}, {0x0001}};
    const auto copy = source;
    CHECK(copy.x.bits == 0x7e01);
    CHECK(copy.y.bits == 0xfc00);
    CHECK(copy.z.bits == 0x8000);
    CHECK(copy.w.bits == 0x0001);
    CHECK(sizeof(WobyRoot) == 336);
    CHECK(offsetof(WobyRoot, triangleEdges) == 304);
    CHECK(offsetof(WobyRoot, transparency) == 320);
}
