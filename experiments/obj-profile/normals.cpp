// Rerun production normal generation; output hashing is outside each timer.
#include "obj_mesh.h"
#include "profile.h"
#include <bit>
#include <iostream>
#include <stdexcept>

namespace {
uint64_t normalDigest(const std::vector<woby::Vertex>& vertices)
{
    uint64_t result = 14695981039346656037ull;
    for (const auto& vertex : vertices) {
        for (float component : vertex.normal) {
            auto bits = std::bit_cast<uint32_t>(component);
            for (unsigned byte = 0; byte < 4; ++byte) {
                result = (result ^ (bits & 255)) * 1099511628211ull;
                bits >>= 8;
            }
        }
    }
    return result;
}
int run(const std::filesystem::path& path, size_t rounds)
{
    woby::ObjPrototypeOptions options; options.deferPreparation = true;
    const auto importStart = obj_profile::Clock::now();
    auto mesh = woby::loadObjMeshPrototype(path, {}, options);
    const auto importMs = std::chrono::duration<double, std::milli>(obj_profile::Clock::now() - importStart).count();
    auto& geometry = woby::editMeshGeometry(mesh);
    obj_profile::Json rows = obj_profile::Json::array();
    for (size_t round = 0; round < rounds; ++round) {
        const auto before = obj_profile::counters();
        const auto start = obj_profile::Clock::now();
        woby::generateSmoothNormals(geometry.vertices, geometry.indices);
        const auto elapsed = std::chrono::duration<double, std::milli>(obj_profile::Clock::now() - start).count();
        const auto after = obj_profile::counters();
        rows.push_back({{"round", round + 1}, {"wall_ms", elapsed},
            {"cpu_ms", double(after.user + after.kernel - before.user - before.kernel) / 10000.0},
            {"peak_commit_bytes", after.peakCommit}, {"normal_digest", normalDigest(geometry.vertices)}});
    }
    std::cout << obj_profile::Json{{"import_ms", importMs}, {"vertices", geometry.vertices.size()},
        {"triangles", geometry.indices.size() / 3}, {"records", rows}}.dump() << '\n';
    return 0;
}
}
int wmain(int argc, wchar_t** argv)
{
    try {
        if (argc < 2 || argc > 3) { throw std::invalid_argument("Usage: woby_normals_benchmark OBJ [rounds]"); }
        const auto rounds = argc > 2 ? std::stoull(argv[2]) : 3;
        if (!rounds || rounds > 20) { throw std::invalid_argument("Rounds must be between 1 and 20"); }
        return run(argv[1], static_cast<size_t>(rounds));
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
