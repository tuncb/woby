#include "../simplification-16/allocation_probe.h"
#include "scene_history.h"
#include "surface_annotation.h"
#include "ui_operations.h"
#include "math_utils.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <set>

namespace {
using namespace woby;
using Json = nlohmann::json;
void require(bool value) { if (!value) { throw std::runtime_error("History benchmark invariant failed."); } }
struct FixtureRoot {
    std::filesystem::path path = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-shared-annotation-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
    FixtureRoot() { require(std::filesystem::create_directory(path)); }
    ~FixtureRoot() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
// This accessor lets the same benchmark source build before and after sharing.
template<class Geometry> const AnnotationGeometry& value(const Geometry& geometry)
{
    if constexpr (requires { geometry.storage; }) { return *geometry.storage; }
    else { return geometry; }
}
UiState fixture(size_t segments, size_t groups, const std::filesystem::path& root)
{
    Mesh mesh;
    auto& data = editMeshGeometry(mesh);
    data.vertices = {{{0,0,0}},{{1,0,0}},{{0,1,0}}}; data.indices = {0,1,2};
    for (size_t i = 0; i < groups; ++i) { data.nodes.push_back({"part " + std::to_string(i), 0, 3}); }
    mesh.bounds = {{0,0,0},{1,1,0},{.5f,.5f,0},1}; publishMeshGeometry(mesh);
    UiState state;
    state.files.push_back(createUiFileState(root / "model.obj", std::move(mesh), 0));
    appendDefaultSceneNodesForFiles(state, 0); assignSceneObjectIds(state);
    if (segments) {
        AnnotationGeometry geometry;
        geometry.projector = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        geometry.fingerprint = annotationFingerprint(state.files[0].mesh, 0, 3);
        geometry.start = {0,0}; geometry.end = {1,0};
        geometry.segments.resize(segments);
        for (auto& segment : geometry.segments) { segment.a = {1,0,0}; segment.b = {0,1,0}; }
        (void)createAnnotation(state, state.files[0].groupSettings[0].objectId, std::move(geometry));
    }
    return state;
}
Json sample(size_t segments, size_t groups, bool drag, const std::filesystem::path& root)
{
    auto state = fixture(segments, groups, root);
    const auto clean = createSceneDocument(state);
    SceneHistory history;
    resetSceneHistory(history, state);
    const auto allocation = allocation_probe::start();
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 12; ++i) {
        setMasterVertexPointSize(state, 5.0f + static_cast<float>(i));
        recordSceneHistory(history, state, drag && i != 11 ? 91 : 0);
        updateSceneDirty(state, clean);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    const auto retained = allocation_probe::live.load() - allocation.liveBytes;
    const auto peak = allocation_probe::peak.load() - allocation.liveBytes;
    const auto traffic = allocation_probe::total.load() - allocation.totalBytes;
    const auto calls = allocation_probe::calls.load() - allocation.callCount;
    require(state.isDirty && history.snapshots.size() == (drag ? 2u : 13u));
    std::set<const AnnotationGeometry*> owners;
    size_t bytes = 0;
    const auto add = [&](const auto& annotations) {
        for (const auto& item : annotations) {
            const auto& geometry = value(item.geometry);
            if (owners.insert(&geometry).second) { bytes += geometry.segments.capacity() * sizeof(AnnotationSegment); }
        }
    };
    add(state.annotations); add(clean.annotations);
    for (const auto& snapshot : history.snapshots) { add(snapshot.content.annotations); add(snapshot.document.annotations); }
    const auto beforeUndo = createSceneDocument(state);
    auto prepared = prepareSceneHistoryStep(history, state, clean, false, {});
    require(prepared.has_value());
    commitSceneHistoryStep(history, state, std::move(*prepared), false);
    prepared = prepareSceneHistoryStep(history, state, clean, true, {});
    require(prepared.has_value());
    commitSceneHistoryStep(history, state, std::move(*prepared), true);
    require(sceneContentEqual(beforeUndo, createSceneDocument(state)));
    return {{"ms",ms},{"extra_retained_bytes",retained},{"extra_peak_bytes",peak},{"allocation_traffic_bytes",traffic},
        {"allocation_calls",calls},{"geometry_owners_live_clean_history",owners.size()},
        {"segment_capacity_bytes_live_clean_history",bytes},{"snapshots",history.snapshots.size()},{"verified",true}};
}
}
int main(int argc, char** argv)
{
    try {
        if (argc != 4) { throw std::runtime_error("Usage: woby_annotation_history_benchmark segments groups samples"); }
        const auto segments = std::stoull(argv[1]), groups = std::stoull(argv[2]);
        const int samples = std::stoi(argv[3]);
        require(segments <= 1000000 && groups > 0 && groups <= 16384 && samples > 0 && samples <= 9);
        const FixtureRoot root;
        (void)sample(10, 1, false, root.path);
        Json result{{"segments",segments},{"groups",groups},{"actions",12},{"discrete",Json::array()},{"drag",Json::array()}};
        for (int i = 0; i < samples; ++i) {
            result["discrete"].push_back(sample(segments, groups, false, root.path));
            result["drag"].push_back(sample(segments, groups, true, root.path));
        }
        std::cout << result.dump(2) << '\n';
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
