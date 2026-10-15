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
#include <unordered_set>
#include <utility>
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <xmmintrin.h>
#endif

namespace woby::obj_mapping {
inline constexpr uint32_t empty = std::numeric_limits<uint32_t>::max();
inline constexpr uint32_t invalidUv = uint32_t{1} << 31;
// Only newly created primitive vertices need placeholder normals. Face IDs
// are resolved first, so a shared face ID must retain its authored/missing normal.
inline constexpr uint32_t unlit = uint32_t{1} << 30;
inline constexpr uint32_t idMask = unlit - 1;
static_assert(std::numeric_limits<SceneBufferSize>::max() / sizeof(Vertex) < unlit);

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
    const std::array<double,2>* preciseUvs, const float* floatUvs, bool allFiniteUvs = false) {
    if (key.uv < 0) { return id | invalidUv; }
    if (allFiniteUvs) { return id; }
    const auto uv = static_cast<size_t>(key.uv);
    const float u = Precise ? static_cast<float>(preciseUvs[uv][0]) : floatUvs[uv*2];
    const float v = Precise ? static_cast<float>(preciseUvs[uv][1]) : floatUvs[uv*2+1];
    return std::isfinite(u) && std::isfinite(v) ? id : id | invalidUv;
}
template<bool Precise, bool Unlit = false>
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
    const auto id = tagId<Precise>(key,table.next,preciseUvs,floatUvs) | (Unlit ? unlit : 0);
    const auto slot = static_cast<uint32_t>(table.seams.size());
    table.seams.push_back({key,id}); table.buckets[bucket] = slot; ++table.next;
    return id;
}

template<bool Precise, bool AllFiniteUvs, bool Unlit = false>
inline uint32_t resolveId(Table& table, Primary* primary, size_t positionCount, const Key& key,
    const std::array<double,2>* preciseUvs, const float* floatUvs) {
    if (static_cast<size_t>(key.position) >= positionCount) {
        throw std::runtime_error("OBJ contains an invalid source position index.");
    }
    auto& entry = primary[static_cast<size_t>(key.position)];
    if (entry.id == empty) {
        (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
        const auto id = tagId<Precise>(key,table.next,preciseUvs,floatUvs,AllFiniteUvs) | (Unlit ? unlit : 0);
        entry = {id,key.normal,key.uv}; ++table.next;
        return id;
    }
    if (entry.normal == key.normal && entry.uv == key.uv) { return entry.id; }
    return seamId<Precise,Unlit>(table,key,preciseUvs,floatUvs);
}

inline void prefetchRead(const void* address) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_prefetch(static_cast<const char*>(address),_MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(address,0,3);
#else
    (void)address;
#endif
}

template<bool Precise>
inline bool allUvsFinite(const std::array<double,2>* preciseUvs, const float* floatUvs,
    size_t uvCount, const ModelLoadProgressCallback& progress, size_t corners, size_t totalIndices = 0) {
    // A scan of unused UV records must not cost more than the corner pass.
    if (uvCount > corners) { return false; }
    constexpr size_t chunkSize = 65536;
    for (size_t begin = 0; begin < uvCount; begin += chunkSize) {
        reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,0,totalIndices ? totalIndices : corners);
        const auto end = std::min(uvCount,begin+chunkSize);
        for (size_t uv = begin; uv < end; ++uv) {
            const float u = Precise ? static_cast<float>(preciseUvs[uv][0]) : floatUvs[uv*2];
            const float v = Precise ? static_cast<float>(preciseUvs[uv][1]) : floatUvs[uv*2+1];
            if (!std::isfinite(u) || !std::isfinite(v)) { return false; }
        }
    }
    return true;
}
template<bool Precise, bool Deferred, bool AllFiniteUvs>
inline void resolveFaceIndices(const rapidobj::Result& result, Table& table,
    MeshGeometry& geometry, SourceMeshData& source, const rapidobj::GeometryBuffers* buffers,
    const ModelLoadProgressCallback& progress, size_t totalIndices) {
    auto* indices = geometry.indices.data(); auto* sourceIndices = source.indices.data();
    auto* primary = table.primary.data();
    const auto positionCount = table.primary.size();
    const auto* preciseUvs = Precise ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = result.attributes.texcoords.data();
    const auto uvCount = Precise ? buffers->texcoords.size() : result.attributes.texcoords.size()/2;
    size_t completed = 0;
    for (size_t shapeIndex = 0; shapeIndex < result.shapes.size(); ++shapeIndex) {
        const auto& shape = result.shapes[shapeIndex];
        const auto offset = static_cast<uint32_t>(completed);
        uint32_t uvFlags = 0;
        constexpr size_t prefetchDistance = 32;
        const auto count = shape.mesh.indices.size();
        if (!count) { continue; }
        // Primary entries are usually local. The first-use UV reads can miss
        // the cache; fetch them ahead only when the finite scan did not pass.
        const auto* prefetchEnd = shape.mesh.indices.data()
            + (count > prefetchDistance ? count-prefetchDistance : 0);
        for (const auto& index : shape.mesh.indices) {
            if ((completed & 16383) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,completed,totalIndices); }
            if constexpr (!AllFiniteUvs) {
                if (&index < prefetchEnd) {
                    const auto uv = static_cast<size_t>((&index)[prefetchDistance].texcoord_index);
                    if (uv < uvCount) {
                        if constexpr (Precise) { prefetchRead(preciseUvs+uv); }
                        else { prefetchRead(floatUvs+uv*2); }
                    }
                }
            }
            if (static_cast<size_t>(index.position_index) >= positionCount) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            const Key key{index.position_index,index.normal_index,index.texcoord_index};
            // Keep the face lookup in this loop. A shared resolveId call slows
            // seam-heavy models on MSVC, even when the compiler inlines it.
            auto& entry = primary[static_cast<size_t>(key.position)];
            uint32_t id;
            if (entry.id == empty) {
                (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
                id = tagId<Precise>(key,table.next,preciseUvs,floatUvs,AllFiniteUvs);
                entry = {id,key.normal,key.uv}; ++table.next;
            } else if (entry.normal == key.normal && entry.uv == key.uv) { id = entry.id; }
            else { id = seamId<Precise>(table,key,preciseUvs,floatUvs); }
            uvFlags |= id;
            indices[completed] = id & idMask;
            if constexpr (!Deferred) { sourceIndices[completed] = static_cast<uint32_t>(key.position); }
            ++completed;
        }
        const auto written = static_cast<uint32_t>(completed)-offset;
        if (written) {
            MeshNode node; node.name = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
            node.indexOffset = offset; node.indexCount = written; node.hasTexcoords = !(uvFlags & invalidUv);
            geometry.nodes.push_back(std::move(node));
        }
    }
}
template<bool Precise>
inline void resolveFaces(const rapidobj::Result& result, Table& table,
    MeshGeometry& geometry, SourceMeshData& source, const rapidobj::GeometryBuffers* buffers,
    const ModelLoadProgressCallback& progress, size_t corners, bool deferred, size_t totalIndices = 0) {
    table.primary.resize(sourcePositions(source).size());
    table.seams.reserve(std::min({sourcePositions(source).size(),corners,size_t{1024}}));
    geometry.indices.resize(corners);
    if (!deferred) { source.indices.resize(corners); }
    const auto* preciseUvs = Precise ? buffers->texcoords.data() : nullptr;
    const auto* floatUvs = result.attributes.texcoords.data();
    const auto uvCount = Precise ? buffers->texcoords.size() : result.attributes.texcoords.size()/2;
    if (!totalIndices) { totalIndices = corners; }
    const bool allFiniteUvs = allUvsFinite<Precise>(preciseUvs,floatUvs,uvCount,progress,corners,totalIndices);
    if (deferred) {
        if (allFiniteUvs) { resolveFaceIndices<Precise,true,true>(result,table,geometry,source,buffers,progress,totalIndices); }
        else { resolveFaceIndices<Precise,true,false>(result,table,geometry,source,buffers,progress,totalIndices); }
    } else {
        if (allFiniteUvs) { resolveFaceIndices<Precise,false,true>(result,table,geometry,source,buffers,progress,totalIndices); }
        else { resolveFaceIndices<Precise,false,false>(result,table,geometry,source,buffers,progress,totalIndices); }
    }
}
template<bool Precise>
inline void resolvePrimitives(const rapidobj::Result& result, Table& table, MeshGeometry& geometry,
    const ModelLoadProgressCallback& progress, size_t corners, size_t totalIndices) {
    std::unordered_set<std::string> names;
    for (const auto& node : geometry.nodes) { names.insert(node.name); }
    const auto groupName = [&](const std::string& base) {
        std::string name = base;
        for (size_t suffix = 2; !names.insert(name).second; ++suffix) { name = base+" ("+std::to_string(suffix)+")"; }
        return name;
    };
    size_t lineWritten = 0, pointWritten = 0, visits = 0;
    auto* primary = table.primary.data(); const auto positions = table.primary.size();
    const auto primitiveVertex = [&](int position) {
        const auto id = resolveId<Precise,true,true>(table,primary,positions,{position,-1,-1},nullptr,nullptr);
        if ((visits++ & 16383) == 0) {
            reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners+lineWritten+pointWritten,totalIndices);
        }
        return id & idMask;
    };
    // Face IDs and groups precede all explicit line and point groups.
    for (size_t shapeIndex = 0; shapeIndex < result.shapes.size(); ++shapeIndex) {
        const auto& shape = result.shapes[shapeIndex];
        const auto base = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
        if (!shape.lines.indices.empty()) {
            MeshNode node; node.name = groupName(base+(shape.mesh.indices.empty() ? "" : " (lines)"));
            node.lineIndexOffset = static_cast<uint32_t>(lineWritten);
            size_t begin = 0;
            for (const auto count : shape.lines.num_line_vertices) {
                auto previous = primitiveVertex(shape.lines.indices[begin].position_index);
                for (size_t i = 1; i < static_cast<size_t>(count); ++i) {
                    const auto next = primitiveVertex(shape.lines.indices[begin+i].position_index);
                    geometry.lineIndices[lineWritten++] = previous;
                    geometry.lineIndices[lineWritten++] = next;
                    previous = next;
                }
                begin += static_cast<size_t>(count);
            }
            node.lineIndexCount = static_cast<uint32_t>(lineWritten)-node.lineIndexOffset;
            geometry.nodes.push_back(std::move(node));
        }
        if (!shape.points.indices.empty()) {
            MeshNode node; node.name = groupName(base+(shape.mesh.indices.empty() && shape.lines.indices.empty() ? "" : " (points)"));
            node.pointIndexOffset = static_cast<uint32_t>(pointWritten);
            for (const auto& index : shape.points.indices) { geometry.pointIndices[pointWritten++] = primitiveVertex(index.position_index); }
            node.pointIndexCount = static_cast<uint32_t>(pointWritten)-node.pointIndexOffset;
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
template<bool Precise, bool PrimaryPass, bool Cloud = false>
inline size_t gatherPass(MeshGeometry& geometry, const Table& table, const Coordinate* positions,
    const rapidobj::GeometryBuffers* buffers, const rapidobj::Attributes& attributes,
    const ModelLoadProgressCallback& progress, size_t corners, size_t requestedWorkers) {
    constexpr size_t chunkSize = 65536;
    const auto count = Cloud ? geometry.vertices.size() : PrimaryPass ? table.primary.size() : table.seams.size();
    const auto chunks = (count+chunkSize-1)/chunkSize;
    if (!chunks) { return 0; }
    const auto limit = requestedWorkers ? requestedWorkers : std::min(size_t{8},std::max(size_t{1},size_t(std::thread::hardware_concurrency())));
    const auto workers = std::min(limit,chunks);
    std::atomic<size_t> next = 0; std::atomic<bool> canceled = false;
    std::vector<std::exception_ptr> errors(workers);
    auto* vertices = geometry.vertices.data();
    auto* colors = geometry.vertexColors.empty() ? nullptr : geometry.vertexColors.data();
    const auto sourceColor = [&](size_t position) {
        if constexpr (Precise) { return (*buffers->vertex_colors)[position]; }
        else {
            uint32_t color = UINT32_C(0xff000000);
            for (size_t axis = 0; axis < 3; ++axis) {
                const double value = attributes.colors[position * 3 + axis];
                if (!std::isfinite(value)) { throw std::runtime_error("OBJ contains a nonfinite vertex color."); }
                color |= rapidobj::detail::PrototypeColorByte(value) << (axis * 8);
            }
            return color;
        }
    };
    const auto work = [&](size_t worker) {
        try {
            while (!canceled.load(std::memory_order_relaxed)) {
                const auto chunk = next.fetch_add(1,std::memory_order_relaxed);
                if (chunk >= chunks) { break; }
                // Only the caller reports progress or polls cancellation.
                if (worker == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,corners,corners); }
                const auto end = std::min(count,(chunk+1)*chunkSize);
                for (size_t i = chunk*chunkSize; i < end; ++i) {
                    if constexpr (Cloud) {
                        vertices[i] = {renderPosition(positions[i]),{0,0,1},{}};
                        if (colors) { colors[i] = sourceColor(i); }
                        geometry.pointIndices[i] = static_cast<uint32_t>(i);
                        continue;
                    }
                    Key key; uint32_t id;
                    if constexpr (PrimaryPass) {
                        const auto& entry = table.primary[i]; id = entry.id;
                        if (id == empty) { continue; }
                        key = {static_cast<int>(i),entry.normal,entry.uv};
                    } else { const auto& seam = table.seams[i]; key = seam.key; id = seam.id; }
                    vertices[id & idMask] = gatherVertex<Precise>(key,positions,buffers,attributes);
                    if (colors) { colors[id & idMask] = sourceColor(static_cast<size_t>(key.position)); }
                    if (id & unlit) { vertices[id & idMask].normal = {0,0,1}; }
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
inline void buildGeometry(rapidobj::Result& result, MeshGeometry& geometry, SourceMeshData& source,
    const rapidobj::GeometryBuffers* buffers, const ModelLoadProgressCallback& progress,
    const ObjMeshCounts& counts, bool deferred, size_t workers, ObjCornerMappingMetrics& metrics) {
    metrics.staged = true;
    const auto corners = counts.triangleIndices;
    const auto totalIndices = corners+counts.lineIndices+counts.pointIndices;
    const bool colored = Precise ? buffers->vertex_colors && !buffers->vertex_colors->empty() : !result.attributes.colors.empty();
    auto start = Clock::now();
    reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,0,totalIndices);
    geometry.nodes.reserve(result.shapes.size());
    geometry.lineIndices.resize(counts.lineIndices); geometry.pointIndices.resize(counts.pointIndices);
    if (counts.vertexCloud) {
        const auto positions = sourcePositions(source).size();
        if (!positions) { return; }
        geometry.vertices.resize(positions); metrics.vertexAllocationMs = elapsed(start);
        if (colored) { geometry.vertexColors.resize(positions); }
        MeshNode node; node.name = "Points"; node.pointIndexCount = static_cast<uint32_t>(positions);
        geometry.nodes.push_back(std::move(node));
        start = Clock::now();
        metrics.gatherWorkers = gatherPass<Precise,true,true>(geometry,Table{},sourcePositions(source).data(),buffers,result.attributes,progress,totalIndices,workers);
        metrics.primaryGatherMs = elapsed(start);
        return;
    }
    if (!totalIndices) { return; } // Freeform-only files have no OBJ primitive IDs.
    Table table;
    resolveFaces<Precise>(result,table,geometry,source,buffers,progress,corners,deferred,totalIndices);
    if (counts.lineIndices || counts.pointIndices) {
        resolvePrimitives<Precise>(result,table,geometry,progress,corners,totalIndices);
    }
    metrics.resolveMs = elapsed(start);
    metrics.primaryCapacityBytes = table.primary.capacity()*sizeof(Primary);
    metrics.keyCapacityBytes = table.seams.capacity()*sizeof(Seam);
    metrics.bucketCapacityBytes = table.buckets.capacity()*sizeof(uint32_t);
    metrics.seamVertices = table.seams.size();
    start = Clock::now();
    for (auto& shape : result.shapes) {
        reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,totalIndices,totalIndices);
        metrics.releasedTriangleBytes += shape.mesh.indices.size()*sizeof(rapidobj::Index)
            + shape.mesh.num_face_vertices.size()*sizeof(uint8_t)
            + shape.mesh.material_ids.size()*sizeof(int32_t)+shape.mesh.smoothing_group_ids.size()*sizeof(uint32_t);
        shape.mesh = {};
        shape.lines = {}; shape.points = {};
    }
    metrics.releaseMs = elapsed(start);
    start = Clock::now(); reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,totalIndices,totalIndices);
    geometry.vertices.resize(table.next); metrics.vertexAllocationMs = elapsed(start);
    if (colored) { geometry.vertexColors.resize(table.next); }
    start = Clock::now();
    bool identity = table.seams.empty();
    if (identity) {
        for (size_t i = 0; i < table.primary.size(); ++i) {
            if ((i & 65535) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,totalIndices,totalIndices); }
            const auto id = table.primary[i].id;
            if (id != empty && (id & idMask) != i) { identity = false; break; }
        }
    }
    if (!identity) {
        geometry.positionIndexing = PositionIndexing::mapped; geometry.positionIndices.resize(table.next);
        for (size_t i = 0; i < table.primary.size(); ++i) {
            if ((i & 65535) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,totalIndices,totalIndices); }
            const auto id = table.primary[i].id;
            if (id != empty) { geometry.positionIndices[id & idMask] = static_cast<uint32_t>(i); }
        }
        for (size_t i = 0; i < table.seams.size(); ++i) {
            if ((i & 65535) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,totalIndices,totalIndices); }
            const auto& seam = table.seams[i]; geometry.positionIndices[seam.id & idMask] = static_cast<uint32_t>(seam.key.position);
        }
    }
    metrics.positionMapMs = elapsed(start);
    start = Clock::now();
    metrics.gatherWorkers = gatherPass<Precise,true>(geometry,table,sourcePositions(source).data(),buffers,result.attributes,progress,totalIndices,workers);
    metrics.primaryGatherMs = elapsed(start); start = Clock::now();
    const auto seamWorkers = gatherPass<Precise,false>(geometry,table,sourcePositions(source).data(),buffers,result.attributes,progress,totalIndices,workers);
    metrics.gatherWorkers = std::max(metrics.gatherWorkers,seamWorkers); metrics.seamGatherMs = elapsed(start);
}
} // namespace woby::obj_mapping
