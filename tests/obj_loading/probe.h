#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>

namespace obj_probe {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
inline Json stages = Json::object();
inline Clock::time_point previous;
inline void start() { previous = Clock::now(); }
inline void mark(const char* name)
{
    const auto now = Clock::now();
    stages[name] = std::chrono::duration<double, std::milli>(now - previous).count();
    previous = now;
}
inline double elapsed(Clock::time_point begin)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}
// Validation runs after timing. Position bounds/sums tolerate float-parser ULP
// differences; ordered index hashes also detect truncated or reordered geometry.
struct Summary {
    uint64_t positions = 0, normals = 0, texcoords = 0, faces = 0, corners = 0, triangles = 0;
    uint64_t polygons = 0, indexHash = 14695981039346656037ull;
    double sum[3]{};
    float minimum[3]{INFINITY, INFINITY, INFINITY};
    float maximum[3]{-INFINITY, -INFINITY, -INFINITY};
};
inline void point(Summary& s, const float* p)
{
    ++s.positions;
    for (size_t a = 0; a < 3; ++a) {
        if (!std::isfinite(p[a])) { throw std::runtime_error("non-finite position"); }
        s.sum[a] += p[a];
        s.minimum[a] = std::min(s.minimum[a], p[a]);
        s.maximum[a] = std::max(s.maximum[a], p[a]);
    }
}
inline void face(Summary& s, size_t n)
{
    if (n < 3) { throw std::runtime_error("non-face primitive in face array"); }
    ++s.faces;
    s.corners += n;
    s.triangles += n - 2;
    s.polygons += n != 3;
}
inline void index(Summary& s, int64_t p, int64_t n, int64_t t)
{
    if (p < 0 || static_cast<uint64_t>(p) >= s.positions || n < -1 || t < -1
        || (n >= 0 && static_cast<uint64_t>(n) >= s.normals)
        || (t >= 0 && static_cast<uint64_t>(t) >= s.texcoords)) {
        throw std::runtime_error("out-of-range parsed index");
    }
    for (const auto value : {p, n, t}) {
        s.indexHash ^= static_cast<uint64_t>(value + 1);
        s.indexHash *= 1099511628211ull;
    }
}
inline Json summary(const Summary& s)
{
    return {{"positions", s.positions}, {"normals", s.normals}, {"texcoords", s.texcoords},
        {"faces", s.faces}, {"corners", s.corners}, {"triangles", s.triangles},
        {"polygons", s.polygons}, {"index_hash", s.indexHash},
        {"position_sum", s.sum}, {"minimum", s.minimum}, {"maximum", s.maximum}};
}
Json rapid(const std::filesystem::path& path);
Json fast(const std::string& path);
Json tiny(const std::string& path, const std::string& mode, int threads);
} // namespace obj_probe
