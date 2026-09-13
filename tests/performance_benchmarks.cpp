#include "mesh_comparison.h"
#include "obj_mesh.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

woby::Mesh grid(uint32_t width, float z)
{
    woby::Mesh result;
    result.vertices.reserve(size_t(width + 1) * (width + 1));
    result.indices.reserve(size_t(width) * width * 6);
    for (uint32_t y = 0; y <= width; ++y) {
        for (uint32_t x = 0; x <= width; ++x) {
            woby::Vertex vertex;
            vertex.position = {static_cast<float>(x), static_cast<float>(y), z};
            vertex.normal = {0, 0, 1};
            result.vertices.push_back(vertex);
        }
    }
    for (uint32_t y = 0; y < width; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto a = y * (width + 1) + x;
            result.indices.insert(result.indices.end(), {a, a + 1, a + width + 2, a, a + width + 2, a + width + 1});
        }
    }
    result.nodes.push_back({"grid", 0, static_cast<uint32_t>(result.indices.size())});
    result.bounds = woby::calculateBounds(result.vertices);
    return result;
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory()
    {
        const auto prefix = "woby_benchmark_" + std::to_string(Clock::now().time_since_epoch().count());
        for (size_t attempt = 0;; ++attempt) {
            path = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) { break; }
        }
    }
    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
}

// Deliberately separate from unit tests: no wall-clock assertions. Build Release
// and run the same workload on the same machine before and after a change.
int main(int argc, char** argv)
{
    try {
        const std::string workload = argc > 1 ? argv[1] : "distance";
        const auto width = argc > 2 ? static_cast<uint32_t>(std::stoul(argv[2])) : 200u;
        const auto repetitions = argc > 3 ? std::stoul(argv[3]) : 3u;
        if (width == 0 || width > 2000 || repetitions == 0 || repetitions > 100) {
            throw std::invalid_argument("Expected width 1..2000 and repetitions 1..100.");
        }
        const auto a = grid(width, 0), b = grid(width, .25f);
        const TemporaryDirectory fixture;
        const auto path = fixture.path / "grid.obj";
        if (workload == "obj") {
            std::ofstream file(path);
            for (const auto& vertex : a.vertices) {
                const auto& p = vertex.position;
                file << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
            }
            file << "vn 0 0 1\n";
            for (size_t i = 0; i < a.indices.size(); i += 3) {
                file << "f " << a.indices[i] + 1 << "//1 " << a.indices[i + 1] + 1 << "//1 " << a.indices[i + 2] + 1 << "//1\n";
            }
            if (!file) { throw std::runtime_error("Cannot write benchmark fixture."); }
        }
        std::vector<double> times;
        double checksum = 0;
        for (size_t run = 0; run < repetitions; ++run) {
            // Input copies belong outside the compaction timer.
            auto compactInput = workload == "compact" ? a : woby::Mesh{};
            const auto start = Clock::now();
            if (workload == "obj") {
                const auto loaded = woby::loadObjMesh(path);
                checksum += static_cast<double>(loaded.vertices.size());
            } else if (workload == "compact") {
                woby::compactMesh(compactInput.vertices, compactInput.indices);
                checksum += static_cast<double>(compactInput.vertices.size());
            } else if (workload == "distance") {
                const auto result = woby::computeComparisonStages(a, b, woby::comparisonDistance);
                checksum += result.original.mean + result.repaired.mean;
            } else if (workload == "quality") {
                const auto result = woby::inspectSurfaceMeshQuality(a);
                checksum += static_cast<double>(result.triangles.size());
            } else {
                throw std::invalid_argument("Workload must be obj, compact, distance, or quality.");
            }
            times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        }
        std::sort(times.begin(), times.end());
        std::cout << "workload,width,triangles,repetitions,hardware_threads,min_ms,median_ms,max_ms,checksum\n"
                  << workload << ',' << width << ',' << a.indices.size() / 3 << ',' << repetitions << ','
                  << std::thread::hardware_concurrency() << ',' << times.front() << ',' << times[times.size() / 2]
                  << ',' << times.back() << ',' << checksum << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
