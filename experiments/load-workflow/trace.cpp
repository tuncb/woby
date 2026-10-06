#include "trace.h"
#include "point_cloud.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace woby::workflow {
namespace {
thread_local std::string currentPath;
std::string environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || !value) { return {}; }
    std::string result(value);
    std::free(value);
    return result;
#else
    const auto* value = std::getenv(name);
    return value ? value : "";
#endif
}
struct Session {
    Clock::time_point start = Clock::now();
    std::string reader = [] {
        const auto value = environment("WOBY_WORKFLOW_VARIANT");
        const std::string name = value.empty() ? "prototype" : value;
        if (name != "legacy" && name != "prototype" && name != "prototype_copy") {
            throw std::invalid_argument("Unknown WOBY_WORKFLOW_VARIANT.");
        }
        return name;
    }();
    std::mutex mutex;
    Json events = Json::array();
    bool published = false, submitted = false;
    ~Session() {
        try {
            if (const auto path = environment("WOBY_WORKFLOW_TRACE"); !path.empty()) {
                std::ofstream stream(std::filesystem::path(path), std::ios::binary);
                stream << Json{{"variant", reader}, {"events", events}}.dump(2);
            }
        } catch (...) {} // Diagnostics must not change shutdown behavior.
    }
};
Session& session() { static Session value; return value; }
Json memory() {
    Json result = Json::object();
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX info{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info), static_cast<DWORD>(sizeof(info)))) {
        result = {{"private_bytes", info.PrivateUsage}, {"working_set_bytes", info.WorkingSetSize},
            {"peak_commit_bytes", info.PeakPagefileUsage}, {"peak_working_set_bytes", info.PeakWorkingSetSize}};
    }
#endif
    return result;
}
double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
template <typename T> size_t capacityBytes(const std::vector<T>& values) { return values.capacity() * sizeof(T); }
void append(const std::string& name, const std::string& path, Clock::time_point start, Clock::time_point finish, Json data) {
    auto& state = session();
    Json row{{"name", name}, {"path", path}, {"start_ms", milliseconds(state.start, start)},
        {"end_ms", milliseconds(state.start, finish)}, {"duration_ms", milliseconds(start, finish)},
        {"memory", memory()}, {"data", std::move(data)}};
    std::lock_guard lock(state.mutex);
    state.events.push_back(std::move(row));
}
} // namespace
void setPath(std::string path) { currentPath = std::move(path); }
const std::string& variant() { return session().reader; }
Phase begin(std::string name) { (void)session(); return {std::move(name), currentPath, Clock::now()}; }
void end(const Phase& phase, Json data) { append(phase.name, phase.path, phase.start, Clock::now(), std::move(data)); }
void next(Phase& phase, std::string name, Json data) { end(phase, std::move(data)); phase = begin(std::move(name)); }
void mark(std::string name, Json data) {
    auto& state = session();
    if (name == "load_requested") { state.published = state.submitted = false; }
    if (name == "scene_committed") { state.published = true; }
    const auto now = Clock::now();
    append(name, currentPath, now, now, std::move(data));
}
void frameSubmitted(bool hasFiles) {
    auto& state = session();
    if (!hasFiles || !state.published || state.submitted) { return; }
    state.submitted = true;
    mark("first_scene_frame_submitted");
}
Json meshInfo(const Mesh& mesh) {
    Json result{{"vertices", mesh.vertices.size()}, {"triangle_indices", mesh.indices.size()},
        {"line_indices", mesh.lineIndices.size()}, {"point_indices", mesh.pointIndices.size()},
        {"groups", mesh.nodes.size()}, {"freeform", bool(mesh.freeform)},
        {"render_vertex_bytes", capacityBytes(mesh.vertices)}, {"precise_position_bytes", capacityBytes(mesh.precisePositions)},
        {"triangle_index_bytes", capacityBytes(mesh.indices)}, {"line_index_bytes", capacityBytes(mesh.lineIndices)},
        {"point_index_bytes", capacityBytes(mesh.pointIndices)}};
    if (mesh.sourceData) {
        result.update({{"source_positions", mesh.sourceData->points.size()},
            {"source_position_bytes", capacityBytes(mesh.sourceData->points)},
            {"source_index_bytes", capacityBytes(mesh.sourceData->indices)},
            {"source_id_bytes", capacityBytes(mesh.sourceData->originalPointIds)}});
    }
    return result;
}
Json preparationInfo(const SceneMeshPreparation& prepared) {
    Json result{{"upload_bytes", prepared.uploadBytes}, {"compact_only", prepared.compactOnly},
        {"point_membership_bytes", capacityBytes(prepared.pointVertexIndices)},
        {"edge_index_bytes", capacityBytes(prepared.edgeIndices)}, {"node_range_bytes", capacityBytes(prepared.nodeRanges)}};
    if (const auto& cloud = prepared.pointCloud) {
        size_t cuts = 0;
        for (const auto& cut : cloud->navigationCuts) { cuts += capacityBytes(cut); }
        result.update({{"cloud_points", cloud->points.size()}, {"cloud_proxies", cloud->proxies.size()},
            {"cloud_point_bytes", capacityBytes(cloud->points)}, {"cloud_proxy_bytes", capacityBytes(cloud->proxies)},
            {"cloud_node_bytes", capacityBytes(cloud->nodes)}, {"cloud_source_mapping_bytes", capacityBytes(cloud->sourceVertices)},
            {"cloud_root_bytes", capacityBytes(cloud->roots)}, {"cloud_group_bytes", capacityBytes(cloud->groups)},
            {"cloud_navigation_bytes", cuts}});
    }
    return result;
}
Json annotationInfo(const MeshAnnotationCache& cache) {
    Json result{{"block_bytes", capacityBytes(cache.blocks)}, {"vertices", cache.vertexCount}, {"indices", cache.indexCount}};
    if (cache.spatial) {
        result.update({{"spatial_triangle_bytes", capacityBytes(cache.spatial->triangles)},
            {"spatial_tree_bytes", capacityBytes(cache.spatial->tree)}});
    }
    if (cache.snapshot) {
        result.update({{"snapshot_vertex_bytes", capacityBytes(cache.snapshot->vertices)},
            {"snapshot_index_bytes", capacityBytes(cache.snapshot->indices)},
            {"snapshot_block_bytes", cache.snapshot->annotationCache ? capacityBytes(cache.snapshot->annotationCache->blocks) : 0}});
    }
    return result;
}
} // namespace woby::workflow
