#include <array>
#include <cstring>
#include <immintrin.h>
#include <span>

namespace woby {

// This file has its own ISA flags. Call only after the CPU and OS checks.
void rebaseCoordinatesAvx512(std::span<std::array<double, 3>> points, const std::array<double, 3>& origin)
{
    static_assert(sizeof(std::array<double, 3>) == 3 * sizeof(double));
    const __m512d offsets[3]{
        _mm512_setr_pd(origin[0], origin[1], origin[2], origin[0], origin[1], origin[2], origin[0], origin[1]),
        _mm512_setr_pd(origin[2], origin[0], origin[1], origin[2], origin[0], origin[1], origin[2], origin[0]),
        _mm512_setr_pd(origin[1], origin[2], origin[0], origin[1], origin[2], origin[0], origin[1], origin[2])};
    auto bytes = std::as_writable_bytes(points);
    size_t i = 0;
    for (; points.size() - i >= 8; i += 8) {
        for (size_t block = 0; block < 3; ++block) {
            auto* destination = bytes.data() + i * sizeof(points[0]) + block * sizeof(__m512d);
            __m512d value;
            std::memcpy(&value, destination, sizeof(value));
            value = _mm512_sub_pd(value, offsets[block]);
            std::memcpy(destination, &value, sizeof(value));
        }
    }
    for (; i < points.size(); ++i) {
        for (size_t k = 0; k < 3; ++k) { points[i][k] -= origin[k]; }
    }
}

} // namespace woby
