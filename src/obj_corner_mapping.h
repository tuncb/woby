#pragma once

#include "obj_mesh.h"
#include "scene_buffer_size.h"
#include <rapidobj/rapidobj.hpp>
#include <rapidobj/prototype.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace woby::obj_mapping {
inline constexpr uint32_t empty = std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t invalidUv = uint32_t{1} << 31;
inline constexpr uint32_t idMask = invalidUv - 1;
static_assert(std::numeric_limits<SceneBufferSize>::max() / sizeof(Vertex) < invalidUv);

struct Key {
    int position = -1, normal = -1, uv = -1;
    bool operator==(const Key&) const noexcept = default;
};
struct Primary { uint32_t id = empty; int normal = -1, uv = -1; };
struct Seam { Key key; uint32_t id = empty; };
struct Table {
    std::vector<Primary> primary;
    std::vector<Seam> seams;
    std::vector<uint32_t> buckets;
    uint32_t next = 0;
};
static_assert(sizeof(Primary) == 12 && sizeof(Seam) == 16);
using Clock = std::chrono::steady_clock;
inline double elapsed(Clock::time_point start) {
    return std::chrono::duration<double,std::milli>(Clock::now()-start).count();
}
inline size_t hashKey(const Key& key) {
    uint64_t hash = uint64_t(static_cast<uint32_t>(key.position))*0x9e3779b185ebca87ull;
    hash ^= uint64_t(static_cast<uint32_t>(key.normal))*0xc2b2ae3d27d4eb4full;
    hash ^= uint64_t(static_cast<uint32_t>(key.uv))*0x165667b19e3779f9ull;
    hash ^= hash >> 33; hash *= 0xff51afd7ed558ccdull;
    return static_cast<size_t>(hash ^ (hash >> 33));
}
inline void growBuckets(Table& table) {
    std::vector<uint32_t> buckets(std::max(size_t{16},table.buckets.size()*2),empty);
    for (const auto slot : table.buckets) {
        if (slot == empty) { continue; }
        auto bucket = hashKey(table.seams[slot].key) & (buckets.size()-1);
        while (buckets[bucket] != empty) { bucket = (bucket+1) & (buckets.size()-1); }
        buckets[bucket] = slot;
    }
    table.buckets = std::move(buckets);
}
template<bool Precise>
inline uint32_t tagId(const Key& key, uint32_t id,
    const std::array<double,2>* preciseUvs, const float* floatUvs) {
    if (key.uv < 0) { return id | invalidUv; }
    const auto uv = static_cast<size_t>(key.uv);
    const float u = Precise ? static_cast<float>(preciseUvs[uv][0]) : floatUvs[uv*2];
    const float v = Precise ? static_cast<float>(preciseUvs[uv][1]) : floatUvs[uv*2+1];
    return std::isfinite(u) && std::isfinite(v) ? id : id | invalidUv;
}
template<bool Precise>
inline uint32_t seamId(Table& table, const Key& key,
    const std::array<double,2>* preciseUvs, const float* floatUvs) {
    if (table.buckets.empty()) { growBuckets(table); }
    auto bucket = hashKey(key) & (table.buckets.size()-1);
    while (table.buckets[bucket] != empty) {
        const auto& seam = table.seams[table.buckets[bucket]];
        if (seam.key == key) { return seam.id; }
        bucket = (bucket+1) & (table.buckets.size()-1);
    }
    (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
    if (table.seams.size() >= table.buckets.size()*3/4) {
        growBuckets(table); bucket = hashKey(key) & (table.buckets.size()-1);
        while (table.buckets[bucket] != empty) { bucket = (bucket+1) & (table.buckets.size()-1); }
    }
    const auto id = tagId<Precise>(key,table.next,preciseUvs,floatUvs);
    const auto slot = static_cast<uint32_t>(table.seams.size());
    table.seams.push_back({key,id}); table.buckets[bucket] = slot; ++table.next;
    return id;
}
template<bool Precise>
inline void resolveFaces(const rapidobj::Result& result, Table& table,
    MeshGeometry& geometry, SourceMeshData& source, const rapidobj::GeometryBuffers* buffers,
    const ModelLoadProgressCallback& progress, size_t corners, bool deferred) {
    table.primary.resize(sourcePositions(source).size());
    table.seams.reserve(std::min({sourcePositions(source).size(),corners,size_t{1024}}));
    geometry.indices.resize(corners);
    if (!deferred) { source.indices.resize(corners); }
    auto* indices = geometry.indices.data(); auto* sourceIndices = source.indices.data();
    auto* primary = table.primary.data();
    const auto* preciseUvs = Precise ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = result.attributes.texcoords.data();
    size_t completed = 0;
    for (size_t shapeIndex = 0; shapeIndex < result.shapes.size(); ++shapeIndex) {
        const auto& shape = result.shapes[shapeIndex];
        const auto offset = static_cast<uint32_t>(completed);
        bool hasTexcoords = true;
        for (const auto& index : shape.mesh.indices) {
            if ((completed & 16383) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,completed,corners); }
            if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= table.primary.size()) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            const Key key{index.position_index,index.normal_index,index.texcoord_index};
            auto& entry = primary[static_cast<size_t>(key.position)];
            uint32_t id;
            if (entry.id == empty) {
                (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
                id = tagId<Precise>(key,table.next,preciseUvs,floatUvs);
                entry = {id,key.normal,key.uv}; ++table.next;
            } else if (entry.normal == key.normal && entry.uv == key.uv) { id = entry.id; }
            else { id = seamId<Precise>(table,key,preciseUvs,floatUvs); }
            hasTexcoords = hasTexcoords && !(id & invalidUv);
            indices[completed] = id & idMask;
            if (!deferred) { sourceIndices[completed] = static_cast<uint32_t>(key.position); }
            ++completed;
        }
        const auto count = static_cast<uint32_t>(completed)-offset;
        if (count) {
            MeshNode node; node.name = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
            node.indexOffset = offset; node.indexCount = count; node.hasTexcoords = hasTexcoords;
            geometry.nodes.push_back(std::move(node));
        }
    }
}
template<bool Precise>
inline Vertex gatherVertex(const Key& key, const Coordinate* positions,
    const rapidobj::GeometryBuffers* buffers, const rapidobj::Attributes& attributes) {
    Vertex vertex{}; vertex.position = renderPosition(positions[static_cast<size_t>(key.position)]);
    if (key.normal >= 0) {
        const auto normal = static_cast<size_t>(key.normal);
        for (size_t axis = 0; axis < 3; ++axis) {
            if constexpr (Precise) { vertex.normal[axis] = static_cast<float>(buffers->normals[normal][axis]); }
            else { vertex.normal[axis] = attributes.normals[normal*3+axis]; }
        }
    }
    if (key.uv >= 0) {
        const auto uv = static_cast<size_t>(key.uv);
        if constexpr (Precise) { vertex.texcoord = {static_cast<float>(buffers->texcoords[uv][0]),1.0f-static_cast<float>(buffers->texcoords[uv][1])}; }
        else { vertex.texcoord = {attributes.texcoords[uv*2],1.0f-attributes.texcoords[uv*2+1]}; }
    }
    return vertex;
}
template<bool Precise, bool PrimaryPass>
inline size_t gatherPass(MeshGeometry& geometry, const Table& table, const Coordinate* positions,
    const rapidobj::GeometryBuffers* buffers, const rapidobj::Attributes& attributes,
    const ModelLoadProgressCallback& progress, size_t corners, size_t requestedWorkers) {
    constexpr size_t chunkSize = 65536;
    const auto count = PrimaryPass ? table.primary.size() : table.seams.size();
    const auto chunks = (count+chunkSize-1)/chunkSize;
    if (!chunks) { return 0; }
    const auto limit = requestedWorkers ? requestedWorkers : std::min(size_t{8},std::max(size_t{1},size_t(std::thread::hardware_concurrency())));
    const auto workers = std::min(limit,chunks);
    std::atomic<size_t> next = 0; std::atomic<bool> canceled = false;
    std::vector<std::exception_ptr> errors(workers);
    auto* vertices = geometry.vertices.data();
    const auto work = [&](size_t worker) {
        try {
            while (!canceled.load(std::memory_order_relaxed)) {
                const auto chunk = next.fetch_add(1,std::memory_order_relaxed);
                if (chunk >= chunks) { break; }
                // Only the caller reports progress or polls cancellation.
                if (worker == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners); }
                const auto end = std::min(count,(chunk+1)*chunkSize);
                for (size_t i = chunk*chunkSize; i < end; ++i) {
                    Key key; uint32_t id;
                    if constexpr (PrimaryPass) {
                        const auto& entry = table.primary[i]; id = entry.id;
                        if (id == empty) { continue; }
                        key = {static_cast<int>(i),entry.normal,entry.uv};
                    } else { const auto& seam = table.seams[i]; key = seam.key; id = seam.id; }
                    vertices[id & idMask] = gatherVertex<Precise>(key,positions,buffers,attributes);
                }
            }
        } catch (...) { errors[worker] = std::current_exception(); canceled.store(true,std::memory_order_relaxed); }
    };
    std::vector<std::jthread> threads; threads.reserve(workers-1);
    try { for (size_t worker = 1; worker < workers; ++worker) { threads.emplace_back(work,worker); } }
    catch (...) { canceled.store(true,std::memory_order_relaxed); throw; }
    work(0);
    for (auto& thread : threads) { thread.join(); }
    for (const auto& error : errors) { if (error) { std::rethrow_exception(error); } }
    reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners);
    return workers;
}
template<bool Precise>
inline void buildFaces(rapidobj::Result& result, MeshGeometry& geometry, SourceMeshData& source,
    const rapidobj::GeometryBuffers* buffers, const ModelLoadProgressCallback& progress,
    size_t corners, bool deferred, size_t workers, ObjCornerMappingMetrics& metrics) {
    Table table; metrics.staged = true;
    auto start = Clock::now();
    resolveFaces<Precise>(result,table,geometry,source,buffers,progress,corners,deferred);
    metrics.resolveMs = elapsed(start);
    metrics.primaryCapacityBytes = table.primary.capacity()*sizeof(Primary);
    metrics.keyCapacityBytes = table.seams.capacity()*sizeof(Seam);
    metrics.bucketCapacityBytes = table.buckets.capacity()*sizeof(uint32_t);
    start = Clock::now();
    for (auto& shape : result.shapes) {
        reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners);
        metrics.releasedTriangleBytes += shape.mesh.indices.size()*sizeof(rapidobj::Index)
            + shape.mesh.num_face_vertices.size()*sizeof(uint8_t)
            + shape.mesh.material_ids.size()*sizeof(int32_t)+shape.mesh.smoothing_group_ids.size()*sizeof(uint32_t);
        shape.mesh = {};
    }
    metrics.releaseMs = elapsed(start);
    start = Clock::now(); reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners);
    geometry.vertices.resize(table.next); metrics.vertexAllocationMs = elapsed(start);
    start = Clock::now();
    bool identity = table.seams.empty();
    if (identity) {
        for (size_t i = 0; i < table.primary.size(); ++i) {
            if ((i & 65535) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners); }
            const auto id = table.primary[i].id;
            if (id != empty && (id & idMask) != i) { identity = false; break; }
        }
    }
    if (!identity) {
        geometry.positionIndexing = PositionIndexing::mapped; geometry.positionIndices.resize(table.next);
        for (size_t i = 0; i < table.primary.size(); ++i) {
            if ((i & 65535) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners); }
            const auto id = table.primary[i].id;
            if (id != empty) { geometry.positionIndices[id & idMask] = static_cast<uint32_t>(i); }
        }
        for (size_t i = 0; i < table.seams.size(); ++i) {
            if ((i & 65535) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners); }
            const auto& seam = table.seams[i]; geometry.positionIndices[seam.id & idMask] = static_cast<uint32_t>(seam.key.position);
        }
    }
    metrics.positionMapMs = elapsed(start);
    start = Clock::now();
    metrics.gatherWorkers = gatherPass<Precise,true>(geometry,table,sourcePositions(source).data(),buffers,result.attributes,progress,corners,workers);
    metrics.primaryGatherMs = elapsed(start); start = Clock::now();
    const auto seamWorkers = gatherPass<Precise,false>(geometry,table,sourcePositions(source).data(),buffers,result.attributes,progress,corners,workers);
    metrics.gatherWorkers = std::max(metrics.gatherWorkers,seamWorkers); metrics.seamGatherMs = elapsed(start);
}
} // namespace woby::obj_mapping
