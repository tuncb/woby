#include "obj_mesh.h"
#include "math_utils.h"
#include "scene_inspector_queries.h"
#include "scene_pick.h"
#include "surface_annotation.h"
#include "ui_operations.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace {
using namespace woby;
using Json = nlohmann::json;

void require(bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error(message); }
}

void setMode(UiState& state, std::string_view mode)
{
    // Probe input only; this program has no UI or persisted edits.
    for (auto& settings : state.files.front().groupSettings) {
        settings.showSolidMesh = mode == "solid" || mode == "combined";
        settings.showTriangles = mode == "edges" || mode == "combined";
        settings.showVertices = mode == "vertices" || mode == "combined";
    }
}

void selfTest()
{
    // This fixture is in memory and has no source filename or filesystem IO.
    UiState state;
    state.files.push_back(createUiFileState({}, loadObjMeshText(
        "v -.8 -.8 .5\nv .8 -.8 .5\nv 0 .8 .5\nf 1 2 3\n"), 0));
    appendDefaultSceneNodesForFiles(state, 0);
    assignSceneObjectIds(state);
    prepareAnnotationMeshCache(state.files.front().mesh);
    SceneQueryRuntime queries;
    selectSceneObject(state, state.files.front().objectId);
    updateSceneSelectionQueries(queries, state);
    require(queries.selection.dimensions.has_value(), "Selected geometry has no dimensions.");
    const auto builds = queries.selection.dimensionBuilds;
    updateSceneSelectionQueries(queries, state);
    require(queries.selection.dimensionBuilds == builds, "Unchanged selection rebuilt dimensions.");
    clearSceneSelection(state);
    updateSceneSelectionQueries(queries, state);
    require(!queries.selection.dimensions, "Cleared selection retained dimensions.");
    ScenePickView view;
    identityMatrix(view.view.data());
    identityMatrix(view.projection.data());
    view.width = view.height = 100;
    // OBJ preparation rebases positions around the model origin. Undo that
    // offset here to retain the fixture's specified clip-space coordinates.
    state.files.front().fileSettings.coordinateOffset = state.files.front().mesh.origin;
    for (const auto mode : {"solid", "edges", "vertices", "combined"}) {
        setMode(state, mode);
        const auto parts = scenePickParts(state);
        require(parts.size() == 1, "Expected one pickable part.");
        require(pickSceneObject(parts, view, {1, 1}) == invalidSceneObjectId, "Empty corner selected geometry.");
        const PickPoint hit = std::string_view(mode) == "vertices" ? PickPoint{50, 10}
            : std::string_view(mode) == "edges" ? PickPoint{50, 90} : PickPoint{50, 50};
        require(pickSceneObject(parts, view, hit) == state.files.front().groupSettings.front().objectId,
            "Expected primitive was not selected.");
    }
    require(removeFileFromState(state, 0) && state.files.empty(), "Model removal failed.");
}

template<typename Work>
void timed(Json& output, const std::string& name, Work work)
{
    std::cerr << name << '\n';
    const auto start = std::chrono::steady_clock::now();
    work();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    output["milliseconds"][name].push_back(ms);
    std::cerr << "  " << ms << " ms\n";
}
}

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") { selfTest(); return 0; }
        const bool verify = argc == 4 && std::string_view(argv[3]) == "--verify";
        const bool retire = argc == 4 && std::string_view(argv[3]) == "--retire";
        if ((argc != 3 && !verify && !retire) || !std::filesystem::path(argv[1]).is_absolute()) {
            throw std::runtime_error("usage: woby_post_load_interaction_probe ABSOLUTE_OBJ_PATH OUTPUT_JSON [--verify|--retire]");
        }
        using namespace woby;
        Json output{{"model", argv[1]}, {"viewport", {1280, 720}}, {"samples", 3}};
        Mesh mesh;
        timed(output, "load_and_prepare", [&] { mesh = loadObjMesh(argv[1]); });
        UiState state;
        timed(output, "create_ui_file", [&] {
            state.files.push_back(createUiFileState(argv[1], std::move(mesh), 0));
            synchronizeCoordinateFrames(state);
            appendDefaultSceneNodesForFiles(state, 0);
            assignSceneObjectIds(state);
            recalculateSceneBounds(state);
            frameCameraToScene(state);
        });
        output["vertices"] = meshGeometry(state.files.front().mesh).vertices.size();
        output["triangles"] = meshGeometry(state.files.front().mesh).indices.size() / 3;
        timed(output, "annotation_cache", [&] { prepareAnnotationMeshCache(state.files.front().mesh); });
        const auto& bounds = state.files.front().mesh.annotationCache->localBounds;
        output["extent_cache_bytes"] = bounds.capacity() * sizeof(bounds.front());
        SceneQueryRuntime queries;
        SceneInspectorRuntime inspector;
        inspector.sharedQueries = &queries;
        updateSceneInspector(inspector, state);
        const auto file = state.files.front().objectId;
        const auto group = state.files.front().groupSettings.front().objectId;
        for (const auto id : {file, group}) {
            const std::string kind = id == file ? "file" : "group";
            for (int run = 0; run < 3; ++run) {
                timed(output, "select_" + kind, [&] { selectSceneObject(state, id); });
                timed(output, "selected_queries_" + kind, [&] { updateSceneSelectionQueries(queries, state); });
                timed(output, "selected_inspector_" + kind, [&] { updateSceneInspector(inspector, state); });
                timed(output, "selected_queries_cached_" + kind, [&] { updateSceneSelectionQueries(queries, state); });
                timed(output, "clear_" + kind, [&] { clearSceneSelection(state); });
                timed(output, "cleared_queries_" + kind, [&] { updateSceneSelectionQueries(queries, state); });
                timed(output, "cleared_inspector_" + kind, [&] { updateSceneInspector(inspector, state); });
            }
        }
        const auto view = scenePickView(state.camera, state.upAxis, state.sceneBounds, 1280, 720, false, 1);
        for (const auto mode : {"solid", "edges", "vertices", "combined"}) {
            setMode(state, mode);
            const auto parts = scenePickParts(state);
            for (int run = 0; run < 3; ++run) {
                for (const auto point : {PickPoint{640, 360}, PickPoint{1, 1}}) {
                    const std::string name = std::string(mode) + (point[0] == 640 ? "_pick_center" : "_pick_corner");
                    SceneObjectId picked = 0;
                    ScenePickStatistics statistics;
                    timed(output, name, [&] { picked = pickSceneObject(parts, view, point, nullptr, &statistics); });
                    output["picked_ids"][name].push_back(picked);
                    output["visited_triangles"][name].push_back(statistics.triangles);
                    output["visited_spatial_nodes"][name].push_back(statistics.spatialNodes);
                }
            }
        }
        if (verify) {
            setMode(state, "combined");
            const auto parts = scenePickParts(state);
            Json samples = Json::array();
            for (int y = 0; y < 9; ++y) {
                for (int x = 0; x < 9; ++x) {
                    const PickPoint point{(static_cast<float>(x) + .5f) * 1280 / 9,
                        (static_cast<float>(y) + .5f) * 720 / 9};
                    SceneTriangleHit detail;
                    ScenePickStatistics statistics;
                    const auto start = std::chrono::steady_clock::now();
                    const auto picked = pickSceneObject(parts, view, point, &detail, &statistics);
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                    samples.push_back({{"point", point}, {"picked", picked}, {"triangle", detail.triangle},
                        {"barycentric", detail.barycentric}, {"milliseconds", ms}, {"triangles", statistics.triangles}});
                }
            }
            const auto hit = std::find_if(samples.begin(), samples.end(), [](const auto& sample) { return sample["picked"] != 0; });
            require(hit != samples.end(), "Grid contains no model hits.");
            const auto target = (*hit)["point"].get<PickPoint>();
            size_t empty = 0;
            double distance = std::numeric_limits<double>::infinity();
            for (size_t i = 0; i < samples.size(); ++i) {
                if (samples[i]["picked"] != 0) { continue; }
                const auto point = samples[i]["point"].get<PickPoint>();
                const double next = std::hypot(point[0] - target[0], point[1] - target[1]);
                if (next < distance) { distance = next; empty = i; }
            }
            const auto current = state.files.front().mesh.annotationCache;
            auto legacy = std::make_shared<MeshAnnotationCache>(*current);
            legacy->spatial.reset(); legacy->localBounds.clear();
            state.files.front().mesh.annotationCache = legacy;
            for (const auto& sample : {*hit, samples[empty]}) {
                SceneObjectId picked = 0;
                SceneTriangleHit detail;
                const auto point = sample["point"].get<PickPoint>();
                timed(output, "verify_exhaustive_pick", [&] { picked = pickSceneObject(parts, view, point, &detail); });
                require(picked == sample["picked"].get<SceneObjectId>()
                    && detail.triangle == sample["triangle"].get<size_t>()
                    && detail.barycentric == sample["barycentric"].get<std::array<double, 3>>(),
                    "Accelerated and exhaustive model picks differ.");
            }
            state.files.front().mesh.annotationCache = current;
            output["combined_grid"] = std::move(samples);
            output["exhaustive_grid_checks"] = 2;
        }
        if (retire) {
            Mesh removed;
            std::weak_ptr<const MeshGeometry> lifetime = state.files.front().mesh.storage;
            timed(output, "detach_geometry", [&] { removed = std::move(state.files.front().mesh); });
            timed(output, "remove_logical_model_retired", [&] { (void)removeFileFromState(state, 0); });
            require(state.files.empty() && !lifetime.expired(), "Retirement did not separate logical removal from release.");
            double releasedMs = 0;
            std::jthread worker;
            timed(output, "start_release_worker", [&] {
                worker = std::jthread([owned = std::move(removed), &releasedMs]() mutable {
                    const auto start = std::chrono::steady_clock::now();
                    owned = {};
                    releasedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                });
            });
            worker.join();
            require(lifetime.expired(), "Retirement retained mesh geometry after completion.");
            output["milliseconds"]["release_geometry_worker"].push_back(releasedMs);
        } else {
            timed(output, "remove_logical_model", [&] { (void)removeFileFromState(state, 0); });
        }
        output["remaining_files"] = state.files.size();
        std::ofstream stream(argv[2]);
        stream << output.dump(2) << '\n';
        if (!stream) { throw std::runtime_error("Cannot write probe output."); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
