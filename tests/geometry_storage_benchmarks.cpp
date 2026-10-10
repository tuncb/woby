#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "scene_history.h"
#include "surface_annotation.h"
#include "ui_operations.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
// Keep this workload usable at the baseline commit without the storage API.
template<class T> const auto& readGeometry(const T& mesh)
{
    if constexpr (requires { meshGeometry(mesh); }) { return meshGeometry(mesh); }
    else { return mesh; }
}
template<class T> auto& writeGeometry(T& mesh)
{
    if constexpr (requires { editMeshGeometry(mesh); }) { return editMeshGeometry(mesh); }
    else { return mesh; }
}
template<class T> auto annotationCache(const T& mesh)
{
    if constexpr (requires { woby::buildAnnotationMeshCache(mesh); }) { return woby::buildAnnotationMeshCache(mesh); }
    else { return woby::buildAnnotationMeshCache(mesh.vertices, mesh.indices, mesh.nodes); }
}
size_t privateBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters))) {
        return counters.PrivateUsage;
    }
#endif
    return 0;
}
struct Directory {
    std::filesystem::path path = std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby-geometry-bench-" + woby::automationRandomHex(8));
    Directory() { std::filesystem::create_directory(path); }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
template<class Work> void measure(const char* name, size_t triangles, Work work)
{
    std::vector<double> times;
    size_t retained = 0, checksum = 0;
    for (size_t run = 0; run < 9; ++run) {
        const auto memory = privateBytes();
        const auto start = Clock::now();
        const auto result = work();
        const auto end = Clock::now();
        const auto used = privateBytes();
        retained = std::max(retained, used > memory ? used - memory : 0);
        checksum += result.second;
        if (run) { times.push_back(std::chrono::duration<double, std::milli>(end - start).count()); }
    }
    std::sort(times.begin(), times.end());
    std::cout << name << ',' << triangles << ',' << times.front() << ','
        << (times[3] + times[4]) / 2 << ',' << times.back() << ',' << retained << ',' << checksum << '\n';
}
}

TEST_CASE("geometry storage benchmark" * doctest::skip())
{
    const Directory directory;
    std::cout << "operation,triangles,min_ms,median_ms,max_ms,max_private_delta_bytes,checksum\n";
    for (const size_t triangles : {100000u, 1000000u}) {
        woby::Mesh mesh;
        writeGeometry(mesh).vertices.resize(triangles * 3);
        writeGeometry(mesh).precisePositions.resize(triangles * 3);
        writeGeometry(mesh).indices.resize(triangles * 3);
        for (size_t i = 0; i < triangles * 3; ++i) {
            const auto face = i / 3;
            const woby::Coordinate p{double(face % 1000) + (i % 3 == 1 ? .75 : 0),
                double(face / 1000) + (i % 3 == 2 ? .75 : 0), 0};
            writeGeometry(mesh).precisePositions[i] = p;
            writeGeometry(mesh).vertices[i].position = woby::renderPosition(p);
            writeGeometry(mesh).indices[i] = static_cast<uint32_t>(i);
        }
        writeGeometry(mesh).nodes = {{"selected", 0, 3}, {"rest", 3, static_cast<uint32_t>(readGeometry(mesh).indices.size() - 3)}};
        mesh.bounds = woby::calculateBounds(readGeometry(mesh).vertices);
        woby::UiState state;
        state.files.push_back(woby::createUiFileState(directory.path / "synthetic.obj", std::move(mesh), 0));
        woby::appendFolderTreeSceneNode(state, directory.path, 0, 1);
        const auto id = woby::createComparison(state);
        woby::setComparisonObjects(state, {state.files[0].groupSettings[0].objectId}, woby::ComparisonSide::a, true, id);
        woby::SceneHistory history;
        const auto clean = woby::createSceneDocument(state);
        woby::resetSceneHistory(history, state);
        state.files[0].groupSettings[0].translation[0] = 2;
        woby::markSceneDirty(state);
        REQUIRE(woby::recordSceneHistory(history, state));
        measure("snapshot_one_part", triangles, [&] {
            auto result = woby::snapshotComparisonInputs(state, id);
            const auto count = readGeometry(result.files[0].mesh).indices.size() + result.parts[0].size();
            return std::pair{std::move(result), count};
        });
        measure("undo_prepare", triangles, [&] {
            auto result = woby::prepareSceneHistoryStep(history, state, clean, false);
            const auto count = readGeometry(result->files[0].mesh).indices.size();
            return std::pair{std::move(result), count};
        });
        measure("comparison_source", triangles, [&] {
            auto result = woby::computeComparisonStages(state.files[0].mesh, {}, woby::comparisonSource);
            const auto count = readGeometry(result.original.source).indices.size();
            return std::pair{std::move(result), count};
        });
        measure("annotation_prepare", triangles, [&] {
            const auto& input = state.files[0].mesh;
            auto result = annotationCache(input);
            const auto count = result->spatial->triangles.size();
            return std::pair{std::move(result), count};
        });
    }
}
