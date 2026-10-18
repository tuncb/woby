#include "automation_registry.h"
#include "command_line.h"
#include "difference_mesh.h"
#include "difference_runtime.h"
#include "obj_mesh.h"
#include "ui_operations.h"
#include "analysis_presentation.h"
#include "scene_history.h"
#include <doctest/doctest.h>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <manifold/manifold.h>
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
Mesh withCollapsedTriangles(Mesh mesh)
{
    auto &g = editMeshGeometry(mesh);
    const auto first = static_cast<uint32_t>(g.vertices.size());
    // Collinear points, coincident positions with distinct indices, and a repeated index.
    // Remote discarded points must not affect the working mesh or result bounds.
    for (const Coordinate p : {Coordinate{1000, 1000, 1000}, Coordinate{1001, 1000, 1000},
                               Coordinate{1002, 1000, 1000}, Coordinate{1000, 1000, 1000}}) {
        Vertex v;
        v.position = renderPosition(p);
        g.vertices.push_back(v);
        g.precisePositions.push_back(p);
    }
    g.indices.insert(g.indices.end(), {first, first + 1, first + 2, first, first + 1, first + 3, 0, 0, 1});
    g.nodes[0].indexCount = static_cast<uint32_t>(g.indices.size());
    publishMeshGeometry(mesh);
    return mesh;
}
Mesh manifoldMesh(const manifold::Manifold &solid, Coordinate origin = {})
{
    const auto mesh = solid.GetMeshGL64();
    std::vector<Coordinate> points;
    std::vector<uint32_t> indices;
    for (size_t i = 0; i < mesh.vertProperties.size(); i += mesh.numProp) {
        points.push_back({mesh.vertProperties[i], mesh.vertProperties[i + 1], mesh.vertProperties[i + 2]});
    }
    for (const auto i : mesh.triVerts) { indices.push_back(static_cast<uint32_t>(i)); }
    return surface(std::move(points), std::move(indices), origin);
}
void checkClosedEdges(const Mesh &mesh)
{
    std::map<std::pair<uint32_t, uint32_t>, std::pair<int, int>> edges;
    const auto &indices = meshGeometry(mesh).indices;
    for (size_t i = 0; i < indices.size(); i += 3) {
        for (size_t k = 0; k < 3; ++k) {
            const auto a = indices[i + k], b = indices[i + (k + 1) % 3];
            auto &edge = edges[std::minmax(a, b)];
            ++edge.first;
            edge.second += a < b ? 1 : -1;
        }
    }
    for (const auto &[key, counts] : edges) {
        (void)key;
        REQUIRE(counts.first == 2);
        REQUIRE(counts.second == 0);
    }
}
Mesh subdivide(const Mesh &mesh)
{
    std::vector<Coordinate> points;
    std::vector<uint32_t> indices;
    const auto &g = meshGeometry(mesh);
    for (size_t i = 0; i < g.indices.size(); i += 3) {
        const auto a = meshPosition(mesh, g.indices[i]), b = meshPosition(mesh, g.indices[i + 1]),
                   c = meshPosition(mesh, g.indices[i + 2]);
        const auto midpoint = [](const Coordinate &p, const Coordinate &q) {
            return Coordinate{(p[0] + q[0]) / 2, (p[1] + q[1]) / 2, (p[2] + q[2]) / 2};
        };
        const auto ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
        for (const auto p : {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca}) {
            indices.push_back(static_cast<uint32_t>(points.size()));
            points.push_back(p);
        }
    }
    return surface(std::move(points), std::move(indices), mesh.origin);
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
    void comparison(Mesh a = cube(), Mesh b = cube(.5), bool scenePlacement = true)
    {
        state.files.push_back(createUiFileState(root / "a.obj", std::move(a), 0));
        state.files.push_back(createUiFileState(root / "b.obj", std::move(b), 1));
        appendDefaultSceneNodesForFiles(state, 0);
        recalculateSceneBounds(state);
        id = createAnalysisFromObjects(state, AnalysisTask::differenceMesh,
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
            const auto &phase = runtime.phase;
            if (phase != "running" && phase != "queued") {
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

TEST_CASE("approximate mesh retains thin differences between rotated solids without a grid")
{
    for (const double shift : {.1, 1e-6, 1e-9}) {
        CAPTURE(shift);
        const auto a = manifoldMesh(manifold::Manifold::Cube({3, 4, 5}).Rotate(12.3, 6.15, 4.1), {1e9, 2e9, 3e9});
        const auto b = manifoldMesh(manifold::Manifold::Cube({3, 4, 5}).Translate({shift, 0, 0}).Rotate(12.3, 6.15, 4.1), a.origin);
        const auto before = meshGeometry(a).precisePositions;
        const auto indices = meshGeometry(a).indices;
        const auto result = buildDifferenceMesh(a, b);
        const auto reverse = buildDifferenceMesh(b, a);
        CHECK(result.message.find("estimates") != std::string::npos);
        for (size_t i = 0; i < 2; ++i) {
            CHECK(std::abs(result.volumes[i] - 20 * shift) < 20 * shift * 1e-4 + 1e-12);
            CHECK(result.volumes[i] == doctest::Approx(reverse.volumes[1 - i]).epsilon(1e-6));
            checkClosedEdges(result.parts[i]);
            CHECK(result.parts[i].origin == a.origin);
            CHECK(originalMeshBounds(result.parts[i])[0][0] > a.origin[0] - 2);
        }
        CHECK(meshGeometry(a).precisePositions == before);
        CHECK(meshGeometry(a).indices == indices);
    }
    CHECK(buildDifferenceMesh(cube(), subdivide(cube())).volumes == std::array<double, 2>{0, 0});
    const auto disjoint = buildDifferenceMesh(cube(), cube(2));
    CHECK(disjoint.volumes[0] == doctest::Approx(1));
    CHECK(disjoint.volumes[1] == doctest::Approx(1));
}

TEST_CASE("approximate mesh detects an internal unit boundary change and preserves cavities")
{
    const auto box = [](double height, double z = 0) {
        return manifoldMesh(manifold::Manifold::Cube({1, 1, height}).Translate({0, 0, z}));
    };
    const auto lower = buildDifferenceMesh(box(.5), box(.6));
    const auto upper = buildDifferenceMesh(box(.5, .5), box(.4, .6));
    CHECK(lower.volumes[0] == doctest::Approx(0));
    CHECK(lower.volumes[1] == doctest::Approx(.1));
    CHECK(upper.volumes[0] == doctest::Approx(.1));
    CHECK(upper.volumes[1] == doctest::Approx(0));
    CHECK(buildDifferenceMesh(cube(), cube()).volumes == std::array<double, 2>{0, 0});
    const auto cavity = manifold::Manifold::Cube({1, 1, 1}) -
                        manifold::Manifold::Cube({.5, .5, .5}).Translate({.25, .25, .25});
    const auto result = buildDifferenceMesh(manifoldMesh(cavity), cube());
    CHECK(result.volumes[0] == doctest::Approx(0));
    CHECK(result.volumes[1] == doctest::Approx(.125));
    checkClosedEdges(result.parts[1]);
}

TEST_CASE("approximate mesh rejects open topology invalid coordinates and canceled work")
{
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(sheet(0), cube()),
                         doctest::Contains("Input A:"), std::runtime_error);
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), sheet(0)),
                         doctest::Contains("Input B:"), std::runtime_error);
    auto bad = cube();
    auto &g = editMeshGeometry(bad);
    g.precisePositions[0][0] = std::numeric_limits<double>::infinity();
    publishMeshGeometry(bad);
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()),
                         doctest::Contains("non-finite"), std::runtime_error);
    bad = cube();
    auto &winding = editMeshGeometry(bad);
    std::swap(winding.indices[0], winding.indices[1]);
    publishMeshGeometry(bad);
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()),
                         doctest::Contains("closed solids"), std::runtime_error);
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), cube(.5), stop.get_token()),
                         doctest::Contains("canceled"), std::runtime_error);
}

TEST_CASE("difference exposes one algorithm and retains display settings")
{
    Fixture f;
    f.comparison();
    auto command = cli({"analysis", "difference-create", "object"});
    command.objectId = f.id;
    (void)controlDifference(f.state, f.runtime, command);
    f.wait();
    INFO(f.runtime.error);
    REQUIRE(f.runtime.phase == "ready");
    const auto status = differenceStatus(f.runtime, f.state, f.id);
    CHECK(status["kind"] == "approximate_mesh");
    CHECK(status["triangles"].size() == 2);
    for (const auto key : {"method", "cellSize", "showWalls", "closureTriangles", "resolutionCheck"}) {
        CHECK_FALSE(status.contains(key));
        CHECK_FALSE(status["settings"].contains(key));
    }
    auto settings = comparisonSettings(f.state, f.id);
    settings.difference.showB = false;
    settings.difference.opacity = .4f;
    setComparisonSettings(f.state, settings, f.id);
    CHECK(differenceResultCurrent(f.runtime, f.state, f.id));
    const auto path = f.root / "difference.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    const auto restored = prepareSceneReplacement(f.state, f.state.files, readSceneDocument(path));
    REQUIRE(restored.comparisons.size() == 1);
    CHECK(restored.comparisons[0].settings.difference == settings.difference);
    for (const auto action : {"difference-create", "difference-rebuild", "difference-set"}) {
        CHECK_THROWS(cli({"analysis", action, "object", "--method", "approximate_mesh"}));
        CHECK_THROWS(cli({"analysis", action, "object", "--cell-size", "1"}));
    }
    CHECK_THROWS(cli({"analysis", "difference-set", "object", "--show-walls", "false"}));
    CHECK_THROWS(cli({"analysis", "difference-check-resolution", "object"}));
    CHECK_THROWS(cli({"analysis", "difference-cancel-check", "object"}));
}

TEST_CASE("difference workers discard results after cancellation source edits or replacement")
{
    Fixture f;
    f.comparison();
    requestDifferenceMesh(f.state, f.id);
    // Supply an owned pending result to exercise publication without relying on timing.
    std::promise<std::shared_ptr<const DifferenceMesh>> pending;
    auto &r = f.runtime;
    r.initialized = true;
    r.phase = "running";
    r.consumedRequest = r.workerRequest = findComparison(f.state, f.id)->differenceRequest.revision;
    r.workerSignature = comparisonGeometrySignature(f.state, f.id);
    r.worker = pending.get_future();
    bool replaced = false;
    SUBCASE("cancel") { requestDifferenceMesh(f.state, f.id, true); }
    SUBCASE("source edit") {
        selectSceneObject(f.state, f.state.files[1].objectId);
        setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 2);
    }
    SUBCASE("new request") { requestDifferenceMesh(f.state, f.id); replaced = true; }
    pending.set_value(std::make_shared<const DifferenceMesh>(buildDifferenceMesh(cube(), cube(.5))));
    updateDifferenceRuntime(r, f.state, f.id, comparisonGeometrySignature(f.state, f.id));
    CHECK_FALSE(r.result);
    if (replaced) {
        CHECK(r.phase == "running");
        f.wait();
        REQUIRE(r.phase == "ready");
        CHECK(differenceResultCurrent(r, f.state, f.id));
    } else {
        CHECK((r.phase == "canceled" || r.phase == "outdated"));
        CHECK_FALSE(differenceResultCurrent(r, f.state, f.id));
    }
}
} // namespace
TEST_CASE("difference solids compute both directions and preserve small differences")
{
    SUBCASE("overlap")
    {
        auto r = buildDifferenceMesh(cube(), cube(.5));
        CHECK(r.volumes[0] == doctest::Approx(.5));
        CHECK(r.volumes[1] == doctest::Approx(.5));
        CHECK(r.removedCollapsedTriangles == std::array<size_t, 2>{0, 0});
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
    SUBCASE("small nonzero-area input triangles")
    {
        const auto r = buildDifferenceMesh(cube(0, 1e-6), cube(2e-6, 1e-6));
        CHECK(r.removedCollapsedTriangles == std::array<size_t, 2>{0, 0});
        CHECK(r.volumes[0] > 0);
        CHECK(r.volumes[1] > 0);
        CHECK(r.volumes[0] == doctest::Approx(1e-18).epsilon(1e-6));
        CHECK(r.volumes[1] == doctest::Approx(1e-18).epsilon(1e-6));
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
TEST_CASE("difference preserves separate solids that meet at an edge")
{
    // A notched prism minus its lower half leaves two triangular prisms.
    // They share a geometric edge, but each has its own closed topology.
    const auto a = surface({{0, 0, 0}, {2, 0, 0}, {2, 2, 0}, {1, 1, 0}, {0, 2, 0},
                            {0, 0, 1}, {2, 0, 1}, {2, 2, 1}, {1, 1, 1}, {0, 2, 1}},
                           {0, 3, 1, 1, 3, 2, 0, 4, 3, 5, 6, 8, 6, 7, 8, 5, 8, 9,
                            0, 1, 6, 0, 6, 5, 1, 2, 7, 1, 7, 6, 2, 3, 8, 2, 8, 7,
                            3, 4, 9, 3, 9, 8, 4, 0, 5, 4, 5, 9});
    auto b = cube();
    auto &g = editMeshGeometry(b);
    for (size_t i = 0; i < g.vertices.size(); ++i) {
        g.precisePositions[i][0] *= 2;
        g.vertices[i].position = renderPosition(g.precisePositions[i]);
    }
    publishMeshGeometry(b);
    for (const bool reverse : {false, true}) {
        CAPTURE(reverse);
        const auto result = buildDifferenceMesh(reverse ? b : a, reverse ? a : b);
        const size_t part = reverse ? 1 : 0;
        CHECK(result.volumes[part] == doctest::Approx(1));
        CHECK(result.volumes[1 - part] == 0);
        checkClosedEdges(result.parts[part]);
        std::map<Coordinate, size_t> positions;
        for (const auto &p : meshGeometry(result.parts[part]).precisePositions) { ++positions[p]; }
        CHECK(positions.at({1, 1, 0}) == 2);
        CHECK(positions.at({1, 1, 1}) == 2);
    }
}

TEST_CASE("difference removes exact collapsed faces from both solid inputs without changing the sources")
{
    const auto a = withCollapsedTriangles(cube());
    const auto b = withCollapsedTriangles(withCollapsedTriangles(cube(.5)));
    const auto aIndices = meshGeometry(a).indices, bIndices = meshGeometry(b).indices;
    const auto aPositions = meshGeometry(a).precisePositions, bPositions = meshGeometry(b).precisePositions;
    const auto r = buildDifferenceMesh(a, b);
    CHECK(r.removedCollapsedTriangles == std::array<size_t, 2>{3, 6});
    CHECK(r.volumes[0] == doctest::Approx(.5));
    CHECK(r.volumes[1] == doctest::Approx(.5));
    CHECK(r.message.find("A: 3, B: 6") != std::string::npos);
    CHECK(meshGeometry(a).indices == aIndices);
    CHECK(meshGeometry(b).indices == bIndices);
    CHECK(meshGeometry(a).precisePositions == aPositions);
    CHECK(meshGeometry(b).precisePositions == bPositions);
}
TEST_CASE("difference reports invalid geometry and cannot silently close holes")
{
    SUBCASE("all triangles collapsed in A or B")
    {
        const auto bad = surface({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}, {0, 1, 2, 0, 0, 1});
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()),
                             "Input A: No triangles remain after removing 2 exact zero-area triangles.", std::runtime_error);
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), bad),
                             "Input B: No triangles remain after removing 2 exact zero-area triangles.", std::runtime_error);
    }
    SUBCASE("non-finite coordinates cannot be discarded as collapsed")
    {
        auto bad = surface({{0, 0, 0}}, {0, 0, 0});
        editMeshGeometry(bad).precisePositions[0][0] = std::numeric_limits<double>::infinity();
        publishMeshGeometry(bad);
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()), doctest::Contains("non-finite"), std::runtime_error);
    }
    SUBCASE("winding")
    {
        auto bad = withCollapsedTriangles(cube());
        auto &g = editMeshGeometry(bad);
        std::swap(g.indices[0], g.indices[1]);
        publishMeshGeometry(bad);
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(bad, cube()), doctest::Contains("closed solids"), std::runtime_error);
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
                        "--show-b-only", "true", "--opacity", "0.6"});
    CHECK(c.visible == false);
    CHECK(c.showAOnly == false);
    CHECK(c.showBOnly == true);
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
    f.comparison(cube(), cube(.5), false);
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

TEST_CASE("duplicated comparisons retain difference settings without copying requests")
{
    Fixture f;
    f.comparison();
    auto settings = comparisonSettings(f.state, f.id);
    settings.difference.opacity = .4f;
    setComparisonSettings(f.state, settings, f.id);
    requestDifferenceMesh(f.state, f.id);
    const auto duplicate = duplicateComparison(f.state, f.id);
    const auto* copy = findComparison(f.state, duplicate);
    REQUIRE(copy);
    CHECK(copy->settings.difference == comparisonSettings(f.state, f.id).difference);
    CHECK(copy->differenceRequest.revision == 0);
    CHECK(findComparison(f.state, f.id)->differenceRequest.revision > 0);
}

TEST_CASE("difference mesh is a separate analysis with its own input membership")
{
    Fixture f;
    f.comparison();
    const auto settings = comparisonSettings(f.state, f.id);
    CHECK(settings.type == AnalysisType::differenceMesh);
    CHECK(comparisonTask(f.state, f.id) == AnalysisTask::differenceMesh);
    CHECK(std::string(analysisTypeKey(settings.type)) == "difference_mesh");
    CHECK(settings.difference.created);
    CHECK(requestedComparisonStages(settings, true, true) == 0);
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, f.id) == 1);
    CHECK(comparisonPartCount(f.state, ComparisonSide::b, f.id) == 1);
    const auto other = createAnalysisFromObjects(f.state, AnalysisTask::surfaceComparison,
        {f.state.files[0].objectId, f.state.files[1].objectId});
    REQUIRE(other != f.id);
    const auto original = *findComparison(f.state, other);
    CHECK_THROWS_AS(requestDifferenceMesh(f.state, other), std::invalid_argument);
    auto command = cli({"analysis", "difference-create", "object"});
    command.objectId = other;
    CHECK_THROWS_AS((void)controlDifference(f.state, f.runtime, command), std::invalid_argument);
    CHECK_THROWS_AS(requestComparisonDetector(f.state, f.id, DiagnosticCategory::boundary), std::invalid_argument);
    setAnalysisTask(f.state, f.id, AnalysisTask::meshChecks);
    CHECK(comparisonSettings(f.state, f.id).type == AnalysisType::differenceMesh);
    setAnalysisTask(f.state, other, AnalysisTask::differenceMesh);
    CHECK(comparisonSettings(f.state, other).type == AnalysisType::surfaceComparison);
    clearComparisonGroup(f.state, ComparisonSide::b, f.id);
    CHECK(comparisonPartCount(f.state, ComparisonSide::b, other) == 1);
    CHECK(comparisonSettings(f.state, other) == original.settings);
    CHECK(findComparison(f.state, other)->differenceRequest.revision == 0);
    CHECK_FALSE(comparisonSettings(f.state, other).difference.created);
}

TEST_CASE("difference runtime builds without comparison preparation detectors or heatmaps")
{
    Fixture f;
    f.comparison();
    ComparisonRuntimes runtimes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        updateComparisonRuntimes(runtimes, f.state);
        const auto &r = runtimes.objects.at(f.id);
        CHECK_FALSE(r.jobs.preparationWorker.valid());
        CHECK_FALSE(r.jobs.worker.valid());
        CHECK_FALSE(r.jobs.intersection.worker.valid());
        CHECK_FALSE(r.sources.prepared);
        CHECK(r.results.cache.completed == 0);
        if (r.difference.phase != "running" && r.difference.phase != "queued") { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    const auto &r = runtimes.objects.at(f.id).difference;
    CHECK(r.phase == "ready");
    CHECK(differenceResultCurrent(r, f.state, f.id));
    destroyComparisonRuntimes(runtimes);
}

TEST_CASE("difference visibility uses the analysis eye and preserves the result")
{
    Fixture f;
    f.comparison();
    f.wait();
    REQUIRE(differenceResultCurrent(f.runtime, f.state, f.id));
    const auto result = f.runtime.result;
    auto command = cli({"analysis", "difference-set", "object", "--visible", "false"});
    command.objectId = f.id;
    const auto status = controlDifference(f.state, f.runtime, command)["difference"];
    CHECK_FALSE(comparisonSettings(f.state, f.id).enabled);
    CHECK(status["settings"]["visible"] == false);
    CHECK(status["current"] == true);
    auto settings = comparisonSettings(f.state, f.id);
    settings.enabled = true;
    setComparisonSettings(f.state, settings, f.id);
    f.wait();
    CHECK(f.runtime.result == result);
    CHECK(differenceStatus(f.runtime, f.state, f.id)["settings"]["visible"] == true);
}

TEST_CASE("restoring a cleared difference through a view or undo starts a fresh build")
{
    Fixture f;
    f.comparison();
    f.wait();
    REQUIRE(differenceResultCurrent(f.runtime, f.state, f.id));
    const auto view = createView(f.state);
    const auto before = createSceneDocument(f.state);
    SceneHistory history;
    resetSceneHistory(history, f.state);
    auto command = cli({"analysis", "difference-delete", "object"});
    command.objectId = f.id;
    (void)controlDifference(f.state, f.runtime, command);
    REQUIRE(f.runtime.phase == "not_created");
    REQUIRE(findComparison(f.state, f.id)->differenceRequest.cancel);
    SUBCASE("saved view") { applyView(f.state, view); }
    SUBCASE("undo") {
        REQUIRE(recordSceneHistory(history, f.state));
        auto undo = prepareSceneHistoryStep(history, f.state, before, false);
        REQUIRE(undo);
        commitSceneHistoryStep(history, f.state, std::move(*undo), false);
    }
    REQUIRE(comparisonSettings(f.state, f.id).difference.created);
    CHECK_FALSE(findComparison(f.state, f.id)->differenceRequest.cancel);
    f.wait();
    CHECK(differenceResultCurrent(f.runtime, f.state, f.id));
    REQUIRE(f.runtime.result);
    CHECK(f.runtime.result->volumes == std::array<double, 2>{.5, .5});
}
