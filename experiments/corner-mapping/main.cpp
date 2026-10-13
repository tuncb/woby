#include "mapping.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include <bit>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace {
struct Fingerprint { uint64_t value = 14695981039346656037ull; };
template <typename T> void hash(Fingerprint& out, const T& value)
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    for (size_t i = 0; i < sizeof(T); ++i) { out.value = (out.value ^ bytes[i]) * 1099511628211ull; }
}
template <typename T> void hashValues(Fingerprint& out, const T& values)
{
    hash(out, values.size());
    for (const auto& value : values) { hash(out, value); }
}
void hashString(Fingerprint& out, const std::string& value)
{
    hash(out, value.size());
    for (const auto letter : value) { hash(out, letter); }
}
obj_profile::Json geometryInfo(const woby::Mesh& mesh)
{
    const auto& geometry = woby::meshGeometry(mesh);
    Fingerprint fingerprint, normals, source;
    hash(fingerprint, mesh.origin);
    if (geometry.sourcePositions) { hashValues(source, *geometry.sourcePositions); }
    hashValues(fingerprint, geometry.indices);
    hashValues(fingerprint, geometry.lineIndices);
    hashValues(fingerprint, geometry.pointIndices);
    hashValues(fingerprint, geometry.positionIndices);
    hash(fingerprint, geometry.positionIndexing);
    for (const auto& vertex : geometry.vertices) {
        hash(fingerprint, vertex.position); hash(fingerprint, vertex.texcoord); hash(normals, vertex.normal);
    }
    obj_profile::Json nodes = obj_profile::Json::array();
    size_t texturedGroups = 0;
    for (const auto& node : geometry.nodes) {
        hashString(fingerprint, node.name);
        hash(fingerprint, node.indexOffset); hash(fingerprint, node.indexCount);
        hash(fingerprint, node.lineIndexOffset); hash(fingerprint, node.lineIndexCount);
        hash(fingerprint, node.pointIndexOffset); hash(fingerprint, node.pointIndexCount);
        hash(fingerprint, node.hasTexcoords);
        if (node.hasTexcoords) { ++texturedGroups; }
        nodes.push_back({{"name", node.name}, {"triangle_offset", node.indexOffset}, {"triangle_indices", node.indexCount},
            {"line_offset", node.lineIndexOffset}, {"line_indices", node.lineIndexCount},
            {"point_offset", node.pointIndexOffset}, {"point_indices", node.pointIndexCount}, {"has_texcoords", node.hasTexcoords}});
    }
    if (mesh.sourceData) { hashValues(source, mesh.sourceData->indices); }
    obj_profile::Json output{{"vertices", geometry.vertices.size()}, {"triangles", geometry.indices.size() / 3},
        {"line_segments", geometry.lineIndices.size() / 2}, {"point_indices", geometry.pointIndices.size()},
        {"source_positions", geometry.sourcePositions ? geometry.sourcePositions->size() : 0},
        {"groups_with_texcoords", texturedGroups},
        {"geometry_fingerprint", fingerprint.value}, {"normal_fingerprint", normals.value}, {"source_fingerprint", source.value},
        {"position_indexing", geometry.positionIndexing == woby::PositionIndexing::identity ? "identity" : "mapped"},
        {"origin", mesh.origin}, {"bounds_min", mesh.bounds.min}, {"bounds_max", mesh.bounds.max},
        {"bounds_center", mesh.bounds.center}, {"bounds_radius", mesh.bounds.radius}, {"nodes", std::move(nodes)}};
    if (mesh.originalBounds) { output["original_bounds"] = *mesh.originalBounds; }
    return output;
}
obj_profile::Json memoryInfo(const woby::Mesh& mesh)
{
    const auto& geometry = woby::meshGeometry(mesh);
    return {{"vertex_size_bytes", geometry.vertices.size() * sizeof(woby::Vertex)},
        {"vertex_capacity_bytes", geometry.vertices.capacity() * sizeof(woby::Vertex)},
        {"source_position_size_bytes", geometry.sourcePositions ? geometry.sourcePositions->size() * sizeof(woby::Coordinate) : 0},
        {"source_position_capacity_bytes", geometry.sourcePositions ? geometry.sourcePositions->capacity() * sizeof(woby::Coordinate) : 0},
        {"precise_position_capacity_bytes", geometry.precisePositions.capacity() * sizeof(woby::Coordinate)},
        {"position_map_size_bytes", geometry.positionIndices.size() * sizeof(uint32_t)},
        {"position_map_capacity_bytes", geometry.positionIndices.capacity() * sizeof(uint32_t)},
        {"render_index_capacity_bytes", geometry.indices.capacity() * sizeof(uint32_t)},
        {"line_index_capacity_bytes", geometry.lineIndices.capacity() * sizeof(uint32_t)},
        {"point_index_capacity_bytes", geometry.pointIndices.capacity() * sizeof(uint32_t)},
        {"source_index_capacity_bytes", mesh.sourceData ? mesh.sourceData->indices.capacity() * sizeof(uint32_t) : 0}};
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc < 2 || argc > 6) {
        std::cerr << "Usage: corner_mapping MODEL [parserWorkers] [gatherWorkers] [deferred|eager] [cancel:PASS:COUNT]\n";
        return 2;
    }
    try {
        woby::ObjPrototypeOptions options;
        options.workers = argc > 2 ? std::stoull(argv[2]) : 16;
        corner_mapping::gatherWorkers = argc > 3 ? std::stoull(argv[3]) : 1;
        if (options.workers > 64 || corner_mapping::gatherWorkers < 1 || corner_mapping::gatherWorkers > 32) {
            throw std::invalid_argument("Worker count is outside the supported range.");
        }
        const std::wstring_view preparation = argc > 4 ? argv[4] : L"deferred";
        if (preparation != L"deferred" && preparation != L"eager") { throw std::invalid_argument("Expected deferred or eager."); }
        options.deferPreparation = preparation == L"deferred";
        char uvSetting[16]{};
        size_t uvSettingSize = 0;
        if (getenv_s(&uvSettingSize, uvSetting, sizeof(uvSetting), "WOBY_MAPPING_UNIQUE_UVS") != 0) {
            throw std::invalid_argument("Invalid unique UV setting.");
        }
        if (uvSettingSize) { corner_mapping::validateUniqueUvs = std::string_view(uvSetting) != "0"; }
        std::string cancelPass;
        size_t cancelAfter = 0, callbackCount = 0;
        if (argc > 5) {
            const auto specification = woby::pathToUtf8(std::filesystem::path(argv[5]));
            const auto split = specification.rfind(':');
            if (!specification.starts_with("cancel:") || split <= 7) { throw std::invalid_argument("Expected cancel:PASS:COUNT."); }
            cancelPass = specification.substr(7, split - 7);
            cancelAfter = std::stoull(specification.substr(split + 1));
        }
        woby::ModelLoadProgressCallback progress;
        if (!cancelPass.empty()) {
            progress = [&](const woby::ModelLoadProgress&) {
                if (corner_mapping::currentPass == cancelPass && ++callbackCount >= cancelAfter) {
                    throw std::runtime_error("Mapping cancellation test");
                }
            };
        }
        woby::ObjPrototypeMetrics metrics;
        const auto total = obj_profile::begin("cpu_import");
        auto mesh = woby::loadObjMeshPrototype(std::filesystem::path(argv[1]), progress, options, &metrics);
        obj_profile::end(total);
        auto output = obj_profile::result();
        output["variant"] = CORNER_MAPPING_VARIANT;
        output["parser_workers_requested"] = options.workers;
        output["gather_workers_requested"] = corner_mapping::gatherWorkers;
        output["deferred"] = options.deferPreparation;
        output["unique_uv_validation"] = corner_mapping::validateUniqueUvs;
        output["cpu_load_ms"] = output["events"].back()["wall_ms"];
        output["parser_ms"] = metrics.parseMs;
        output["parser_actual_workers"] = metrics.workers;
        output["memory"] = memoryInfo(mesh);
        const auto check = obj_profile::begin("geometry_validation");
        output["geometry"] = geometryInfo(mesh);
        obj_profile::end(check);
        output.update(obj_profile::result());
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
