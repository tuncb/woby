#include "mesh_topology.h"
#include "mesh_comparison.h"
#include "comparison_scene.h"
#include "comparison_view.h"
#include "comparison_report.h"
#include "control_scene.h"
#include "scene_history.h"
#include "scene_pick.h"
#include "obj_mesh.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numbers>
#include <random>

namespace {
using namespace woby;
using Point = std::array<double, 3>;
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-topology-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path write(const char* name, const std::string& text) const {
        const auto path = root/name; std::ofstream out(path); out << text; return path;
    }
};
DuplicateSource sourceFor(std::vector<Point> points = {{0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}},
    std::vector<uint32_t> indices = {0,1,2, 0,2,3})
{
    auto data = std::make_shared<SourceMeshData>();
    data->points = std::move(points); data->indices = std::move(indices);
    data->provenance = SourceProvenance::objPositions;
    SourcePartInstance part;
    part.partId = 2; part.indexCount = data->indices.size();
    part.transform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    return {1, "test.obj", std::move(data), true, {part}};
}
Mesh meshFor(const DuplicateSource& source)
{
    Mesh mesh;
    for (const auto& point : source.data->points) {
        Vertex v; for (size_t k = 0; k < 3; ++k) { v.position[k] = static_cast<float>(point[k]); }
        mesh.vertices.push_back(v);
    }
    mesh.indices = source.data->indices; mesh.bounds = calculateBounds(mesh.vertices);
    auto input = std::make_shared<DuplicateInput>(); input->sources.push_back(source); mesh.duplicateInput = input;
    return mesh;
}
nlohmann::json jsonFor(const MeshTopology& topology, const char* detector)
{
    MeshComparison result; result.original.source.indices = {0,1,2}; result.original.topology = topology;
    return controlComparisonResults(result, .05)["aToB"]["detectors"][detector];
}
}

TEST_CASE("shared topology retains disk incidence links boundaries and closed components")
{
    for (const auto mode : {TopologyMode::automatic, TopologyMode::originalIndex, TopologyMode::exactPosition}) {
        const auto disk = buildMeshTopology({sourceFor()}, mode);
        REQUIRE(disk.sources.size() == 1);
        const auto& source = disk.sources[0];
        CHECK(source.vertices.size() == 4); CHECK(source.faces.size() == 2); CHECK(source.edges.size() == 5);
        CHECK(source.components.size() == 1); CHECK(source.components[0].size() == 2);
        CHECK(source.vertices[0].link.size() == 2); CHECK(source.vertices[0].boundaryEdges.size() == 2);
        CHECK(disk.boundaries.size() == 4); CHECK(disk.nonManifoldEdges.empty()); CHECK(disk.windingFaces.empty());
        const auto tetra = buildMeshTopology({sourceFor({{0,0,0},{1,0,0},{0,1,0},{0,0,1}}, {0,2,1, 0,1,3, 1,2,3, 2,0,3})}, mode);
        CHECK(tetra.boundaries.empty()); CHECK(tetra.windingEdges.empty()); CHECK(tetra.sources[0].components.size() == 1);
    }
}

TEST_CASE("topology distinguishes original IDs exact positions signed zero and near positions")
{
    const auto source = sourceFor({{0,0,0},{1,0,0},{1,1,0},{-0.0,0,0},{1,1,0},{0,1,0}}, {0,1,2, 3,4,5});
    CHECK(buildMeshTopology({source}).boundaries.size() == 6);
    const auto welded = buildMeshTopology({source}, TopologyMode::exactPosition);
    CHECK(welded.boundaries.size() == 4);
    CHECK(welded.sources[0].vertices[0].references.size() == 2);
    auto near = source;
    auto data = std::make_shared<SourceMeshData>(*near.data); data->points[3][0] = 1e-16; near.data = data;
    CHECK(buildMeshTopology({near}, TopologyMode::exactPosition).boundaries.size() == 6);
    // Differences below float resolution stay distinct in the topology snapshot.
    data->points[3] = {1+1e-12, 0, 0};
    CHECK(buildMeshTopology({near}, TopologyMode::exactPosition).sources[0].vertices.size() == 5);
}

TEST_CASE("topology defaults for STL are explicit and unavailable is not a clean result")
{
    auto stl = sourceFor(); auto data = std::make_shared<SourceMeshData>(*stl.data);
    data->provenance = SourceProvenance::stlCorners; stl.data = data;
    auto result = buildMeshTopology({stl});
    CHECK(result.sources[0].mode == TopologyMode::exactPosition); CHECK(result.availableSources == 1);
    result = buildMeshTopology({stl}, TopologyMode::originalIndex);
    CHECK(std::string(topologyStatus(result)) == "unavailable");
    CHECK(jsonFor(result, "non_manifold_edges")["count"].is_null());
    auto obj = sourceFor(); obj.fileId = 3;
    result = buildMeshTopology({stl, obj}, TopologyMode::originalIndex);
    CHECK(std::string(topologyStatus(result)) == "partial");
    CHECK(result.availableSources == 1); CHECK(result.unavailableSources == 1);
    obj.data.reset();
    CHECK(buildMeshTopology({obj}).unavailableSources == 1);
}

TEST_CASE("topology never welds files and partitions original IDs by transformed instance")
{
    auto source = sourceFor(); auto other = source; other.fileId = 7;
    for (const auto mode : {TopologyMode::originalIndex, TopologyMode::exactPosition}) {
        const auto result = buildMeshTopology({other, source}, mode);
        CHECK(result.boundaries.size() == 8); CHECK(result.windingFaces.empty());
        CHECK(result.sources[0].fileId == 1); CHECK(result.sources[1].fileId == 7);
    }
    source.parts[0].indexCount = 3;
    auto second = source.parts[0]; second.partId = 3; second.firstIndex = 3;
    source.parts.push_back(second);
    CHECK(buildMeshTopology({source}).boundaries.size() == 4);
    source.parts[1].transform[12] = 10;
    CHECK(buildMeshTopology({source}).boundaries.size() == 6);
    CHECK(buildMeshTopology({source}, TopologyMode::exactPosition).boundaries.size() == 6);
    const auto before = jsonFor(buildMeshTopology({source}), "boundary_edges");
    std::reverse(source.parts.begin(), source.parts.end());
    CHECK(jsonFor(buildMeshTopology({source}), "boundary_edges") == before);
    source.parts.push_back(source.parts.front());
    CHECK(jsonFor(buildMeshTopology({source}), "boundary_edges") == before);
}

TEST_CASE("topology retains duplicate faces and every incident reference but excludes collapsed faces")
{
    const auto source = sourceFor({{0,0,0},{1,0,0},{1,1,0},{0,1,0}}, {0,1,2, 0,2,3, 0,1,2, 0,0,1});
    const auto result = buildMeshTopology({source});
    CHECK(result.excludedCollapsedFaces == 1); REQUIRE(result.nonManifoldEdges.size() == 1);
    const auto json = jsonFor(result, "non_manifold_edges");
    CHECK(json["count"] == 1); CHECK(json["findings"][0]["incidentFaceCount"] == 3);
    CHECK(json["findings"][0]["incidentFaces"][0]["triangleId"] == 1);
    CHECK(json["findings"][0]["incidentFaces"][1]["triangleId"] == 2);
    CHECK(json["findings"][0]["incidentFaces"][2]["triangleId"] == 3);
    CHECK(json["findings"][0]["incidentFaces"][0]["partId"] == "2");
    CHECK(buildMeshTopology({sourceFor({{0,0,0},{1,0,0},{0,1e-14,0}}, {0,1,2})}).boundaries.size() == 3);
}

TEST_CASE("winding findings report both faces and detect orientation contradictions")
{
    auto result = buildMeshTopology({sourceFor({{0,0,0},{1,0,0},{1,1,0},{0,1,0}}, {0,1,2, 0,3,2})});
    REQUIRE(result.windingEdges.size() == 1); CHECK(result.windingFaces.size() == 2);
    CHECK(result.orientationContradictions == 0);
    const auto json = jsonFor(result, "inconsistently_oriented_tris");
    CHECK(json["count"] == 2); CHECK(json["findingCount"] == 1); CHECK(json["findings"][0]["sameDirection"] == true);
    // Five quads forming a Mobius strip: the seam swaps the two endpoints.
    std::vector<Point> points; std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < 5; ++i) {
        const double angle = i * 2 * std::numbers::pi / 5;
        for (const double offset : {-.2, .2}) {
            points.push_back({(1+offset*std::cos(angle/2))*std::cos(angle),
                (1+offset*std::cos(angle/2))*std::sin(angle), offset*std::sin(angle/2)});
        }
        const uint32_t a = i*2, b = a+1, c = i == 4 ? 1 : a+2, d = i == 4 ? 0 : a+3;
        indices.insert(indices.end(), {a,c,d, a,d,b});
    }
    result = buildMeshTopology({sourceFor(points, indices)});
    CHECK(result.excludedCollapsedFaces == 0); CHECK(result.orientationContradictions > 0);
    CHECK(result.nonManifoldEdges.empty()); CHECK(result.sources[0].components.size() == 1);
}

TEST_CASE("topology validates input cancellation and bounds JSON without losing counts")
{
    auto source = sourceFor(); auto data = std::make_shared<SourceMeshData>(*source.data); source.data = data;
    data->indices[0] = 999; CHECK_THROWS((void)buildMeshTopology({source})); data->indices[0] = 0;
    data->points[0][0] = std::numeric_limits<double>::infinity(); CHECK_THROWS((void)buildMeshTopology({source})); data->points[0][0] = 0;
    source.parts[0].indexCount = 999; CHECK_THROWS((void)buildMeshTopology({source})); source.parts[0].indexCount = 6;
    source.parts[0].transform[0] = std::numeric_limits<float>::quiet_NaN(); CHECK_THROWS((void)buildMeshTopology({source}));
    std::stop_source stop; stop.request_stop(); CHECK_THROWS((void)buildMeshTopology({}, {}, stop.get_token()));
    data->indices.clear(); for (size_t i = 0; i < 120; ++i) { data->indices.insert(data->indices.end(), {0,1,2}); }
    source = sourceFor(data->points, data->indices);
    const auto json = jsonFor(buildMeshTopology({source}), "non_manifold_edges");
    CHECK(json["count"] == 3); CHECK(json["findings"][0]["incidentFaceCount"] == 120);
    CHECK(json["findings"][0]["incidentFaces"].size() == 100); CHECK(json["findings"][0]["incidentFacesTruncated"] == true);
    std::vector<DuplicateSource> many;
    for (uint64_t i = 1; i <= 120; ++i) { auto item = sourceFor(); item.fileId = i; many.push_back(item); }
    const auto boundaries = jsonFor(buildMeshTopology(many), "boundary_edges");
    CHECK(boundaries["count"] == 480); CHECK(boundaries["findings"].size() == 100); CHECK(boundaries["findingsTruncated"] == true);
    CHECK(boundaries["sources"].size() == 100); CHECK(boundaries["sourcesTruncated"] == true);
}

TEST_CASE("topology mode changes invalidate only topology and reject stale stage results")
{
    const auto mesh = meshFor(sourceFor());
    ComparisonSettings settings;
    ComparisonCacheStatus cache{17, 0}; MeshComparison result;
    const auto stages = requestedComparisonStages(settings, true, true);
    REQUIRE(applyComparisonStages(result, cache, computeComparisonStages(mesh, mesh, stages), 17, stages));
    const auto* distances = result.original.distances.data(); const auto* quality = result.original.quality.triangles.data();
    auto stale = computeComparisonStages(mesh, {}, comparisonTopology | comparisonDuplicatePoints);
    REQUIRE(resetComparisonTopologyCache(cache, TopologyMode::exactPosition));
    CHECK(cache.completed == (stages & ~comparisonTopology));
    CHECK(applyComparisonStages(result, cache, std::move(stale), 17, comparisonTopology | comparisonDuplicatePoints));
    CHECK((cache.completed & comparisonTopology) == 0);
    REQUIRE(applyComparisonStages(result, cache, computeComparisonStages(mesh, mesh, comparisonTopology, {}, {}, TopologyMode::exactPosition), 17, comparisonTopology));
    CHECK(cache.completed == stages); CHECK(result.original.distances.data() == distances); CHECK(result.original.quality.triangles.data() == quality);
    CHECK_FALSE(resetComparisonTopologyCache(cache, TopologyMode::exactPosition));
    CHECK(normalizedTopologyMode(static_cast<TopologyMode>(99)) == TopologyMode::automatic);
}

TEST_CASE("topology settings navigation CLI and independent visibility roundtrip through scene operations")
{
    Fixture fixture;
    const auto path = fixture.write("test.obj", "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 4 3\n");
    UiState state; state.files.push_back(createUiFileState(path, loadObjMesh(path), 0)); appendDefaultSceneNodesForFiles(state, 0);
    const auto id = createComparison(state); setComparisonObjects(state, {state.files[0].objectId}, ComparisonSide::a, true, id);
    setComparisonTranslation(state, id, {100,0,0});
    auto settings = comparisonSettings(state, id); settings.diagnosticCategory = DiagnosticCategory::winding; setComparisonSettings(state, settings, id);
    const auto input = comparisonWorldMesh(state, ComparisonSide::a, id); const auto result = compareMeshes(input, {});
    const auto signature = comparisonGeometrySignature(state, id);
    REQUIRE(result.original.topology.windingFaces.size() == 2);
    selectComparisonDiagnostic(state, result, signature, 0, id); REQUIRE(findComparison(state, id)->diagnosticFocus);
    CHECK(state.camera.target[0] == doctest::Approx(100.5));
    const auto clean = createSceneDocument(state);
    auto command = parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"}, {"showNonManifold", false}, {"showWinding", true}, {"topologyMode", "exact_position"}});
    CHECK(controlOperationParams(command)["topologyMode"] == "exact_position"); command.objectId = id;
    (void)applyControlSceneOperation(state, clean, command, [](auto value) { return std::to_string(value); }, 200, 800);
    CHECK(state.isDirty); CHECK(comparisonGeometrySignature(state, id) == signature);
    CHECK_FALSE(findComparison(state, id)->diagnosticFocus);
    selectComparisonDiagnostic(state, result, signature, 0, id); CHECK_FALSE(findComparison(state, id)->diagnosticFocus);
    CHECK_FALSE(comparisonSettings(state, id).showNonManifold); CHECK(comparisonSettings(state, id).showWinding);
    ComparisonRuntime runtime; runtime.ready = true; runtime.resultSignature = signature; runtime.cache = {signature, requestedComparisonStages(settings, false)};
    CHECK_FALSE(comparisonResultsReady(runtime, state, id));
    const auto saved = fixture.root/"scene.woby"; writeSceneDocument(saved, createSceneDocument(state));
    CHECK(readSceneDocument(saved).comparisons[0].settings == comparisonSettings(state, id));
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"}, {"topologyMode", "near"}}));
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"}, {"showWinding", "true"}}));
    CHECK_THROWS((void)readSceneDocument(fixture.write("bad.woby", "version = 10\n[[analyses]]\ntopology_mode = \"near\"\n")));
}

TEST_CASE("old scenes migrate combined visibility including saved views independent of key order")
{
    Fixture fixture;
    for (int version = 2; version <= 9; ++version) {
        const auto old = fixture.write("old.woby", "version = " + std::to_string(version)
            + "\n[[analyses]]\nanalysis_show_non_manifold = false\n");
        const auto document = readSceneDocument(old);
        CHECK_FALSE(document.comparisons[0].settings.showNonManifold); CHECK_FALSE(document.comparisons[0].settings.showWinding);
    }
    for (const auto& keys : {std::string("analysis_show_winding = true\nanalysis_show_non_manifold = false\n"),
        std::string("analysis_show_non_manifold = false\nanalysis_show_winding = true\n")}) {
        const auto document = readSceneDocument(fixture.write("new.woby", "version = 10\n[[analyses]]\n" + keys));
        CHECK(document.comparisons[0].settings.showWinding); CHECK_FALSE(document.comparisons[0].settings.showNonManifold);
    }
    const auto document = readSceneDocument(fixture.write("view.woby", "version = 9\n[[analyses]]\nname = \"test\"\n"
        "[[views]]\nname = \"test\"\n[[views.objects]]\nkind = \"analysis\"\nindex = 0\nanalysis_show_non_manifold = false\n"));
    REQUIRE(document.views.size() == 1); REQUIRE(document.views[0].objects.size() == 1);
    CHECK_FALSE(document.views[0].objects[0].settings.comparison.showWinding);
    auto updated = document;
    updated.views[0].objects[0].settings.comparison.topologyMode = TopologyMode::exactPosition;
    updated.views[0].objects[0].settings.comparison.showWinding = true;
    writeSceneDocument(fixture.root/"view-new.woby", updated);
    CHECK(readSceneDocument(fixture.root/"view-new.woby").views[0].objects[0].settings.comparison
        == updated.views[0].objects[0].settings.comparison);
}

TEST_CASE("topology winding visibility controls picking and reports independently without recomputation")
{
    const auto source = sourceFor({{0,0,0},{1,0,0},{1,1,0},{0,1,0}}, {0,1,2, 0,3,2});
    auto result = compareMeshes(meshFor(source), {});
    UiComparison comparison;
    comparison.objectId = 5; comparison.settings.enabled = true;
    auto settings = comparison.settings; settings.mode = ComparisonMode::original;
    settings.showBoundaries = false; settings.showNonManifold = false; settings.showWinding = true;
    std::vector<ScenePickPart> parts;
    appendComparisonPickParts(parts, comparison, settings, result, false);
    REQUIRE(parts.size() == 2); CHECK(parts[1].diagnosticEdges.size() == 1);
    CHECK(parts[1].diagnosticEdges.data() == result.original.topologyWinding.data());
    std::string report;
    for (const auto& line : comparisonReportLines("Test", "source.obj", "", settings, result, {})) { report += line + "\n"; }
    CHECK(report.find("Red edges: winding conflicts") != std::string::npos);
    CHECK(report.find("Pink edges:") == std::string::npos);
    const auto stages = requestedComparisonStages(settings, false);
    settings.showWinding = false; settings.showNonManifold = true;
    CHECK(requestedComparisonStages(settings, false) == stages);
    parts.clear(); appendComparisonPickParts(parts, comparison, settings, result, false);
    CHECK(parts.size() == 1);
}
