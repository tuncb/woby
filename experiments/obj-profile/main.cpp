#include "profile.h"
#include "obj_mesh.h"
#include "ui_state.h"
#include "scene_mesh_preparation.h"
#include "surface_annotation.h"
#include "point_cloud.h"
#include <rapidobj/prototype.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bit>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <fstream>
#include <iostream>
#include <memory>
#include <immintrin.h>

namespace {
obj_profile::Json flopControl() {
    constexpr size_t iterations = 128 * 1024 * 1024;
    const auto phase = obj_profile::begin("flop_control");
    auto value = _mm_set1_pd(1.0);
    const auto scale = _mm_set1_pd(1.0000000001), offset = _mm_set1_pd(0.000000001);
    for (size_t i = 0; i < iterations; ++i) { value = _mm_add_pd(_mm_mul_pd(value, scale), offset); }
    double result[2]{}; _mm_storeu_pd(result, value);
    obj_profile::end(phase);
    return {{"iterations", iterations}, {"expected_flops", iterations * 4}, {"result", result[0]}};
}
obj_profile::Json memoryCopy() {
    constexpr size_t bytes = 512 * 1024 * 1024, repeats = 16;
    const auto allocate = obj_profile::begin("memory_copy_allocate");
    std::vector<unsigned char> source(bytes, 0x5a), destination(bytes, 0x11);
    obj_profile::end(allocate);
    const auto start = obj_profile::Clock::now();
    const auto phase = obj_profile::begin("memory_copy");
    for (size_t i = 0; i < repeats; ++i) {
        std::memcpy(destination.data(), source.data(), bytes);
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    obj_profile::end(phase);
    const auto seconds = std::chrono::duration<double>(obj_profile::Clock::now() - start).count();
    if (destination.front() != 0x5a || destination.back() != 0x5a) { throw std::runtime_error("Copy check failed"); }
    return {{"buffer_bytes", bytes}, {"repeats", repeats}, {"payload_GB_per_second", bytes * repeats / seconds / 1e9},
        {"minimum_read_plus_write_GB_per_second", 2 * bytes * repeats / seconds / 1e9}};
}
obj_profile::Json readFile(const std::filesystem::path& path, size_t workerCount, bool native) {
    const size_t workers = workerCount ? workerCount : 1;
    if (workers > 64) { throw std::invalid_argument("At most 64 read workers"); }
    const auto size = std::filesystem::file_size(path);
    constexpr uint64_t block = 256 * 1024;
    const auto blocks = (size + block - 1) / block;
    std::vector<std::future<uint64_t>> pending;
    for (size_t i = 0; i < workers; ++i) {
        pending.push_back(std::async(std::launch::async, [&, i] {
            const auto begin = blocks * i / workers * block;
            const auto finish = std::min(size, blocks * (i + 1) / workers * block);
            if (begin >= finish) { return uint64_t{0}; }
            const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                FILE_FLAG_SEQUENTIAL_SCAN | (native ? FILE_FLAG_NO_BUFFERING : 0), nullptr);
            if (handle == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot open read probe input"); }
            const auto close = [](void* value) { CloseHandle(value); };
            std::unique_ptr<void, decltype(close)> owner(handle, close);
            const auto free = [](char* value) { _aligned_free(value); };
            std::unique_ptr<char, decltype(free)> buffer(static_cast<char*>(_aligned_malloc(block, 4096)), free);
            if (!buffer) { throw std::bad_alloc(); }
            LARGE_INTEGER offset{}; offset.QuadPart = static_cast<LONGLONG>(begin);
            if (!SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN)) { throw std::runtime_error("Cannot seek read probe input"); }
            uint64_t readTotal = 0;
            while (begin + readTotal < finish) {
                DWORD got = 0;
                if (!ReadFile(handle, buffer.get(), static_cast<DWORD>(block), &got, nullptr) || !got) {
                    throw std::runtime_error("Read probe failed");
                }
                readTotal += got;
            }
            if (readTotal != finish - begin) { throw std::runtime_error("Read probe byte count mismatch"); }
            return readTotal;
        }));
    }
    uint64_t total = 0;
    for (auto& future : pending) { total += future.get(); }
    return {{"input_bytes", total}, {"workers", workers}, {"block_bytes", block}, {"no_buffering", native}};
}
struct Fingerprint { uint64_t value = 14695981039346656037ull; };
template <typename T> void hash(Fingerprint& out, const T& value) {
    const auto* data = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(value); ++i) { out.value = (out.value ^ data[i]) * 1099511628211ull; }
}
template <typename T> void hashValues(Fingerprint& out, const T& values) {
    hash(out, values.size());
    for (const auto& value : values) { hash(out, value); }
}
obj_profile::Json meshInfo(const woby::Mesh& mesh) {
    const auto& g = woby::meshGeometry(mesh);
    Fingerprint geometry, normals;
    hash(geometry, mesh.origin);
    if (g.sourcePositions) { hashValues(geometry, *g.sourcePositions); }
    hashValues(geometry, g.indices); hashValues(geometry, g.positionIndices);
    for (const auto& v : g.vertices) {
        hash(geometry, v.position); hash(geometry, v.texcoord); hash(normals, v.normal);
    }
    const auto bytes = g.vertices.capacity()*sizeof(woby::Vertex) + g.indices.capacity()*sizeof(uint32_t)
        + g.positionIndices.capacity()*sizeof(uint32_t)
        + (g.sourcePositions ? g.sourcePositions->capacity()*sizeof(woby::Coordinate) : 0)
        + (mesh.sourceData ? mesh.sourceData->indices.capacity()*sizeof(uint32_t) : 0);
    return {{"source_positions", g.sourcePositions ? g.sourcePositions->size() : 0},
        {"vertices", g.vertices.size()}, {"triangles", g.indices.size()/3}, {"groups", g.nodes.size()},
        {"groups_with_texcoords", std::count_if(g.nodes.begin(), g.nodes.end(), [](const auto& node) { return node.hasTexcoords; })},
        {"geometry_fingerprint", geometry.value}, {"normal_fingerprint", normals.value},
        {"retained_buffer_capacity_bytes", bytes}, {"vertex_bytes", g.vertices.size()*sizeof(woby::Vertex)},
        {"index_bytes", g.indices.size()*sizeof(uint32_t)}, {"vertex_stride", sizeof(woby::Vertex)}};
}
int run(const std::string& mode, const std::filesystem::path& path, size_t workerCount, bool samples) {
    obj_profile::Json output{{"mode", mode}, {"cpu", obj_profile::cpuCapabilities()}, {"workers_requested", workerCount},
        {"clock_origin", obj_profile::clockOrigin()}};
#if defined(__AVX2__)
    output["compiler_isa"] = "AVX2";
#else
    output["compiler_isa"] = "SSE2";
#endif
    obj_profile::startSampling(samples);
    try {
        if (mode == "flop-control") {
            output["stats"] = flopControl();
        } else if (mode == "memory") {
            output["stats"] = memoryCopy();
        } else if (mode == "read-native" || mode == "read-cached") {
            const auto total = obj_profile::begin("read_only");
            output["stats"] = readFile(path, workerCount, mode == "read-native");
            obj_profile::end(total);
        } else if (mode == "parser") {
            const auto total = obj_profile::begin("parser_total");
            std::vector<std::array<double, 3>> positions, normals;
            std::vector<std::array<double, 2>> texcoords;
            rapidobj::PrototypeOptions options; options.workers = workerCount;
            auto parsed = rapidobj::ParseFilePrototype(path, {positions, texcoords, normals}, options);
            obj_profile::end(total);
            if (parsed.polygons.error) { throw std::runtime_error(parsed.polygons.error.code.message()); }
            Fingerprint fingerprint;
            hashValues(fingerprint, positions); hashValues(fingerprint, texcoords); hashValues(fingerprint, normals);
            size_t faces = 0;
            for (const auto& shape : parsed.polygons.shapes) {
                hashValues(fingerprint, shape.mesh.indices); hashValues(fingerprint, shape.mesh.num_face_vertices);
                faces += shape.mesh.num_face_vertices.size();
            }
            output["geometry"] = {{"positions", positions.size()}, {"texcoords", texcoords.size()},
                {"normals", normals.size()}, {"faces", faces}, {"fingerprint", fingerprint.value}};
            output["stats"] = {{"workers", parsed.stats.workers}, {"chunks", parsed.stats.chunks},
                {"input_bytes", parsed.stats.input_bytes}, {"parsed_chunk_bytes", parsed.stats.parsed_chunk_bytes},
                {"peak_inflight_text_bytes", parsed.stats.peak_inflight_text_bytes}};
        } else if (mode == "load" || mode == "prepare" || mode == "annotation" || mode == "replay" || mode == "simd-replay") {
            woby::ObjPrototypeOptions options; options.workers = workerCount;
            woby::ObjPrototypeMetrics metrics;
            const auto total = obj_profile::begin("cpu_import");
            auto mesh = woby::loadObjMeshPrototype(path, {}, options, &metrics);
            obj_profile::end(total);
            if (mode == "prepare") {
                auto group = obj_profile::begin("group_state");
                auto settings = woby::createUiGroupStates(mesh, 0);
                obj_profile::end(group, {{"groups", settings.size()}});
                auto prepare = obj_profile::begin("scene_cpu_preparation");
                const auto data = woby::prepareSceneMesh(mesh);
                if (!data) { throw std::runtime_error("Unexpected cancellation"); }
                obj_profile::end(prepare, {{"upload_bytes", data->uploadBytes}, {"has_point_cloud", bool(data->pointCloud)}});
            } else if (mode == "replay") {
                output["replay"] = obj_profile::deepReplay(mesh);
            } else if (mode == "simd-replay") {
                output["simd_replay"] = obj_profile::simdReplay(mesh);
            } else if (mode == "annotation") {
                const auto cachePhase = obj_profile::begin("annotation_cache");
                const auto cache = woby::buildAnnotationMeshCache(mesh);
                obj_profile::end(cachePhase, {{"blocks", cache->blocks.size()}});
            }
            const auto check = obj_profile::begin("geometry_validation");
            output["geometry"] = meshInfo(mesh);
            obj_profile::end(check);
            output["stats"] = {{"workers", metrics.workers}, {"chunks", metrics.chunks},
                {"input_bytes", metrics.inputBytes}, {"parsed_chunk_bytes", metrics.parsedChunkBytes},
                {"peak_inflight_text_bytes", metrics.peakInflightTextBytes}};
        } else { throw std::invalid_argument("Expected parser, load, prepare, annotation, read-native, read-cached, or memory"); }
        output.update(obj_profile::result());
        output.update(obj_profile::stopSampling());
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        (void)obj_profile::stopSampling();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 5) { std::cerr << "Usage: woby_obj_profile MODE path [workers] [sample]\n"; return 2; }
    try {
        return run(std::filesystem::path(argv[1]).string(), std::filesystem::path(argv[2]),
            argc > 3 ? std::stoull(argv[3]) : 0, argc > 4 && std::wstring_view(argv[4]) == L"sample");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
