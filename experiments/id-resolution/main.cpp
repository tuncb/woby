#include "obj_corner_mapping.h"
#include "baseline.h"
#include <nlohmann/json.hpp>
#include <cstring>
#include <fstream>
#include <iostream>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace probe {
using namespace woby;
using namespace woby::obj_mapping;
using Json = nlohmann::json;

inline constexpr auto vacant = woby::obj_mapping::empty;
struct alignas(16) Wide { uint32_t id = vacant; int normal = -1, uv = -1; uint32_t padding = 0; };
struct alignas(16) Packed { uint64_t attributes = 0; uint32_t id = vacant, padding = 0; };
struct Reordered { int normal = -1, uv = -1; uint32_t id = vacant; };
struct Compact { uint32_t id = vacant; int uv = -1; };
struct Pair { uint32_t id = vacant; int uv = -1, normal = -1; };
struct Split { std::vector<uint32_t> ids; std::vector<uint64_t> attributes; };

inline uint64_t attributes(const Key& key) {
    return uint64_t(static_cast<uint32_t>(key.normal)) | uint64_t(static_cast<uint32_t>(key.uv)) << 32;
}
template<typename Entry> inline uint32_t entryId(const std::vector<Entry>& entries, size_t position) {
    return entries[position].id;
}
inline uint32_t entryId(const Split& entries, size_t position) { return entries.ids[position]; }
template<typename Entry> inline bool matches(const std::vector<Entry>& entries, size_t position, const Key& key) {
    return entries[position].normal == key.normal && entries[position].uv == key.uv;
}
inline bool matches(const std::vector<Packed>& entries, size_t position, const Key& key) {
    return entries[position].attributes == attributes(key);
}
inline bool matches(const Split& entries, size_t position, const Key& key) {
    return entries.attributes[position] == attributes(key);
}
inline bool matches(const std::vector<Compact>& entries, size_t position, const Key& key) {
    return key.normal == -1 && entries[position].uv == key.uv;
}
inline bool matches(const std::vector<Pair>& entries, size_t position, const Key& key) {
    uint64_t pair;
    std::memcpy(&pair,&entries[position].uv,sizeof(pair));
    const auto expected = uint64_t(static_cast<uint32_t>(key.uv)) | uint64_t(static_cast<uint32_t>(key.normal)) << 32;
    return pair == expected;
}
template<typename Entry> inline void insert(std::vector<Entry>& entries, size_t position, const Key& key, uint32_t id) {
    entries[position].id = id; entries[position].normal = key.normal; entries[position].uv = key.uv;
}
inline void insert(std::vector<Packed>& entries, size_t position, const Key& key, uint32_t id) {
    entries[position] = {attributes(key),id};
}
inline void insert(Split& entries, size_t position, const Key& key, uint32_t id) {
    entries.ids[position] = id; entries.attributes[position] = attributes(key);
}
inline void insert(std::vector<Compact>& entries, size_t position, const Key& key, uint32_t id) {
    if (key.normal != -1) { throw std::runtime_error("Compact UV entries require absent normals."); }
    entries[position] = {id,key.uv};
}
template<typename Entry> inline void allocate(std::vector<Entry>& entries, size_t count) { entries.resize(count); }
inline void allocate(Split& entries, size_t count) { entries.ids.resize(count,vacant); entries.attributes.resize(count); }

template<int Hint, typename Entry> inline void prefetch(const std::vector<Entry>& entries, size_t position) {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(reinterpret_cast<const char*>(&entries[position]), Hint);
#else
    (void)entries; (void)position;
#endif
}
template<int Hint> inline void prefetch(const Split& entries, size_t position) {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_prefetch(reinterpret_cast<const char*>(&entries.ids[position]), Hint);
    _mm_prefetch(reinterpret_cast<const char*>(&entries.attributes[position]), Hint);
#else
    (void)entries; (void)position;
#endif
}

struct Reference { Table table; MeshGeometry geometry; };
template<typename Entries> void validate(const Entries& entries, const Table& table, const MeshGeometry& geometry,
    const Reference& reference, size_t positions) {
    if (geometry.indices != reference.geometry.indices || table.next != reference.table.next
        || table.seams.size() != reference.table.seams.size() || geometry.nodes.size() != reference.geometry.nodes.size()) {
        throw std::runtime_error("Resolved IDs or nodes differ from the production reference.");
    }
    for (size_t i = 0; i < positions; ++i) {
        const auto& expected = reference.table.primary[i];
        if (entryId(entries,i) != expected.id || (expected.id != vacant
            && !matches(entries,i,Key{static_cast<int>(i),expected.normal,expected.uv}))) {
            throw std::runtime_error("Primary entry differs from the production reference.");
        }
    }
    for (size_t i = 0; i < table.seams.size(); ++i) {
        if (table.seams[i].key != reference.table.seams[i].key || table.seams[i].id != reference.table.seams[i].id) {
            throw std::runtime_error("Seam entry differs from the production reference.");
        }
    }
    for (size_t i = 0; i < geometry.nodes.size(); ++i) {
        const auto& a = geometry.nodes[i]; const auto& b = reference.geometry.nodes[i];
        if (a.indexOffset != b.indexOffset || a.indexCount != b.indexCount || a.hasTexcoords != b.hasTexcoords || a.name != b.name) {
            throw std::runtime_error("Group attributes differ from the production reference.");
        }
    }
}

template<typename Entries, size_t Ahead, int Hint = 3, bool Batched = false, bool MatchFirst = false,
    bool AccumulateFlags = false, bool LazyKey = false, bool PrefetchUvs = false, bool PrefetchPrimary = true,
    bool ValidateUvs = false>
Json resolve(const rapidobj::Result& result, SourceMeshData& source, const rapidobj::GeometryBuffers& buffers,
    size_t corners, const Reference& reference) {
    Table table; MeshGeometry geometry; Entries entries;
    const auto positions = sourcePositions(source).size();
    const auto start = Clock::now(); allocate(entries,positions);
    table.seams.reserve(std::min({positions,corners,size_t{1024}}));
    geometry.indices.resize(corners);
    const auto allocatedMs = elapsed(start);
    const auto* preciseUvs = buffers.texcoords.data();
    const auto validationStart = Clock::now();
    bool allFiniteUvs = false;
    if constexpr (ValidateUvs) {
        allFiniteUvs = true;
        for (const auto& uv : buffers.texcoords) {
            if (!std::isfinite(static_cast<float>(uv[0])) || !std::isfinite(static_cast<float>(uv[1]))) {
                allFiniteUvs = false; break;
            }
        }
    }
    const auto validationMs = elapsed(validationStart);
    const auto tag = [&](const Key& key, uint32_t id) {
        if constexpr (ValidateUvs) {
            if (allFiniteUvs) { return key.uv >= 0 ? id : id | invalidUv; }
        }
        return tagId<true>(key,id,preciseUvs,nullptr);
    };
    auto* indices = geometry.indices.data();
    ModelLoadProgressCallback progress = [](const ModelLoadProgress&) {};
    size_t completed = 0;
    const auto resolveStart = Clock::now();
    for (size_t shapeIndex = 0; shapeIndex < result.shapes.size(); ++shapeIndex) {
        const auto& shape = result.shapes[shapeIndex];
        const auto offset = static_cast<uint32_t>(completed);
        bool hasTexcoords = true;
        uint32_t flags = 0;
        const auto* input = shape.mesh.indices.data();
        const auto count = shape.mesh.indices.size();
        for (size_t batch = 0; batch < count; batch += Batched ? 16384 : count) {
            const auto end = Batched ? std::min(count,batch+16384) : count;
            if constexpr (Batched) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,completed,corners); }
            for (size_t i = batch; i < end; ++i) {
                if constexpr (!Batched) {
                    if ((completed & 16383) == 0) { reportModelLoadProgress(progress,ModelLoadStage::buildingMesh,completed,corners); }
                }
                if constexpr (Ahead) {
                    if (i+Ahead < count) {
                        const auto future = static_cast<size_t>(input[i+Ahead].position_index);
                        if constexpr (PrefetchPrimary) { if (future < positions) { prefetch<Hint>(entries,future); } }
                        if constexpr (PrefetchUvs) {
                            const auto uv = input[i+Ahead].texcoord_index;
                            if (static_cast<size_t>(uv) < buffers.texcoords.size()) {
                                _mm_prefetch(reinterpret_cast<const char*>(preciseUvs+uv),Hint);
                            }
                        }
                    }
                }
                const auto& index = input[i];
                if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= positions) {
                    throw std::runtime_error("OBJ contains an invalid source position index.");
                }
                const Key key{index.position_index,index.normal_index,index.texcoord_index};
                const auto position = static_cast<size_t>(key.position);
                const auto existing = entryId(entries,position);
                uint32_t id;
                if constexpr (LazyKey) {
                    if (existing != vacant && matches(entries,position,key)) { id = existing; }
                    else {
                        if (existing == vacant) {
                            (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
                            id = tag(key,table.next);
                            insert(entries,position,key,id); ++table.next;
                        } else { id = seamId<true>(table,key,preciseUvs,nullptr); }
                    }
                } else if constexpr (MatchFirst) {
                    if (matches(entries,position,key) && existing != vacant) { id = existing; }
                    else if (existing == vacant) {
                        (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
                        id = tag(key,table.next);
                        insert(entries,position,key,id); ++table.next;
                    } else { id = seamId<true>(table,key,preciseUvs,nullptr); }
                } else {
                    if (existing == vacant) {
                        (void)sceneBufferBytes(size_t(table.next)+1,sizeof(Vertex));
                        id = tag(key,table.next);
                        insert(entries,position,key,id); ++table.next;
                    } else if (matches(entries,position,key)) { id = existing; }
                    else { id = seamId<true>(table,key,preciseUvs,nullptr); }
                }
                if constexpr (AccumulateFlags) { flags |= id; }
                else { hasTexcoords = hasTexcoords && !(id & invalidUv); }
                indices[completed++] = id & idMask;
            }
        }
        const auto written = static_cast<uint32_t>(completed)-offset;
        if (written) {
            MeshNode node; node.name = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
            node.indexOffset = offset; node.indexCount = written;
            node.hasTexcoords = AccumulateFlags ? !(flags & invalidUv) : hasTexcoords;
            geometry.nodes.push_back(std::move(node));
        }
    }
    const auto resolveMs = elapsed(resolveStart);
    const auto totalMs = elapsed(start);
    validate(entries,table,geometry,reference,positions);
    return {{"id_ms",resolveMs},{"allocate_ms",allocatedMs},{"uv_validation_ms",validationMs},{"resolve_ms",totalMs},{"exact",true},
        {"vertices",table.next},{"seams",table.seams.size()}};
}

Json dispatch(std::string_view name, const rapidobj::Result& result, SourceMeshData& source,
    const rapidobj::GeometryBuffers& buffers, size_t corners, const Reference& reference) {
    if (name.starts_with("parallel")) {
        Table table; MeshGeometry geometry; const auto start = Clock::now();
        ModelLoadProgressCallback progress = [](const ModelLoadProgress&) {};
        const auto workers = std::stoull(std::string(name.substr(8)));
        const auto dash = name.find('-');
        const auto discoverySize = dash == std::string_view::npos ? 1024 : std::stoull(std::string(name.substr(dash+1)));
        const auto times = resolveParallelFaces<true>(result,table,geometry,source,&buffers,progress,corners,true,workers,discoverySize);
        const auto ms = elapsed(start);
        validate(table.primary,table,geometry,reference,sourcePositions(source).size());
        return {{"resolve_ms",ms},{"exact",true},{"vertices",table.next},{"seams",table.seams.size()},
            {"allocate_ms",times.allocate},{"discover_ms",times.discover},{"count_ms",times.count},
            {"assign_ms",times.assign},{"output_ms",times.output},{"merge_ms",times.merge},
            {"bounds_ms",times.bounds},{"scanned_corners",times.scanned}};
    }
    if (name == "original" || name == "production") {
        Table table; MeshGeometry geometry; const auto start = Clock::now();
        ModelLoadProgressCallback progress = [](const ModelLoadProgress&) {};
        if (name == "original") { resolveOriginalFaces<true>(result,table,geometry,source,&buffers,progress,corners,true); }
        else { resolveFaces<true>(result,table,geometry,source,&buffers,progress,corners,true); }
        const auto ms = elapsed(start);
        validate(table.primary,table,geometry,reference,sourcePositions(source).size());
        return {{"resolve_ms",ms},{"exact",true},{"vertices",table.next},{"seams",table.seams.size()}};
    }
#define RUN(NAME, ...) if (name == NAME) { return resolve<__VA_ARGS__>(result,source,buffers,corners,reference); }
    RUN("aos12",std::vector<Primary>,0)
    RUN("aos16",std::vector<Wide>,0)
    RUN("packed16",std::vector<Packed>,0)
    RUN("reordered12",std::vector<Reordered>,0)
    RUN("split",Split,0)
    RUN("batch",std::vector<Primary>,0,3,true)
    RUN("match",std::vector<Primary>,0,3,false,true)
    RUN("p4",std::vector<Primary>,4)
    RUN("p8",std::vector<Primary>,8)
    RUN("p16",std::vector<Primary>,16)
    RUN("p32",std::vector<Primary>,32)
    RUN("p64",std::vector<Primary>,64)
    RUN("p128",std::vector<Primary>,128)
    RUN("p256",std::vector<Primary>,256)
    RUN("wide-p32",std::vector<Wide>,32)
    RUN("wide-p64",std::vector<Wide>,64)
    RUN("packed-p32",std::vector<Packed>,32)
    RUN("split-p32",Split,32)
    RUN("p32-t1",std::vector<Primary>,32,2)
    RUN("p32-nta",std::vector<Primary>,32,0)
    RUN("p32-batch",std::vector<Primary>,32,3,true)
    RUN("p32-match",std::vector<Primary>,32,3,false,true)
    RUN("compact",std::vector<Compact>,0)
    RUN("compact-p32",std::vector<Compact>,32)
    RUN("pair",std::vector<Pair>,0)
    RUN("flags",std::vector<Primary>,0,3,false,false,true)
    RUN("flags-batch",std::vector<Primary>,0,3,true,false,true)
    RUN("flags-match",std::vector<Primary>,0,3,false,true,true)
    RUN("flags-lazy",std::vector<Primary>,0,3,false,false,true,true)
    RUN("flags-p32",std::vector<Primary>,32,3,false,false,true)
    RUN("flags-p128",std::vector<Primary>,128,3,false,false,true)
    RUN("flags-pair",std::vector<Pair>,0,3,false,false,true)
    RUN("flags-compact",std::vector<Compact>,0,3,false,false,true)
    RUN("flags-compact-p32",std::vector<Compact>,32,3,false,false,true)
    RUN("uv-p32",std::vector<Primary>,32,3,false,false,false,false,true)
    RUN("uv-p8",std::vector<Primary>,8,3,false,false,false,false,true)
    RUN("uv-p16",std::vector<Primary>,16,3,false,false,false,false,true)
    RUN("uv-p64",std::vector<Primary>,64,3,false,false,false,false,true)
    RUN("uv-p128",std::vector<Primary>,128,3,false,false,false,false,true)
    RUN("uv-only8",std::vector<Primary>,8,3,false,false,false,false,true,false)
    RUN("uv-only16",std::vector<Primary>,16,3,false,false,false,false,true,false)
    RUN("uv-only32",std::vector<Primary>,32,3,false,false,false,false,true,false)
    RUN("uv-only64",std::vector<Primary>,64,3,false,false,false,false,true,false)
    RUN("uv-only128",std::vector<Primary>,128,3,false,false,false,false,true,false)
    RUN("uv-only256",std::vector<Primary>,256,3,false,false,false,false,true,false)
    RUN("uv-only32-t1",std::vector<Primary>,32,2,false,false,false,false,true,false)
    RUN("uv-only32-nta",std::vector<Primary>,32,0,false,false,false,false,true,false)
    RUN("uv-flags",std::vector<Primary>,32,3,false,false,true,false,true,false)
    RUN("uv-batch",std::vector<Primary>,32,3,true,false,true,false,true,false)
    RUN("uv-pair",std::vector<Pair>,32,3,true,false,true,false,true,false)
    RUN("uv-compact",std::vector<Compact>,32,3,true,false,true,false,true,false)
    RUN("finite",std::vector<Primary>,0,3,false,false,true,false,false,false,true)
    RUN("finite-batch",std::vector<Primary>,0,3,true,false,true,false,false,false,true)
    RUN("finite-p32",std::vector<Primary>,32,3,true,false,true,false,true,false,true)
    RUN("finite-compact",std::vector<Compact>,0,3,true,false,true,false,false,false,true)
#undef RUN
    throw std::invalid_argument("Unknown resolver variant: "+std::string(name));
}
} // namespace probe

int wmain(int argc, wchar_t** argv) {
    using namespace probe;
    try {
        if (argc < 3) { throw std::invalid_argument("Usage: woby_id_resolution MODEL OUTPUT [ROUNDS] [VARIANT ...]"); }
        const std::filesystem::path model(argv[1]);
        const std::filesystem::path destination(argv[2]);
        if (std::filesystem::exists(destination)) { throw std::invalid_argument("The measurement output already exists."); }
        const size_t rounds = argc > 3 ? std::stoull(argv[3]) : 1;
        if (!rounds || rounds > 100) { throw std::invalid_argument("Expected one to 100 measurement rounds."); }
        std::ofstream output(destination,std::ios::binary);
        if (!output) { throw std::runtime_error("Cannot open the measurement output."); }
        const auto emit = [&](Json record) { output << record.dump() << '\n'; output.flush(); std::cout << record.dump() << std::endl; };
        std::vector<Coordinate> positions,normals; std::vector<std::array<double,2>> texcoords;
        rapidobj::GeometryBuffers buffers{positions,texcoords,normals}; rapidobj::PrototypeOptions options; options.workers = 16;
        const auto start = Clock::now();
        auto parsed = rapidobj::ParseFilePrototype(model,buffers,options);
        if (parsed.polygons.error) { throw std::runtime_error(parsed.polygons.error.code.message()); }
        parsed.statements = {}; parsed.weights = {};
        if (!rapidobj::Triangulate(parsed.polygons,positions)) { throw std::runtime_error("Triangulation failed."); }
        SourceMeshData source; source.draftPositions = std::move(positions);
        size_t corners = 0; for (const auto& shape : parsed.polygons.shapes) { corners += shape.mesh.indices.size(); }
        emit({{"phase","parse"},{"ms",elapsed(start)},{"positions",sourcePositions(source).size()},
            {"normals",normals.size()},{"uvs",texcoords.size()},{"corners",corners},{"input_bytes",std::filesystem::file_size(model)}});
        Reference reference; const auto referenceStart = Clock::now();
        ModelLoadProgressCallback progress = [](const ModelLoadProgress&) {};
        resolveOriginalFaces<true>(parsed.polygons,reference.table,reference.geometry,source,&buffers,progress,corners,true);
        emit({{"phase","original-reference"},{"resolve_ms",elapsed(referenceStart)},
            {"vertices",reference.table.next},{"seams",reference.table.seams.size()}});
        std::vector<std::string> variants;
        for (int i = 4; i < argc; ++i) { variants.push_back(std::filesystem::path(argv[i]).string()); }
        if (variants.empty()) {
            variants = {"original","production","parallel1","parallel4","parallel8","aos12","aos16","packed16","reordered12","split","batch","match",
                "p4","p8","p16","p32","p64","p128","p256","uv-p16","uv-p32","uv-p64","uv-only32",
                "uv-batch","finite","finite-batch","finite-p32"};
            if (normals.empty()) { variants.insert(variants.end(),{"compact","uv-compact","finite-compact"}); }
        }
        for (size_t round = 0; round < rounds; ++round) {
            for (size_t i = 0; i < variants.size(); ++i) {
                const auto& name = variants[(i+round)%variants.size()];
                auto record = dispatch(name,parsed.polygons,source,buffers,corners,reference);
                record["variant"] = name; record["round"] = round+1; emit(std::move(record));
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << std::endl; return 1; }
}
