#include "automation_registry.h"
#include "command_line.h"
#include "difference_mesh.h"
#include "difference_runtime.h"
#include "obj_mesh.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

namespace
{
using namespace woby;
Mesh surface(std::vector<Coordinate> points, std::vector<uint32_t> indices, Coordinate origin = {})
{
    Mesh mesh;
    mesh.origin = origin;
    auto &g = editMeshGeometry(mesh);
    for (const auto &p : points) {
        Vertex v;
        v.position = renderPosition(p);
        g.vertices.push_back(v);
        g.precisePositions.push_back(p);
    }
    g.indices = std::move(indices);
    g.nodes = {{"surface", 0, static_cast<uint32_t>(g.indices.size())}};
    mesh.bounds = calculateBounds(g.vertices);
    publishMeshGeometry(mesh);
    return mesh;
}
Mesh cube(double x = 0, double size = 1, Coordinate origin = {})
{
    return surface(
        {{x, 0, 0},
         {x + size, 0, 0},
         {x + size, size, 0},
         {x, size, 0},
         {x, 0, size},
         {x + size, 0, size},
         {x + size, size, size},
         {x, size, size}},
        {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7},
        origin);
}
Mesh sheet(double height, bool extraVertex = false)
{
    if (extraVertex) {
        return surface(
            {{0, 0, height}, {.5, 0, height}, {1, 0, height}, {1, 1, height}, {0, 1, height}, {.5, .5, height}},
            {0, 1, 5, 1, 2, 5, 2, 3, 5, 3, 4, 5, 4, 0, 5});
    }
    return surface({{0, 0, height}, {1, 0, height}, {1, 1, height}, {0, 1, height}}, {0, 1, 2, 0, 2, 3});
}
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path()) /
                                 ("woby-difference-" + automationRandomHex(8));
    UiState state;
    DifferenceRuntime runtime;
    SceneObjectId id = 0;
    Fixture()
    {
        std::filesystem::create_directory(root);
    }
    ~Fixture()
    {
        destroyDifferenceRuntime(runtime);
        std::error_code e;
        std::filesystem::remove_all(root, e);
    }
    void comparison(bool scenePlacement = true)
    {
        state.files.push_back(createUiFileState(root / "a.obj", cube(), 0));
        state.files.push_back(createUiFileState(root / "b.obj", cube(.5), 1));
        appendDefaultSceneNodesForFiles(state, 0);
        recalculateSceneBounds(state);
        id = createAnalysisFromObjects(state, AnalysisTask::surfaceComparison,
                                       {state.files[0].objectId, state.files[1].objectId});
        if (scenePlacement) {
            // These tests measure the intentional offset between the two cubes.
            auto settings = comparisonSettings(state, id);
            settings.lockPlacement = false;
            settings.alignOrigins = false;
            setComparisonSettings(state, settings, id);
        }
    }
    void wait()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            updateDifferenceRuntime(runtime, state, id, comparisonGeometrySignature(state, id));
            if (runtime.phase != "running" && runtime.phase != "queued") {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("Difference worker timed out");
    }
};
ControlOperation cli(std::vector<std::string> words)
{
    words.insert(words.begin(), {"woby", "ctl", "--instance", "test"});
    std::vector<char *> argv;
    for (auto &w : words) {
        argv.push_back(w.data());
    }
    return parseCommandLine(static_cast<int>(argv.size()), argv.data()).control.operation;
}
} // namespace
TEST_CASE("difference solids compute both directions and preserve small differences")
{
    SUBCASE("overlap")
    {
        auto r = buildDifferenceMesh(cube(), cube(.5));
        CHECK(r.volumes[0] == doctest::Approx(.5));
        CHECK(r.volumes[1] == doctest::Approx(.5));
        CHECK_FALSE(r.constructed);
    }
    SUBCASE("identical")
    {
        auto r = buildDifferenceMesh(cube(), cube());
        CHECK(r.volumes[0] == 0);
        CHECK(r.volumes[1] == 0);
        CHECK(meshGeometry(r.parts[0]).indices.empty());
    }
    SUBCASE("disjoint")
    {
        auto r = buildDifferenceMesh(cube(), cube(2));
        CHECK(r.volumes[0] == doctest::Approx(1));
        CHECK(r.volumes[1] == doctest::Approx(1));
    }
    SUBCASE("contained")
    {
        auto r = buildDifferenceMesh(cube(), cube(0, .5));
        CHECK(r.volumes[0] == doctest::Approx(.875));
        CHECK(r.volumes[1] == 0);
    }
    SUBCASE("thin displacement")
    {
        auto r = buildDifferenceMesh(cube(), cube(.000001));
        CHECK(r.volumes[0] == doctest::Approx(.000001).epsilon(.0001));
        CHECK(r.volumes[1] == doctest::Approx(.000001).epsilon(.0001));
    }
    SUBCASE("global winding reversal")
    {
        auto b = cube(.5);
        auto &g = editMeshGeometry(b);
        for (size_t i = 0; i < g.indices.size(); i += 3) {
            std::swap(g.indices[i + 1], g.indices[i + 2]);
        }
        publishMeshGeometry(b);
        auto r = buildDifferenceMesh(cube(), b);
        CHECK(r.volumes[0] == doctest::Approx(.5));
    }
    SUBCASE("different origins")
    {
        auto r = buildDifferenceMesh(cube(0, 1, {1e9, 2e9, 3e9}), cube(0, 1, {1e9 + .5, 2e9, 3e9}));
        CHECK(r.volumes[0] == doctest::Approx(.5));
        CHECK((r.parts[0].origin == Coordinate{1e9, 2e9, 3e9}));
        CHECK(originalMeshBounds(r.parts[0])[0][0] == 1e9);
        CHECK(r.parts[0].bounds.max[0] <= 1);
    }
}
TEST_CASE("difference closes open surfaces with unequal boundary vertex counts")
{
    const auto r = buildDifferenceMesh(sheet(0), sheet(.25, true));
    CHECK(r.constructed);
    CHECK(r.volumes[0] == doctest::Approx(.25));
    CHECK(r.volumes[1] == 0);
    CHECK(r.closureTriangles == 9);
    CHECK_FALSE(meshGeometry(r.parts[0]).indices.empty());
    CHECK(meshGeometry(r.parts[2]).indices.size() == 27);
    const auto reversed = buildDifferenceMesh(sheet(.25, true), sheet(0));
    CHECK(reversed.volumes[0] == doctest::Approx(.25));
}
TEST_CASE("difference reports invalid geometry and cannot silently close holes")
{
    SUBCASE("mixed topology")
    {
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), sheet(0)), doctest::Contains("one outer boundary"),
                             std::runtime_error);
    }
    SUBCASE("hole")
    {
        auto ring = surface({{0, 0, 0}, {3, 0, 0}, {3, 3, 0}, {0, 3, 0}, {1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {1, 2, 0}},
                            {0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7});
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(ring, sheet(1)), doctest::Contains("A: 2"), std::runtime_error);
    }
    SUBCASE("crossing surfaces")
    {
        auto tilted = surface({{0, 0, -1}, {1, 0, 1}, {1, 1, 1}, {0, 1, -1}}, {0, 1, 2, 0, 2, 3});
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(sheet(0), tilted), doctest::Contains("self-intersect"),
                             std::runtime_error);
    }
    SUBCASE("collapsed triangle")
    {
        auto bad = surface({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, {0, 1, 2});
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()), doctest::Contains("collapsed"), std::runtime_error);
    }
    SUBCASE("winding")
    {
        auto bad = cube();
        auto &g = editMeshGeometry(bad);
        std::swap(g.indices[0], g.indices[1]);
        publishMeshGeometry(bad);
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()), doctest::Contains("winding"), std::runtime_error);
    }
    SUBCASE("cancel")
    {
        std::stop_source stop;
        stop.request_stop();
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), cube(.5), stop.get_token()), doctest::Contains("canceled"),
                             std::runtime_error);
    }
}
TEST_CASE("difference export preserves world coordinates and refuses overwrite")
{
    Fixture f;
    auto r = buildDifferenceMesh(cube(0, 1, {1e9, 2e9, 3e9}), cube(.5, 1, {1e9, 2e9, 3e9}));
    const auto path = f.root / "result.obj";
    exportDifferenceMesh(r, path);
    const auto loaded = loadObjMesh(path);
    CHECK(meshGeometry(loaded).indices.size() ==
          meshGeometry(r.parts[0]).indices.size() + meshGeometry(r.parts[1]).indices.size());
    CHECK(originalMeshBounds(loaded)[0][0] == 1e9);
    CHECK(originalMeshBounds(loaded)[1][0] == 1e9 + 1.5);
    const auto size = std::filesystem::file_size(path);
    CHECK_THROWS(exportDifferenceMesh(r, path));
    CHECK(std::filesystem::file_size(path) == size);
    CHECK_THROWS(exportDifferenceMesh(r, "relative.obj"));
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS(exportDifferenceMesh(r, f.root / "cancel.obj", stop.get_token()));
    CHECK_FALSE(std::filesystem::exists(f.root / "cancel.obj"));
    CHECK(std::distance(std::filesystem::directory_iterator(f.root), std::filesystem::directory_iterator{}) == 1);
}
TEST_CASE("difference runtime rejects stale results and rebuilds persisted settings")
{
    Fixture f;
    f.comparison();
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    INFO(f.runtime.error);
    REQUIRE(f.runtime.phase == "ready");
    CHECK(differenceStatus(f.runtime, f.state, f.id)["totalVolume"] == 1);
    auto settings = comparisonSettings(f.state, f.id);
    settings.tolerance = .123f;
    settings.difference.opacity = .4f;
    settings.difference.showA = false;
    setComparisonSettings(f.state, settings, f.id);
    f.wait();
    CHECK(f.runtime.phase == "ready");
    const auto path = f.root / "test.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    const auto restored = prepareSceneReplacement(f.state, f.state.files, readSceneDocument(path));
    REQUIRE(restored.comparisons.size() == 1);
    const auto loadedId = restored.comparisons[0].objectId;
    CHECK(comparisonSettings(restored, loadedId).difference == settings.difference);
    DifferenceRuntime loaded;
    updateDifferenceRuntime(loaded, restored, loadedId, comparisonGeometrySignature(restored, loadedId));
    CHECK(loaded.phase == "running");
    destroyDifferenceRuntime(loaded);
    selectSceneObject(f.state, f.state.files[1].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 2);
    CHECK(differenceStatus(f.runtime, f.state, f.id)["state"] == "outdated");
    CHECK(differenceStatus(f.runtime, f.state, f.id)["totalVolume"].is_null());
    f.wait();
    CHECK(f.runtime.phase == "outdated");
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    INFO(f.runtime.error);
    CHECK(f.runtime.phase == "ready");
    CHECK(differenceStatus(f.runtime, f.state, f.id)["totalVolume"] == 2);
    requestDifferenceMesh(f.state, f.id);
    updateDifferenceRuntime(f.runtime, f.state, f.id, comparisonGeometrySignature(f.state, f.id));
    requestDifferenceMesh(f.state, f.id, true);
    f.wait();
    CHECK(f.runtime.phase == "canceled");
}
TEST_CASE("difference CLI supports creation display export cancellation and deletion")
{
    for (const auto action :
         {"difference-create", "difference-get", "difference-rebuild", "difference-cancel", "difference-delete"}) {
        const auto c = cli({"analysis", action, "object"});
        const auto p = controlOperationParams(c);
        CHECK(controlOperationParams(parseControlOperation(controlMethod(c.action), p)) == p);
    }
    const auto c = cli({"analysis", "difference-set", "object", "--visible", "false", "--show-a-only", "false",
                        "--show-b-only", "true", "--show-walls", "false", "--opacity", "0.6"});
    CHECK(c.visible == false);
    CHECK(c.showAOnly == false);
    CHECK(c.showBOnly == true);
    CHECK(c.showWalls == false);
    REQUIRE(c.opacity.has_value());
    CHECK(*c.opacity == doctest::Approx(.6f));
    CHECK(controlOperationParams(parseControlOperation(controlMethod(c.action), controlOperationParams(c))) ==
          controlOperationParams(c));
    CHECK_THROWS(cli({"analysis", "difference-export", "object"}));
    Fixture f;
    f.comparison();
    auto command = cli({"analysis", "difference-create", "object"});
    command.objectId = f.id;
    (void)controlDifference(f.state, f.runtime, command);
    f.wait();
    REQUIRE(f.runtime.phase == "ready");
    command = c;
    command.objectId = f.id;
    (void)controlDifference(f.state, f.runtime, command);
    CHECK_FALSE(comparisonSettings(f.state, f.id).difference.showA);
    command = cli({"analysis", "difference-delete", "object"});
    command.objectId = f.id;
    CHECK(controlDifference(f.state, f.runtime, command)["difference"]["state"] == "not_created");
    CHECK_FALSE(comparisonSettings(f.state, f.id).difference.created);
    CHECK(f.runtime.phase == "not_created");
}
TEST_CASE("difference screenshot readiness rejects source changes before the runtime update")
{
    Fixture f;
    f.comparison();
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    REQUIRE(f.runtime.phase == "ready");
    ComparisonRuntimes runtimes;
    runtimes.objects[f.id].difference = std::move(f.runtime);
    runtimes.objects[f.id].difference.uploaded = true;
    CHECK(comparisonsReadyForScreenshot(f.state, runtimes));
    selectSceneObject(f.state, f.state.files[1].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 2);
    CHECK_FALSE(comparisonsReadyForScreenshot(f.state, runtimes));
    destroyComparisonRuntimes(runtimes);
}

TEST_CASE("locked difference results remain current when source models move")
{
    Fixture f;
    f.comparison(false);
    REQUIRE(comparisonSettings(f.state, f.id).alignOrigins);
    REQUIRE(comparisonSettings(f.state, f.id).lockPlacement);
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    REQUIRE(f.runtime.phase == "ready");
    CHECK(differenceStatus(f.runtime, f.state, f.id)["totalVolume"] == 0);
    const auto signature = comparisonGeometrySignature(f.state, f.id);
    selectSceneObject(f.state, f.state.files[1].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 20);
    setSelectedObjectProperty(f.state, UiObjectProperty::rotationZ, 35);
    CHECK(comparisonGeometrySignature(f.state, f.id) == signature);
    CHECK(differenceStatus(f.runtime, f.state, f.id)["state"] == "ready");
    ComparisonRuntimes runtimes;
    runtimes.objects[f.id].difference = std::move(f.runtime);
    runtimes.objects[f.id].difference.uploaded = true;
    CHECK(comparisonsReadyForScreenshot(f.state, runtimes));
    auto settings = comparisonSettings(f.state, f.id);
    settings.lockPlacement = false;
    settings.alignOrigins = false;
    setComparisonSettings(f.state, settings, f.id);
    CHECK_FALSE(comparisonsReadyForScreenshot(f.state, runtimes));
    destroyComparisonRuntimes(runtimes);
}
