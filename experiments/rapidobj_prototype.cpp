#include "obj_mesh.h"
#include "freeform.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {
struct Fingerprint { uint64_t value = 14695981039346656037ull; };
template <typename T> void hash(Fingerprint& out, const T& value) {
    const auto* data = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) { out.value = (out.value ^ data[i]) * 1099511628211ull; }
}
void hashText(Fingerprint& out, const std::string& text) {
    hash(out, text.size());
    for (const auto ch : text) { hash(out, ch); }
}
template <typename T> void hashBuffer(Fingerprint& out, const std::vector<T>& values) {
    hash(out, values.size());
    for (const auto& value : values) { hash(out, value); }
}
uint64_t fingerprint(const woby::Mesh& mesh) {
    Fingerprint out;
    hash(out, mesh.origin);
    hashBuffer(out, meshGeometry(mesh).vertices); hashBuffer(out, meshGeometry(mesh).positionIndices);
    if (meshGeometry(mesh).sourcePositions) { hashBuffer(out, *meshGeometry(mesh).sourcePositions); }
    hashBuffer(out, meshGeometry(mesh).indices); hashBuffer(out, meshGeometry(mesh).lineIndices); hashBuffer(out, meshGeometry(mesh).pointIndices);
    if (mesh.sourceData) {
        hashBuffer(out, woby::sourcePositions(*mesh.sourceData)); hashBuffer(out, mesh.sourceData->indices);
        hashBuffer(out, mesh.sourceData->originalPointIds);
    }
    for (const auto& node : meshGeometry(mesh).nodes) {
        hashText(out, node.name); hash(out, node.indexOffset); hash(out, node.indexCount);
        hash(out, node.lineIndexOffset); hash(out, node.lineIndexCount);
        hash(out, node.pointIndexOffset); hash(out, node.pointIndexCount); hash(out, node.hasTexcoords);
    }
    if (mesh.freeform) {
        for (const auto& patch : mesh.freeform->patches) {
            hashText(out, patch.name); hashBuffer(out, patch.controls);
            hashBuffer(out, patch.texcoords); hashBuffer(out, patch.normals);
            hashBuffer(out, patch.knotsU); hashBuffer(out, patch.knotsV);
            hash(out, patch.domainU); hash(out, patch.domainV);
        }
        for (const auto& grid : mesh.freeform->grids) {
            hashBuffer(out, grid.u); hashBuffer(out, grid.v);
            hashBuffer(out, grid.samples); hashBuffer(out, grid.triangles);
        }
    }
    return out.value;
}
size_t retainedBytes(const woby::Mesh& mesh) {
    size_t bytes = meshGeometry(mesh).vertices.capacity() * sizeof(woby::Vertex)
        + meshGeometry(mesh).vertexColors.capacity() * sizeof(uint32_t)
        + meshGeometry(mesh).precisePositions.capacity() * sizeof(woby::Coordinate)
        + meshGeometry(mesh).positionIndices.capacity() * sizeof(uint32_t)
        + (meshGeometry(mesh).indices.capacity() + meshGeometry(mesh).lineIndices.capacity() + meshGeometry(mesh).pointIndices.capacity()) * sizeof(uint32_t);
    if (mesh.sourceData) {
        bytes += woby::sourcePositions(*mesh.sourceData).capacity() * sizeof(woby::Coordinate)
            + mesh.sourceData->indices.capacity() * sizeof(uint32_t)
            + mesh.sourceData->originalPointIds.capacity() * sizeof(uint64_t);
    }
    return bytes;
}
int run(bool prototype, const std::filesystem::path& path, size_t workers, bool deferred = false) {
    try {
        std::vector<std::filesystem::path> paths;
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (entry.is_regular_file() && entry.path().extension() == ".obj") { paths.push_back(entry.path()); }
            }
            std::sort(paths.begin(), paths.end());
        } else { paths.push_back(path); }
        if (paths.empty()) { throw std::runtime_error("Benchmark input contains no OBJ files."); }
        woby::ObjPrototypeOptions options; options.workers = workers;
        options.deferPreparation = deferred;
        woby::ObjPrototypeMetrics metrics;
        std::vector<woby::Mesh> meshes; meshes.reserve(paths.size());
        const auto begin = std::chrono::steady_clock::now();
        for (const auto& file : paths) {
            woby::ObjPrototypeMetrics current;
            meshes.push_back(prototype ? woby::loadObjMeshPrototype(file, {}, options, &current) : woby::loadObjMeshLegacy(file));
            metrics.parseMs += current.parseMs; metrics.freeformMs += current.freeformMs; metrics.prepareMs += current.prepareMs;
            metrics.inputBytes += current.inputBytes; metrics.chunks += current.chunks;
            metrics.parsedChunkBytes += current.parsedChunkBytes; metrics.positionCopyBytesAvoided += current.positionCopyBytesAvoided;
            metrics.workers = std::max(metrics.workers, current.workers);
            metrics.peakInflightTextBytes = std::max(metrics.peakInflightTextBytes, current.peakInflightTextBytes);
            const auto& mapping = current.mapping;
            metrics.mapping.staged = metrics.mapping.staged || mapping.staged;
            metrics.mapping.resolveWorkers = std::max(metrics.mapping.resolveWorkers,mapping.resolveWorkers);
            metrics.mapping.resolveScratchBytes += mapping.resolveScratchBytes;
            metrics.mapping.gatherWorkers = std::max(metrics.mapping.gatherWorkers,mapping.gatherWorkers);
            metrics.mapping.primaryCapacityBytes += mapping.primaryCapacityBytes;
            metrics.mapping.keyCapacityBytes += mapping.keyCapacityBytes;
            metrics.mapping.bucketCapacityBytes += mapping.bucketCapacityBytes;
            metrics.mapping.seamVertices += mapping.seamVertices;
            metrics.mapping.releasedTriangleBytes += mapping.releasedTriangleBytes;
            metrics.mapping.vertexCapacityBytes += mapping.vertexCapacityBytes;
            metrics.mapping.positionMapCapacityBytes += mapping.positionMapCapacityBytes;
            metrics.mapping.resolveMs += mapping.resolveMs; metrics.mapping.releaseMs += mapping.releaseMs;
            metrics.mapping.vertexAllocationMs += mapping.vertexAllocationMs; metrics.mapping.positionMapMs += mapping.positionMapMs;
            metrics.mapping.primaryGatherMs += mapping.primaryGatherMs; metrics.mapping.seamGatherMs += mapping.seamGatherMs;
            metrics.mapping.totalMs += mapping.totalMs;
        }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        size_t vertices = 0, points = 0, indices = 0, retained = 0;
        for (const auto& mesh : meshes) {
            vertices += meshGeometry(mesh).vertices.size(); indices += meshGeometry(mesh).indices.size(); retained += retainedBytes(mesh);
            points += mesh.sourceData ? woby::sourcePositions(*mesh.sourceData).size() : 0;
        }
        nlohmann::json result{{"mode", prototype ? "prototype" : "legacy"}, {"cpu_load_ms", elapsed}, {"files", paths.size()},
            {"render_vertices", vertices}, {"source_points", points},
            {"triangle_indices", indices}, {"retained_mesh_buffer_capacity_bytes", retained}};
#ifdef _WIN32
        PROCESS_MEMORY_COUNTERS_EX memory{};
        if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), static_cast<DWORD>(sizeof(memory)))) {
            result["peak_working_set_bytes"] = memory.PeakWorkingSetSize;
            result["peak_commit_bytes"] = memory.PeakPagefileUsage;
            result["private_bytes_after_load"] = memory.PrivateUsage;
        }
#endif
        if (prototype) {
            result["parse_ms"] = metrics.parseMs; result["freeform_ms"] = metrics.freeformMs;
            result["prepare_ms"] = metrics.prepareMs; result["chunks"] = metrics.chunks; result["workers"] = metrics.workers;
            result["input_bytes"] = metrics.inputBytes; result["parsed_chunk_bytes"] = metrics.parsedChunkBytes;
            result["peak_inflight_text_bytes"] = metrics.peakInflightTextBytes;
            result["position_copy_bytes_avoided"] = metrics.positionCopyBytesAvoided;
            result["deferred"] = deferred;
            const auto& m = metrics.mapping;
            result["corner_mapping"] = {{"staged",m.staged},{"gather_workers",m.gatherWorkers},
                {"resolve_workers",m.resolveWorkers},{"resolve_scratch_bytes",m.resolveScratchBytes},
                {"primary_capacity_bytes",m.primaryCapacityBytes},{"key_capacity_bytes",m.keyCapacityBytes},
                {"bucket_capacity_bytes",m.bucketCapacityBytes},{"seam_vertices",m.seamVertices},{"released_triangle_bytes",m.releasedTriangleBytes},
                {"vertex_capacity_bytes",m.vertexCapacityBytes},{"position_map_capacity_bytes",m.positionMapCapacityBytes},
                {"resolve_ms",m.resolveMs},{"release_ms",m.releaseMs},{"vertex_allocation_ms",m.vertexAllocationMs},
                {"position_map_ms",m.positionMapMs},{"primary_gather_ms",m.primaryGatherMs},{"seam_gather_ms",m.seamGatherMs},{"total_ms",m.totalMs}};
        }
        // Validation is outside the timed load and memory snapshot.
        Fingerprint combined;
        for (const auto& mesh : meshes) { hash(combined, fingerprint(mesh)); }
        result["fingerprint"] = combined.value;
        Fingerprint colors;
        size_t colorBytes = 0;
        for (const auto& mesh : meshes) {
            hashBuffer(colors, meshGeometry(mesh).vertexColors);
            colorBytes += meshGeometry(mesh).vertexColors.size() * sizeof(uint32_t);
        }
        result["color_fingerprint"] = colors.value;
        result["vertex_color_bytes"] = colorBytes;
        std::cout << result.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5 || (std::wstring_view(argv[1]) != L"prototype" && std::wstring_view(argv[1]) != L"legacy") || (argc == 5 && std::wstring_view(argv[4]) != L"deferred")) {
#else
int main(int argc, char** argv) {
    if (argc < 3 || argc > 5 || (std::string_view(argv[1]) != "prototype" && std::string_view(argv[1]) != "legacy") || (argc == 5 && std::string_view(argv[4]) != "deferred")) {
#endif
        std::cerr << "Usage: woby_obj_prototype_benchmark legacy|prototype absolute.obj [workers] [deferred]\n";
        return 2;
    }
#ifdef _WIN32
    const bool prototype = std::wstring_view(argv[1]) != L"legacy";
#else
    const bool prototype = std::string_view(argv[1]) != "legacy";
#endif
    return run(prototype, std::filesystem::path(argv[2]), argc >= 4 ? static_cast<size_t>(std::stoull(argv[3])) : 0, argc == 5);
}
