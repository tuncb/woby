#include "model_mesh.h"
#include "comparison_scene.h"
#include "obj_mesh.h"
#include "scene_dimensions.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using namespace woby;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) { throw std::runtime_error(message); } }
size_t privateBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != 0, "Process memory query failed");
    return counters.PrivateUsage;
#else
    return 0;
#endif
}
uint64_t hashBytes(const void* pointer, size_t bytes, uint64_t hash = 14695981039346656037ull)
{
    const auto* data = static_cast<const unsigned char*>(pointer);
    while (bytes >= 8) {
        uint64_t word;
        std::memcpy(&word, data, 8);
        hash = (hash ^ word) * 1099511628211ull;
        data += 8; bytes -= 8;
    }
    while (bytes--) { hash = (hash ^ *data++) * 1099511628211ull; }
    return hash;
}
template<class T> uint64_t hashVector(const std::vector<T>& values)
{
    return hashBytes(values.data(), values.size() * sizeof(T));
}
Json meshHash(const Mesh& mesh)
{
    const auto& g = meshGeometry(mesh);
    Json nodes = Json::array();
    for (const auto& n : g.nodes) {
        nodes.push_back({n.name, n.indexOffset, n.indexCount, n.lineIndexOffset, n.lineIndexCount,
            n.pointIndexOffset, n.pointIndexCount, n.sourceObjectId, n.hasTexcoords});
    }
    uint64_t positions = 14695981039346656037ull;
    for (size_t i = 0; i < g.vertices.size(); ++i) {
        const auto point = meshPosition(mesh, i);
        positions = hashBytes(point.data(), sizeof(point), positions);
    }
    return {{"vertices", hashVector(g.vertices)}, {"positions", positions},
        {"indices", hashVector(g.indices)}, {"batches", hashVector(g.analysisVertexBatches)},
        {"nodes", nodes}, {"origin", mesh.origin}, {"bounds", {mesh.bounds.min, mesh.bounds.max}}};
}
Json groupsHash(const std::vector<UiGroupState>& groups)
{
    Json result = Json::array();
    for (const auto& g : groups) {
        result.push_back({g.center, g.localBounds.min, g.localBounds.max,
            g.originalBounds ? Json(*g.originalBounds) : Json()});
    }
    return result;
}

Json preparePositions(Mesh& mesh)
{
    require(mesh.sourceData != nullptr, "Missing source positions");
    const auto& geometry = meshGeometry(mesh);
    require(geometry.lineIndices.empty() && geometry.pointIndices.empty() && !mesh.freeform,
        "This benchmark requires a triangle-only OBJ");
    require(mesh.sourceData->indices.size() == geometry.indices.size(), "Source triangle order differs");
    std::vector<uint32_t> mapping(geometry.vertices.size(), UINT32_MAX);
    for (size_t i = 0; i < geometry.indices.size(); ++i) {
        const auto render = geometry.indices[i], source = mesh.sourceData->indices[i];
        auto& id = mapping.at(render);
        require(id == UINT32_MAX || id == source, "Conflicting source identities");
        id = source;
    }
    size_t nonidentity = 0, nearby = 0;
    for (size_t i = 0; i < mapping.size(); ++i) {
        const auto& source = woby::sourcePositions(*mesh.sourceData).at(mapping[i]);
        const auto actual = meshPosition(mesh, i);
        require((geometry.positionIndexing == PositionIndexing::identity ? i : geometry.positionIndices.at(i)) == mapping[i],
            "Stored source identity differs");
        require(std::memcmp(source.data(), actual.data(), sizeof(Coordinate)) == 0, "Position bits differ");
        nonidentity += mapping[i] != i;
        if (i && std::abs(int64_t(mapping[i]) - int64_t(mapping[i - 1])) <= 16) { ++nearby; }
    }
    Json result{{"render_vertices", geometry.vertices.size()}, {"source_points", woby::sourcePositions(*mesh.sourceData).size()},
        {"triangle_indices", geometry.indices.size()}, {"nodes", geometry.nodes.size()},
        {"line_indices", geometry.lineIndices.size()}, {"point_indices", geometry.pointIndices.size()},
        {"mapping_nonidentity_entries", nonidentity}, {"mapping_adjacent_source_delta_at_most_16", nearby},
        {"mapping_hash", hashVector(mapping)}, {"input", meshHash(mesh)},
        {"source_positions_hash", hashVector(woby::sourcePositions(*mesh.sourceData))},
        {"source_indices_hash", hashVector(mesh.sourceData->indices)}};

    return result;
}

template<class Work, class Digest>
void measure(Json& report, const char* name, int samples, Work work, Digest digest)
{
    std::vector<double> times;
    Json expected;
    size_t peakRetained = 0;
    for (int run = -1; run < samples; ++run) {
        std::cerr << name << " sample " << run << '\n';
        const auto start = Clock::now();
        const auto result = work();
        const auto end = Clock::now();
        peakRetained = std::max(peakRetained, privateBytes());
        // Full output fingerprints are outside the timed interval, for every run.
        const auto actual = digest(result);
        if (run == -1) { expected = actual; }
        else {
            require(actual == expected, "Repeated operation changed its output");
            times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        }
    }
    auto sorted = times;
    std::sort(sorted.begin(), sorted.end());
    report["operations"][name] = {{"samples_ms", times}, {"median_ms", sorted[sorted.size() / 2]},
        {"min_ms", sorted.front()}, {"max_ms", sorted.back()}, {"output", expected},
        {"max_retained_process_private_bytes", peakRetained}};
}

void selfTest()
{
    auto mesh = loadObjMeshText("v -0 0 0\nv 1 0 0\nv 0 1 0\nv 1 0 0\nv 8 8 8\nvt 0 0\nvt 1 0\nvt 0 1\nf 3/3 1/1 2/2\nf 3/1 4/2 1/3\n");
    std::vector<Coordinate> expected;
    for (size_t i = 0; i < meshGeometry(mesh).vertices.size(); ++i) { expected.push_back(meshPosition(mesh, i)); }
    preparePositions(mesh);
    auto copy = mesh;
    mesh = {};
    copy.sourceData.reset();
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto p = meshPosition(copy, i);
        require(std::memcmp(p.data(), expected[i].data(), sizeof(Coordinate)) == 0, "Snapshot lost precise coordinates");
    }
    bool rejected = false;
    try { (void)meshPosition(copy, expected.size()); } catch (const std::out_of_range&) { rejected = true; }
    require(rejected, "Out-of-range lookup accepted");
    auto direct = loadObjMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    require(meshPosition(direct, 1) == Coordinate{1, 0, 0}, "Direct geometry fallback changed");
}
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { selfTest(); return 0; }
        require(argc == 3, "usage: woby_position_VARIANT ABSOLUTE_OBJ_PATH SAMPLE_COUNT");
        const std::filesystem::path path(argv[1]);
        const int samples = std::stoi(argv[2]);
        require(path.is_absolute() && samples > 0 && samples <= 9, "Invalid path or sample count");
        Json report{{"variant", POSITION_VARIANT}, {"file", path.string()}, {"samples", samples}};
        std::cerr << "Loading " << path << '\n';
        const auto start = Clock::now();
        auto mesh = loadObjMesh(path);
        report["load_ms"] = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        std::cerr << "Preparing position relationship\n";
        report["model"] = preparePositions(mesh);
        report["position_storage_capacity_bytes"] = meshGeometry(mesh).precisePositions.capacity() * sizeof(Coordinate)
            + meshGeometry(mesh).positionIndices.capacity() * sizeof(uint32_t);
        report["render_vertex_capacity_bytes"] = meshGeometry(mesh).vertices.capacity() * sizeof(Vertex);
        report["steady_input_private_bytes"] = privateBytes();
        report["point_line_picking"] = "not applicable: triangle-only model; no synthetic primitives added";
        measure(report, "group_setup", samples, [&] { return createUiGroupStates(mesh, 0); }, groupsHash);
        ComparisonInputSnapshot snapshot;
        snapshot.origin = mesh.origin;
        snapshot.files.push_back(createUiFileState(path, std::move(mesh), 0));
        auto& file = snapshot.files[0];
        std::vector<ScenePickPart> parts;
        for (size_t i = 0; i < meshGeometry(file.mesh).nodes.size(); ++i) {
            const auto& node = meshGeometry(file.mesh).nodes[i];
            if (!node.indexCount) { continue; }
            file.groupSettings[i].objectId = i + 1;
            ComparisonInputPart source{0, i, {}};
            coordinateIdentity(source.parent.data());
            groupTransformMatrix(file.groupSettings[i], source.model.data());
            snapshot.parts[0].push_back(source);
            ScenePickPart part;
            part.mesh = &file.mesh; part.indexOffset = node.indexOffset; part.indexCount = node.indexCount;
            part.selected = true; part.model = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            parts.push_back(part);
        }
        measure(report, "group_bounds", samples, [&] {
            std::vector<std::array<Coordinate, 2>> result;
            for (const auto& node : meshGeometry(file.mesh).nodes) { result.push_back(originalMeshBounds(file.mesh, &node)); }
            return result;
        }, [](const auto& value) { return Json(value); });
        measure(report, "dimensions_uncached", samples, [&] { return sceneDimensions(parts); }, [](const auto& value) {
            require(value.has_value(), "Dimensions missing");
            return Json{{"corners", value->corners}, {"lengths", value->lengths}, {"object_axes", value->objectAxes}};
        });
        measure(report, "comparison_world", samples, [&] { return comparisonWorldMesh(snapshot, ComparisonSide::a); }, meshHash);
        std::cout << report.dump(2) << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
