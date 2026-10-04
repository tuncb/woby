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
    hashBuffer(out, mesh.vertices); hashBuffer(out, mesh.precisePositions);
    hashBuffer(out, mesh.indices); hashBuffer(out, mesh.lineIndices); hashBuffer(out, mesh.pointIndices);
    if (mesh.sourceData) {
        hashBuffer(out, mesh.sourceData->points); hashBuffer(out, mesh.sourceData->indices);
        hashBuffer(out, mesh.sourceData->originalPointIds);
    }
    for (const auto& node : mesh.nodes) {
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
    size_t bytes = mesh.vertices.capacity() * sizeof(woby::Vertex)
        + mesh.precisePositions.capacity() * sizeof(woby::Coordinate)
        + (mesh.indices.capacity() + mesh.lineIndices.capacity() + mesh.pointIndices.capacity()) * sizeof(uint32_t);
    if (mesh.sourceData) {
        bytes += mesh.sourceData->points.capacity() * sizeof(woby::Coordinate)
            + mesh.sourceData->indices.capacity() * sizeof(uint32_t)
            + mesh.sourceData->originalPointIds.capacity() * sizeof(uint64_t);
    }
    return bytes;
}
int run(bool prototype, const std::filesystem::path& path, size_t workers) {
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
        }
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        size_t vertices = 0, points = 0, indices = 0, retained = 0;
        for (const auto& mesh : meshes) {
            vertices += mesh.vertices.size(); indices += mesh.indices.size(); retained += retainedBytes(mesh);
            points += mesh.sourceData ? mesh.sourceData->points.size() : 0;
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
        }
        // Validation is outside the timed load and memory snapshot.
        Fingerprint combined;
        for (const auto& mesh : meshes) { hash(combined, fingerprint(mesh)); }
        result["fingerprint"] = combined.value;
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
    if (argc < 3 || argc > 4 || (std::wstring_view(argv[1]) != L"prototype" && std::wstring_view(argv[1]) != L"legacy")) {
#else
int main(int argc, char** argv) {
    if (argc < 3 || argc > 4 || (std::string_view(argv[1]) != "prototype" && std::string_view(argv[1]) != "legacy")) {
#endif
        std::cerr << "Usage: woby_obj_prototype_benchmark legacy|prototype absolute.obj [workers]\n";
        return 2;
    }
#ifdef _WIN32
    const bool prototype = std::wstring_view(argv[1]) == L"prototype";
#else
    const bool prototype = std::string_view(argv[1]) == "prototype";
#endif
    return run(prototype, std::filesystem::path(argv[2]), argc == 4 ? static_cast<size_t>(std::stoull(argv[3])) : 0);
}
