#include "automation_registry.h"
#include "command_line.h"
#include "difference_mesh.h"
#include "difference_runtime.h"
#include "obj_mesh.h"
#include "ui_operations.h"
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
DifferenceSettings approximateSettings(double cellSize = .1)
{
    DifferenceSettings settings;
    settings.method = DifferenceMethod::approximate;
    settings.cellSize = cellSize;
    return settings;
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
Mesh parametricCube()
{
    return loadObjMeshText("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
        "cstype bezier\ndeg 1 1\n"
        "surf 0 1 0 1 1 4 2 3\nparm u 0 1\nparm v 0 1\nend\n"
        "surf 0 1 0 1 5 6 8 7\nparm u 0 1\nparm v 0 1\nend\n"
        "surf 0 1 0 1 1 2 5 6\nparm u 0 1\nparm v 0 1\nend\n"
        "surf 0 1 0 1 2 3 6 7\nparm u 0 1\nparm v 0 1\nend\n"
        "surf 0 1 0 1 3 4 7 8\nparm u 0 1\nparm v 0 1\nend\n"
        "surf 0 1 0 1 4 1 8 5\nparm u 0 1\nparm v 0 1\nend\n");
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
    void wait(bool resolution = false)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            updateDifferenceRuntime(runtime, state, id, comparisonGeometrySignature(state, id));
            const auto &phase = resolution ? runtime.resolution.phase : runtime.phase;
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

TEST_CASE("binary level sets grow their surface table without collapsed vertices")
{
    // Equal magnitudes prevent Manifold's NearSurface pass from moving vertices.
    // These grids fill its initial sparse table and must grow before triangulation.
    for (const double cellSize : {.1, .05}) {
        CAPTURE(cellSize);
        const auto solid = manifold::Manifold::LevelSet([](manifold::vec3 p) {
            return p.x > 0 && p.x < 1 && p.y > 0 && p.y < 1 && p.z > 0 && p.z < 1 ? 1.0 : -1.0;
        }, {{-.2, -.2, -.2}, {1.2, 1.2, 1.2}}, cellSize, 0, -1, false);
        REQUIRE(solid.Status() == manifold::Manifold::Error::NoError);
        CHECK_FALSE(solid.IsEmpty());
        CHECK(std::abs(solid.Volume() - 1) < .2);
    }
}

TEST_CASE("approximate difference measures solids on a shared grid and improves with refinement")
{
    const auto coarse = buildDifferenceMesh(cube(), cube(.5), approximateSettings(.2));
    const auto fine = buildDifferenceMesh(cube(), cube(.5), approximateSettings(.05));
    CHECK_FALSE(fine.constructed);
    CHECK(fine.construction.method == DifferenceMethod::approximate);
    for (size_t i = 0; i < 2; ++i) {
        INFO(i, ": coarse=", coarse.volumes[i], ", fine=", fine.volumes[i]);
        CHECK(std::abs(fine.volumes[i] - .5) < .06);
        CHECK(std::abs(fine.volumes[i] - .5) < std::abs(coarse.volumes[i] - .5));
    }
    const auto identical = buildDifferenceMesh(cube(), subdivide(cube()), approximateSettings());
    CHECK(identical.volumes == std::array<double, 2>{0, 0});
    CHECK(meshGeometry(identical.parts[0]).indices.empty());
    const auto validated = buildDifferenceMesh(fine.parts[0], fine.parts[0]);
    CHECK(validated.volumes == std::array<double, 2>{0, 0});
    const auto disjoint = buildDifferenceMesh(cube(), cube(2), approximateSettings(.05));
    CHECK(std::abs(disjoint.volumes[0] - 1) < .12);
    CHECK(std::abs(disjoint.volumes[1] - 1) < .12);
}

TEST_CASE("approximate difference tolerates holes and non-manifold faces without editing inputs")
{
    auto a = subdivide(subdivide(cube()));
    auto &g = editMeshGeometry(a);
    SUBCASE("missing triangle") { g.indices.erase(g.indices.begin(), g.indices.begin() + 3); }
    SUBCASE("non-manifold duplicate face") {
        const std::array face{g.indices[0], g.indices[1], g.indices[2]};
        g.indices.insert(g.indices.end(), face.begin(), face.end());
    }
    SUBCASE("globally reversed winding") {
        for (size_t i = 0; i < g.indices.size(); i += 3) { std::swap(g.indices[i], g.indices[i + 1]); }
    }
    publishMeshGeometry(a);
    a = withCollapsedTriangles(a);
    const auto indices = meshGeometry(a).indices;
    const auto positions = meshGeometry(a).precisePositions;
    const auto result = buildDifferenceMesh(a, cube(.5), approximateSettings(.1));
    CHECK(result.removedCollapsedTriangles[0] == 3);
    CHECK(std::abs(result.volumes[0] - .5) < .15);
    CHECK(std::abs(result.volumes[1] - .5) < .15);
    CHECK(meshGeometry(a).indices == indices);
    CHECK(meshGeometry(a).precisePositions == positions);
    const auto identical = buildDifferenceMesh(a, a, approximateSettings());
    CHECK(identical.volumes == std::array<double, 2>{0, 0});
}

TEST_CASE("approximate difference accepts tessellated parametric solids and preserves world placement")
{
    auto a = parametricCube();
    REQUIRE(a.freeform);
    const auto indices = meshGeometry(a).indices;
    auto b = cube(.5);
    a.origin = {1e9, 2e9, 3e9};
    b.origin = a.origin;
    const auto result = buildDifferenceMesh(a, b, approximateSettings(.125));
    const auto reverse = buildDifferenceMesh(b, a, approximateSettings(.125));
    for (size_t i = 0; i < 2; ++i) {
        CHECK(std::abs(result.volumes[i] - .5) < .16);
        CHECK(result.volumes[i] == doctest::Approx(reverse.volumes[1 - i]).epsilon(1e-9));
    }
    CHECK(originalMeshBounds(result.parts[0])[0][0] > 1e9 - .2);
    CHECK(originalMeshBounds(result.parts[0])[1][0] < 1e9 + .7);
    CHECK(meshGeometry(a).indices == indices);
}

TEST_CASE("approximate difference rejects invalid settings oversized grids and surfaces without a solid")
{
    for (const double size : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), cube(.5), approximateSettings(size)),
                             doctest::Contains("finite and positive"), std::invalid_argument);
    }
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), cube(.5), approximateSettings(1e-5)),
                         doctest::Contains("grid limit"), std::runtime_error);
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(sheet(0), cube(), approximateSettings()),
                         doctest::Contains("Input A: No solid volume"), std::runtime_error);
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), sheet(0), approximateSettings()),
                         doctest::Contains("Input B: No solid volume"), std::runtime_error);
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS_WITH_AS((void)buildDifferenceMesh(cube(), cube(.5), approximateSettings(), stop.get_token()),
                         doctest::Contains("canceled"), std::runtime_error);
}

TEST_CASE("approximate difference measures a curved parametric face against a triangular solid")
{
    // The quadratic top is z = 1 + 4u(1-u)v(1-v). Its added volume is 1/9.
    // Its tessellated boundary also has T-junctions against the coarse triangle sides.
    const auto curved = loadObjMeshText(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 1 1 1\nv 0 1 1\n"
        "f 1 3 2\nf 1 4 3\nf 1 2 6\nf 1 6 5\nf 2 3 7\nf 2 7 6\nf 3 4 8\nf 3 8 7\nf 4 1 5\nf 4 5 8\n"
        "v 0 0 1\nv .5 0 1\nv 1 0 1\nv 0 .5 1\nv .5 .5 2\nv 1 .5 1\nv 0 1 1\nv .5 1 1\nv 1 1 1\n"
        "cstype bezier\ndeg 2 2\nsurf 0 1 0 1 9 10 11 12 13 14 15 16 17\nparm u 0 1\nparm v 0 1\nend\n");
    const auto result = buildDifferenceMesh(curved, cube(), approximateSettings(.0625));
    CHECK(std::abs(result.volumes[0] - 1.0 / 9) < .025);
    CHECK(result.volumes[1] < 1e-8);
    checkClosedEdges(result.parts[0]);
}

TEST_CASE("approximate difference preserves oriented cavities and accepts overlapping components")
{
    auto a = cube();
    auto inner = cube(.25);
    bool cavity = false;
    SUBCASE("overlapping components") {}
    SUBCASE("oriented cavity") {
        cavity = true;
        inner = cube(.25, .5, {0, .25, .25});
        auto &g = editMeshGeometry(inner);
        for (size_t i = 0; i < g.indices.size(); i += 3) { std::swap(g.indices[i], g.indices[i + 1]); }
        publishMeshGeometry(inner);
    }
    auto &g = editMeshGeometry(a);
    const auto first = static_cast<uint32_t>(g.vertices.size());
    const auto &ig = meshGeometry(inner);
    for (size_t i = 0; i < ig.vertices.size(); ++i) {
        auto p = meshPosition(inner, static_cast<uint32_t>(i));
        for (size_t k = 0; k < 3; ++k) { p[k] += inner.origin[k]; }
        Vertex vertex;
        vertex.position = renderPosition(p);
        g.vertices.push_back(vertex);
        g.precisePositions.push_back(p);
    }
    for (const auto index : ig.indices) { g.indices.push_back(first + index); }
    publishMeshGeometry(a);
    const auto result = buildDifferenceMesh(a, cavity ? cube() : cube(.5), approximateSettings(.05));
    checkClosedEdges(result.parts[0]);
    checkClosedEdges(result.parts[1]);
    if (cavity) {
        CHECK(result.volumes[0] < 1e-8);
        CHECK(std::abs(result.volumes[1] - .125) < .04);
    } else {
        CHECK(std::abs(result.volumes[0] - .5) < .06);
        CHECK(std::abs(result.volumes[1] - .25) < .04);
    }
}

TEST_CASE("approximate settings persist and invalidate only construction results")
{
    Fixture f;
    f.comparison();
    auto settings = comparisonSettings(f.state, f.id);
    settings.difference = approximateSettings(.1);
    settings.difference.visible = true;
    setComparisonSettings(f.state, settings, f.id);
    f.wait();
    CHECK(f.runtime.phase == "not_created");
    CHECK(comparisonSettings(f.state, f.id).difference.visible);
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    INFO(f.runtime.error);
    REQUIRE(f.runtime.phase == "ready");
    CHECK(differenceStatus(f.runtime, f.state, f.id)["kind"] == "approximate_volume");
    const auto original = f.runtime.result;
    requestDifferenceResolutionCheck(f.state, f.id);
    f.wait(true);
    INFO(f.runtime.resolution.error);
    REQUIRE(f.runtime.resolution.phase == "ready");
    CHECK(f.runtime.result == original);
    CHECK(f.runtime.resolution.cellSize == .05);
    CHECK(differenceStatus(f.runtime, f.state, f.id)["resolutionCheck"]["cellSize"] == .05);
    settings = comparisonSettings(f.state, f.id);
    settings.difference.showB = false;
    settings.difference.opacity = .25f;
    settings.tolerance = .9f;
    setComparisonSettings(f.state, settings, f.id);
    CHECK(differenceResultCurrent(f.runtime, f.state, f.id));
    const auto path = f.root / "approximate.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    const auto restored = prepareSceneReplacement(f.state, f.state.files, readSceneDocument(path));
    REQUIRE(restored.comparisons.size() == 1);
    CHECK(restored.comparisons[0].settings.difference == settings.difference);
    settings.difference.cellSize = .2;
    setComparisonSettings(f.state, settings, f.id);
    CHECK_FALSE(differenceResultCurrent(f.runtime, f.state, f.id));
    CHECK(differenceStatus(f.runtime, f.state, f.id)["totalVolume"].is_null());
    CHECK_FALSE(differenceStatus(f.runtime, f.state, f.id)["resolutionCheck"].contains("totalVolume"));
    f.wait();
    CHECK(f.runtime.phase == "outdated");
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    REQUIRE(f.runtime.phase == "ready");
    CHECK(f.runtime.result->construction.cellSize == .2);
    settings = comparisonSettings(f.state, f.id);
    settings.difference.method = DifferenceMethod::direct;
    setComparisonSettings(f.state, settings, f.id);
    CHECK_FALSE(differenceResultCurrent(f.runtime, f.state, f.id));
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    CHECK(differenceStatus(f.runtime, f.state, f.id)["totalVolume"] == 1);
}

TEST_CASE("approximate workers discard obsolete builds and canceled resolution checks")
{
    Fixture f;
    f.comparison();
    auto settings = comparisonSettings(f.state, f.id);
    settings.difference = approximateSettings(.1);
    setComparisonSettings(f.state, settings, f.id);
    requestDifferenceMesh(f.state, f.id);
    updateDifferenceRuntime(f.runtime, f.state, f.id, comparisonGeometrySignature(f.state, f.id));
    REQUIRE(f.runtime.worker.valid());
    settings = comparisonSettings(f.state, f.id);
    settings.difference.cellSize = .2;
    setComparisonSettings(f.state, settings, f.id);
    f.runtime.worker.wait();
    f.wait();
    CHECK(f.runtime.phase == "outdated");
    CHECK_FALSE(f.runtime.result);
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    REQUIRE(f.runtime.phase == "ready");
    requestDifferenceResolutionCheck(f.state, f.id);
    updateDifferenceRuntime(f.runtime, f.state, f.id, comparisonGeometrySignature(f.state, f.id));
    REQUIRE(f.runtime.resolution.worker.valid());
    requestDifferenceResolutionCheck(f.state, f.id, true);
    f.runtime.resolution.worker.wait();
    f.wait(true);
    CHECK(f.runtime.resolution.phase == "canceled");
    CHECK(differenceResultCurrent(f.runtime, f.state, f.id));
    CHECK_FALSE(differenceStatus(f.runtime, f.state, f.id)["resolutionCheck"].contains("totalVolume"));
}

TEST_CASE("approximate difference CLI validates settings and supports configuration before creation")
{
    const auto command = cli({"analysis", "difference-set", "object", "--method", "approximate", "--cell-size", "0.125"});
    CHECK(command.method == "approximate");
    CHECK(command.cellSize == .125);
    CHECK(controlOperationParams(parseControlOperation(controlMethod(command.action), controlOperationParams(command))) ==
          controlOperationParams(command));
    CHECK_THROWS(cli({"analysis", "difference-set", "object", "--method", "unknown"}));
    CHECK_THROWS(cli({"analysis", "difference-set", "object", "--cell-size", "0"}));
    CHECK_THROWS(cli({"analysis", "difference-set", "object", "--cell-size", "-1"}));
    Fixture f;
    f.comparison();
    auto create = cli({"analysis", "difference-create", "object", "--method", "approximate", "--cell-size", "0.125"});
    create.objectId = f.id;
    (void)controlDifference(f.state, f.runtime, create);
    f.wait();
    CHECK(differenceStatus(f.runtime, f.state, f.id)["method"] == "approximate");
    auto settings = comparisonSettings(f.state, f.id);
    settings.difference.cellSize = std::numeric_limits<double>::quiet_NaN();
    setComparisonSettings(f.state, settings, f.id);
    CHECK(comparisonSettings(f.state, f.id).difference.cellSize == .5);
}

TEST_CASE("difference CLI checks resolution and reports changes without replacing the base result")
{
    Fixture f;
    f.comparison();
    const auto run = [&](std::vector<std::string> words) {
        auto command = cli(std::move(words));
        command.objectId = f.id;
        return controlDifference(f.state, f.runtime, command)["difference"];
    };
    CHECK_THROWS(run({"analysis", "difference-check-resolution", "object"}));
    run({"analysis", "difference-create", "object"});
    f.wait();
    CHECK_THROWS(run({"analysis", "difference-check-resolution", "object"}));
    run({"analysis", "difference-rebuild", "object", "--method", "approximate", "--cell-size", "0.2"});
    f.wait();
    REQUIRE(differenceResultCurrent(f.runtime, f.state, f.id));
    const auto original = f.runtime.result;
    const auto total = original->volumes[0] + original->volumes[1];
    auto status = run({"analysis", "difference-check-resolution", "object"});
    CHECK(status["resolutionCheck"]["state"] == "running");
    CHECK_FALSE(status["resolutionCheck"].contains("totalVolume"));
    f.wait(true);
    status = run({"analysis", "difference-get", "object"});
    const auto &check = status["resolutionCheck"];
    REQUIRE(check["state"] == "ready");
    CHECK(check["baseCellSize"] == .2);
    CHECK(check["cellSize"] == .1);
    CHECK(check["totalVolume"].get<double>() == check["volumeA"].get<double>() + check["volumeB"].get<double>());
    CHECK(check["netVolume"].get<double>() == check["volumeB"].get<double>() - check["volumeA"].get<double>());
    CHECK(check["totalVolumeChange"].get<double>() == doctest::Approx(check["totalVolume"].get<double>() - total));
    CHECK(check["relativeChangePercent"].get<double>() == doctest::Approx(100 * check["totalVolumeChange"].get<double>() / total));
    CHECK(status["totalVolume"] == total);
    CHECK(f.runtime.result == original);

    run({"analysis", "difference-check-resolution", "object"});
    REQUIRE(f.runtime.resolution.worker.valid());
    status = run({"analysis", "difference-cancel-check", "object"});
    CHECK(status["resolutionCheck"]["state"] == "canceled");
    f.runtime.resolution.worker.wait();
    f.wait(true);
    CHECK(f.runtime.resolution.phase == "canceled");
    CHECK(differenceResultCurrent(f.runtime, f.state, f.id));
    CHECK(f.runtime.result == original);
    CHECK_FALSE(differenceStatus(f.runtime, f.state, f.id)["resolutionCheck"].contains("totalVolume"));

    run({"analysis", "difference-set", "object", "--cell-size", "0.3"});
    CHECK_THROWS(run({"analysis", "difference-check-resolution", "object"}));
    CHECK_FALSE(run({"analysis", "difference-get", "object"})["resolutionCheck"].contains("totalVolume"));
    run({"analysis", "difference-set", "object", "--method", "direct"});
    CHECK_NOTHROW(run({"analysis", "difference-cancel-check", "object"}));
}

TEST_CASE("resolution status handles a zero base volume and failed reconstruction")
{
    Fixture f;
    f.comparison(cube(), cube());
    auto settings = comparisonSettings(f.state, f.id);
    settings.difference = approximateSettings(.2);
    setComparisonSettings(f.state, settings, f.id);
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    REQUIRE(differenceResultCurrent(f.runtime, f.state, f.id));
    auto command = cli({"analysis", "difference-check-resolution", "object"});
    command.objectId = f.id;
    (void)controlDifference(f.state, f.runtime, command);
    SUBCASE("identical inputs") {
        f.wait(true);
        const auto check = differenceStatus(f.runtime, f.state, f.id)["resolutionCheck"];
        REQUIRE(check["state"] == "ready");
        CHECK(check["totalVolumeChange"] == 0);
        CHECK(check["relativeChangePercent"].is_null());
    }
    SUBCASE("failed check retains current base") {
        // A worker failure must be returned by get, while base volumes stay available.
        f.runtime.resolution.worker.wait();
        f.runtime.resolution.worker = std::async(std::launch::async, []() -> std::array<double, 2> {
            throw std::runtime_error("Grid allocation failed");
        });
        f.wait(true);
        const auto status = differenceStatus(f.runtime, f.state, f.id);
        CHECK(status["current"] == true);
        CHECK(status["resolutionCheck"]["state"] == "failed");
        CHECK(status["resolutionCheck"]["error"] == "Grid allocation failed");
        CHECK_FALSE(status["resolutionCheck"].contains("totalVolume"));
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
        CHECK_FALSE(r.constructed);
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
TEST_CASE("difference removes exact collapsed faces from both solid inputs without changing the sources")
{
    const auto a = withCollapsedTriangles(cube());
    const auto b = withCollapsedTriangles(withCollapsedTriangles(cube(.5)));
    const auto aIndices = meshGeometry(a).indices, bIndices = meshGeometry(b).indices;
    const auto aPositions = meshGeometry(a).precisePositions, bPositions = meshGeometry(b).precisePositions;
    const auto r = buildDifferenceMesh(a, b);
    CHECK_FALSE(r.constructed);
    CHECK(r.removedCollapsedTriangles == std::array<size_t, 2>{3, 6});
    CHECK(r.volumes[0] == doctest::Approx(.5));
    CHECK(r.volumes[1] == doctest::Approx(.5));
    CHECK(r.message.find("A: 3, B: 6") != std::string::npos);
    CHECK(meshGeometry(a).indices == aIndices);
    CHECK(meshGeometry(b).indices == bIndices);
    CHECK(meshGeometry(a).precisePositions == aPositions);
    CHECK(meshGeometry(b).precisePositions == bPositions);
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
TEST_CASE("difference cleanup excludes unused points from open surface results")
{
    const auto r = buildDifferenceMesh(withCollapsedTriangles(sheet(0)), sheet(.25, true));
    CHECK(r.constructed);
    CHECK(r.removedCollapsedTriangles == std::array<size_t, 2>{3, 0});
    CHECK(r.volumes[0] == doctest::Approx(.25));
    CHECK(r.closureTriangles == 9);
    for (const auto part : {0, 2}) {
        CHECK(r.parts[part].bounds.max == std::array<float, 3>{1, 1, .25f});
        CHECK(r.parts[part].bounds.min == std::array<float, 3>{0, 0, 0});
    }
}
TEST_CASE("difference runtime accepts a parametric collapsed boundary and reports cleanup counts")
{
    Fixture f;
    const auto parametric = loadObjMeshText(
        "v 0 0 0\nv 0 0 0\nv 0 1 0\nv 1 1 0\n"
        "cstype bezier\ndeg 1 1\nsurf 0 1 0 1 1 2 3 4\nparm u 0 1\nparm v 0 1\nend\n");
    const auto triangular = surface({{0, 0, .25}, {1, 1, .25}, {0, 1, .25}}, {0, 1, 2});
    const auto sourceIndices = meshGeometry(parametric).indices;
    REQUIRE(sourceIndices.size() / 3 == 2048);
    bool reverse = false;
    SUBCASE("parametric input A") {}
    SUBCASE("parametric input B") { reverse = true; }
    f.comparison(reverse ? triangular : parametric, reverse ? parametric : triangular);
    requestDifferenceMesh(f.state, f.id);
    f.wait();
    INFO(f.runtime.error);
    REQUIRE(f.runtime.phase == "ready");
    REQUIRE(f.runtime.result);
    CHECK(f.runtime.result->constructed);
    CHECK(f.runtime.result->volumes[0] == doctest::Approx(.125));
    CHECK(meshGeometry(f.runtime.result->parts[0]).indices.size() / 3 == 2017);
    CHECK(meshGeometry(f.state.files[reverse ? 1 : 0].mesh).indices == sourceIndices);
    CHECK(f.state.files[reverse ? 1 : 0].mesh.freeform == parametric.freeform);
    const auto status = differenceStatus(f.runtime, f.state, f.id);
    CHECK(status["removedCollapsedTriangles"]["a"] == (reverse ? 0 : 32));
    CHECK(status["removedCollapsedTriangles"]["b"] == (reverse ? 32 : 0));
    CHECK(f.runtime.result->message.find(reverse ? "A: 0, B: 32" : "A: 32, B: 0") != std::string::npos);
    selectSceneObject(f.state, f.state.files[0].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 2);
    CHECK_FALSE(differenceStatus(f.runtime, f.state, f.id).contains("removedCollapsedTriangles"));
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
    f.comparison(cube(), cube(.5), false);
    REQUIRE(comparisonSettings(f.state, f.id).alignOrigins);
    REQUIRE(comparisonSettings(f.state, f.id).lockPlacement);
    SUBCASE("direct") {}
    SUBCASE("approximate") {
        auto settings = comparisonSettings(f.state, f.id);
        settings.difference = approximateSettings(.2);
        setComparisonSettings(f.state, settings, f.id);
    }
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
    settings.difference = approximateSettings(.2);
    setComparisonSettings(f.state, settings, f.id);
    requestDifferenceMesh(f.state, f.id);
    requestDifferenceResolutionCheck(f.state, f.id);
    const auto duplicate = duplicateComparison(f.state, f.id);
    const auto* copy = findComparison(f.state, duplicate);
    REQUIRE(copy);
    CHECK(copy->settings.difference == comparisonSettings(f.state, f.id).difference);
    CHECK(copy->differenceRequest.revision == 0);
    CHECK(copy->differenceResolutionRequest.revision == 0);
    CHECK_FALSE(copy->differenceResolutionRequest.cancel);
    CHECK(findComparison(f.state, f.id)->differenceResolutionRequest.revision > 0);
}
