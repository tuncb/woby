#include "mapping_probe.h"
#include "obj_mesh.h"
#include "scene_renderer.h"
#include "utf8_path.h"
#include <cstddef>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif
namespace woby {
GpuMesh mappingPointRanges(const Mesh& mesh);
void prepareAnnotationMeshCache(Mesh& mesh);
}
namespace {
uint64_t hashBytes(uint64_t hash, const void* data, size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < bytes; ++i) { hash = (hash ^ p[i]) * 1099511628211ull; }
    return hash;
}
template<typename T> uint64_t hashVector(const std::vector<T>& values) {
    return hashBytes(14695981039346656037ull, values.data(), values.size()*sizeof(T));
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    using namespace mapping_probe;
    Json output;
    try {
        if (argc != 2) { throw std::runtime_error("usage: mapping_benchmark ABSOLUTE_OBJ_PATH"); }
#ifdef _WIN32
        const auto path = std::filesystem::path(argv[1]);
#else
        const auto path = woby::pathFromUtf8(argv[1]);
#endif
        if (!path.is_absolute()) { throw std::runtime_error("absolute path required"); }
        const auto begin = Clock::now();
        auto mesh = woby::loadObjMesh(path);
        const double loadMs = elapsed(begin);
        output = {{"file", woby::pathToUtf8(path)}, {"variant", MAPPING_VARIANT}, {"load_ms", loadMs},
            {"vertices", mesh.vertices.size()}, {"indices", mesh.indices.size()}, {"nodes", mesh.nodes.size()},
            {"vertex_bytes", mesh.vertices.size()*sizeof(woby::Vertex)},
            {"vertex_capacity_bytes", mesh.vertices.capacity()*sizeof(woby::Vertex)},
            {"index_bytes", mesh.indices.size()*sizeof(uint32_t)},
            {"source_point_bytes", mesh.sourceData->points.size()*sizeof(mesh.sourceData->points[0])},
            {"source_index_bytes", mesh.sourceData->indices.size()*sizeof(uint32_t)},
            {"layout", {sizeof(woby::Vertex), offsetof(woby::Vertex, position), offsetof(woby::Vertex, normal), offsetof(woby::Vertex, texcoord)}},
            {"map", stats}};
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX memory{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
        output["load_peak_working_set_bytes"] = memory.PeakWorkingSetSize;
        output["load_peak_commit_bytes"] = memory.PeakPagefileUsage;
#endif
        start();
        woby::prepareAnnotationMeshCache(mesh);
        mark("annotation_cache_ms");
        output["annotation_blocks"] = mesh.annotationCache ? mesh.annotationCache->blocks.size() : 0;
        const auto groups = woby::createUiGroupStates(mesh, 0);
        mark("group_state_ms");
        output["groups"] = groups.size();
        auto points = woby::mappingPointRanges(mesh);
        output["point_entries"] = points.pointVertexIndices.size();
        output["stages"] = stages;
        // Full ordered byte fingerprints after all measured stages, not in load timing.
        Json hashes = {{"vertices", hashVector(mesh.vertices)}, {"indices", hashVector(mesh.indices)},
            {"source_points", hashVector(mesh.sourceData->points)}, {"source_indices", hashVector(mesh.sourceData->indices)},
            {"point_indices", hashVector(points.pointVertexIndices)}};
        uint64_t nodes = 14695981039346656037ull;
        for (const auto& node : mesh.nodes) {
            nodes = hashBytes(nodes, node.name.data(), node.name.size());
            nodes = hashBytes(nodes, &node.indexOffset, sizeof(node.indexOffset));
            nodes = hashBytes(nodes, &node.indexCount, sizeof(node.indexCount));
        }
        hashes["nodes"] = nodes;
        output["hashes"] = hashes;
        output["bounds"] = {mesh.bounds.min, mesh.bounds.max, mesh.bounds.center};
        output["radius"] = mesh.bounds.radius;
        output["ok"] = true;
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        output["ok"] = false; output["error"] = error.what();
        std::cout << output.dump() << '\n'; return 1;
    }
}
