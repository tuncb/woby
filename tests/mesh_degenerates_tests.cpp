#include "mesh_degenerates.h"
#include "mesh_comparison.h"
#include "comparison_scene.h"
#include "comparison_view.h"
#include "comparison_report.h"
#include "control_scene.h"
#include "scene_history.h"
#include "obj_mesh.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

namespace {
using namespace woby;
using Points = std::array<std::array<double, 3>, 3>;
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-degenerates-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path write(const char* name, const std::string& text) const {
        const auto path = root/name; std::ofstream out(path); out << text; return path;
    }
};
DuplicateSource sourceFor(const Points& points)
{
    auto data = std::make_shared<SourceMeshData>();
    data->points.assign(points.begin(), points.end()); data->indices = {0,1,2};
    data->provenance = SourceProvenance::objPositions;
    SourcePartInstance part;
    part.partId = 2; part.indexCount = 3;
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
    mesh.indices = source.data->indices;
    mesh.bounds = calculateBounds(mesh.vertices);
    auto input = std::make_shared<DuplicateInput>(); input->sources.push_back(source); mesh.duplicateInput = input;
    return mesh;
}
}

TEST_CASE("degenerate triangles distinguish collapse needles and caps with strict thresholds")
{
    const Points right = {{{0,0,0}, {4,0,0}, {0,3,0}}};
    DegenerateSettings settings;
    settings.needleThresholdRatio = 2;
    settings.capMinAngleDegrees = 90;
    const auto clean = classifyDegenerateTriangle(right, settings);
    CHECK_FALSE(clean.collapsed); CHECK_FALSE(clean.needle); CHECK_FALSE(clean.cap);
    CHECK(clean.maximumAngleDegrees == doctest::Approx(90));
    CHECK(clean.edgeRatio == doctest::Approx(5.0/3));

    const Points ratioTwo = {{{0,0,0}, {2,0,0}, {1,0,0}}};
    auto boundary = classifyDegenerateTriangle(ratioTwo, settings);
    CHECK(boundary.collapsed); CHECK_FALSE(boundary.needle); CHECK(boundary.cap);
    settings.needleThresholdRatio = std::nextafter(2.0f, 1.0f);
    CHECK(classifyDegenerateTriangle(ratioTwo, settings).needle);
    settings.needleThresholdRatio = std::nextafter(2.0f, 3.0f);
    CHECK_FALSE(classifyDegenerateTriangle(ratioTwo, settings).needle);
    settings.capMinAngleDegrees = 180;
    CHECK_FALSE(classifyDegenerateTriangle(ratioTwo, settings).cap);
    settings.capMinAngleDegrees = std::nextafter(180.0f, 0.0f);
    CHECK(classifyDegenerateTriangle(ratioTwo, settings).cap);
    const Points angle135 = {{{0,0,0}, {1,0,0}, {-1,1,0}}};
    settings.capMinAngleDegrees = 135;
    CHECK_FALSE(classifyDegenerateTriangle(angle135, settings).cap);
    settings.capMinAngleDegrees = std::nextafter(135.0f, 0.0f);
    CHECK(classifyDegenerateTriangle(angle135, settings).cap);
    settings.capMinAngleDegrees = std::nextafter(135.0f, 180.0f);
    CHECK_FALSE(classifyDegenerateTriangle(angle135, settings).cap);

    const Points zeroEdge = {{{0,0,0}, {0,0,0}, {1,0,0}}};
    const auto zero = classifyDegenerateTriangle(zeroEdge, {});
    CHECK(zero.collapsed); CHECK_FALSE(zero.needle); CHECK_FALSE(zero.cap);
    CHECK(std::isnan(zero.edgeRatio)); CHECK(std::isnan(zero.maximumAngleDegrees));
    CHECK(classifyDegenerateTriangle(Points{}, {}).collapsed);

    const Points needle = {{{0,0,0}, {1,0,0}, {0,.0001,0}}};
    const auto n = classifyDegenerateTriangle(needle, {});
    CHECK(n.needle); CHECK_FALSE(n.cap); CHECK_FALSE(n.collapsed);
    const Points cap = {{{-1,0,0}, {1,0,0}, {0,.01,0}}};
    const auto c = classifyDegenerateTriangle(cap, {});
    CHECK(c.cap); CHECK_FALSE(c.needle); CHECK_FALSE(c.collapsed);
    settings = {}; settings.capMinAngleDegrees = 179.5f;
    CHECK_FALSE(classifyDegenerateTriangle(cap, settings).cap);
    settings.needleThresholdRatio = 1;
    const auto unionResult = inspectDegenerates({sourceFor(cap)}, settings);
    REQUIRE(unionResult.findings.size() == 1);
    CHECK(unionResult.needleCount == 1); CHECK(unionResult.capCount == 0);
    settings.capMinAngleDegrees = 177.5f;
    const auto overlap = inspectDegenerates({sourceFor(cap)}, settings);
    CHECK(overlap.findings.size() == 1); CHECK(overlap.needleCount == 1); CHECK(overlap.capCount == 1);
}

TEST_CASE("degenerate classification is invariant to winding translation and uniform scale")
{
    const Points original = {{{-1,0,0}, {1,0,0}, {0,.01,0}}};
    for (const double scale : {1e-150, 1e-15, 1.0, 1e15, 1e150}) {
        auto points = original;
        for (auto& p : points) { for (auto& value : p) { value = (value+4)*scale; } }
        for (int flip = 0; flip < 2; ++flip) {
            const auto result = classifyDegenerateTriangle(points, {});
            CHECK(result.cap); CHECK_FALSE(result.needle); CHECK_FALSE(result.collapsed);
            CHECK(result.edgeRatio == doctest::Approx(classifyDegenerateTriangle(original, {}).edgeRatio));
            std::swap(points[0], points[2]);
        }
    }
}

TEST_CASE("degenerate source findings preserve instances scope ordering and STL support")
{
    auto source = sourceFor({{{0,0,0}, {1,0,0}, {0,1,0}}});
    auto thin = source.parts[0]; thin.partId = 8; thin.transform[5] = .0001f; thin.transform[12] = 10;
    source.parts.push_back(thin); source.parts.push_back(thin); // Overlapping selection is counted once.
    const auto result = inspectDegenerates({source}, {});
    REQUIRE(result.findings.size() == 1);
    CHECK(result.findings[0].partId == 8); CHECK(result.findings[0].triangleId == 0);
    CHECK(result.findings[0].geometry[0][0] == 10);
    CHECK(result.needleCount == 1);
    source.parts.erase(source.parts.begin()+1, source.parts.end());
    CHECK(inspectDegenerates({source}, {}).findings.empty());

    source = sourceFor({{{0,0,0}, {1,0,0}, {2,0,0}}});
    auto data = std::make_shared<SourceMeshData>(*source.data);
    data->indices.insert(data->indices.end(), {0,2,1});
    data->provenance = SourceProvenance::stlCorners; source.data = data;
    source.parts[0].indexCount = 6;
    auto other = source; other.fileId = 20;
    const auto all = inspectDegenerates({other, source}, {});
    REQUIRE(all.findings.size() == 4);
    CHECK(all.findings[0].fileId == 1); CHECK(all.findings[1].triangleId == 1);
    CHECK(all.findings[2].fileId == 20); CHECK(all.collapsedCount == 4);
    CHECK(std::string(degenerateStatus(all)) == "complete");
}

TEST_CASE("degenerate validation disabled unavailable and cancellation are explicit")
{
    auto source = sourceFor({{{0,0,0}, {1,0,0}, {0,1,0}}});
    auto invalid = std::make_shared<SourceMeshData>(*source.data); source.data = invalid;
    invalid->indices[0] = 99;
    CHECK_THROWS((void)inspectDegenerates({source}, {}));
    DegenerateSettings off; off.enabled = false;
    CHECK(std::string(degenerateStatus(inspectDegenerates({source}, off))) == "disabled");
    invalid->indices[0] = 0; invalid->indices.push_back(1);
    CHECK_THROWS((void)inspectDegenerates({source}, {}));
    invalid->indices.pop_back(); invalid->points[0][0] = std::numeric_limits<double>::infinity();
    CHECK_THROWS((void)inspectDegenerates({source}, {}));
    invalid->points[0][0] = 0; source.parts[0].firstIndex = 1;
    CHECK_THROWS((void)inspectDegenerates({source}, {}));
    source.parts[0].firstIndex = 0; source.parts[0].transform[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_THROWS((void)inspectDegenerates({source}, {}));
    source.data.reset();
    CHECK(std::string(degenerateStatus(inspectDegenerates({source}, {}))) == "unavailable");
    auto valid = sourceFor({{{0,0,0}, {1,0,0}, {0,1,0}}});
    CHECK(std::string(degenerateStatus(inspectDegenerates({source, valid}, {}))) == "partial");
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS((void)inspectDegenerates({valid}, {}, stop.get_token()));
    auto settings = normalizedDegenerateSettings({true, true, -1, 900});
    CHECK(settings.needleThresholdRatio == 1); CHECK(settings.capMinAngleDegrees == 180);
    settings = normalizedDegenerateSettings({true, true, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()});
    CHECK(settings == DegenerateSettings{});
}

TEST_CASE("degenerate thresholds invalidate only their stage and reject stale findings")
{
    const auto source = meshFor(sourceFor({{{-1,0,0}, {1,0,0}, {0,.01,0}}}));
    ComparisonSettings settings;
    MeshComparison result;
    ComparisonCacheStatus cache{42, 0};
    const auto stages = requestedComparisonStages(settings, true, true);
    REQUIRE(applyComparisonStages(result, cache, computeComparisonStages(source, source, stages), 42, stages));
    REQUIRE(result.original.degenerates.findings.size() == 1);
    const auto* distance = result.original.distances.data();
    const auto* topology = result.original.diagnostics.boundaryEdges.data();
    const auto* quality = result.original.quality.triangles.data();
    auto stale = computeComparisonStages(source, {}, comparisonDegenerates);
    settings.degenerates.capMinAngleDegrees = 179.5f;
    REQUIRE(resetComparisonDegenerateCache(cache, settings.degenerates));
    CHECK((stages & ~cache.completed) == comparisonDegenerates);
    CHECK_FALSE(applyComparisonStages(result, cache, std::move(stale), 42, comparisonDegenerates));
    auto mixed = computeComparisonStages(source, source, comparisonDegenerates | comparisonDistance);
    const auto* freshDistance = mixed.original.distances.data();
    REQUIRE(applyComparisonStages(result, cache, std::move(mixed), 42, comparisonDegenerates | comparisonDistance));
    CHECK(result.original.distances.data() == freshDistance); // Preserve successful independent work.
    distance = freshDistance;
    CHECK((stages & ~cache.completed) == comparisonDegenerates);
    REQUIRE(applyComparisonStages(result, cache, computeComparisonStages(source, source, comparisonDegenerates, {}, settings.degenerates), 42, comparisonDegenerates));
    CHECK(result.original.degenerates.findings.empty());
    CHECK(result.original.distances.data() == distance);
    CHECK(result.original.diagnostics.boundaryEdges.data() == topology);
    CHECK(result.original.quality.triangles.data() == quality);
    settings.degenerates.show = false;
    CHECK_FALSE(resetComparisonDegenerateCache(cache, settings.degenerates));
    settings.degenerates.enabled = false;
    CHECK_FALSE(resetComparisonDegenerateCache(cache, settings.degenerates));
    setComparisonDegenerateSettings(result, DegenerateSettings{false});
    CHECK(controlComparisonResults(result, .05)["aToB"]["detectors"]["degenerate_tris"]["count"].is_null());
    settings.degenerates.enabled = true;
    CHECK_FALSE(resetComparisonDegenerateCache(cache, settings.degenerates));
    CHECK((requestedComparisonStages(settings, true, true) & ~cache.completed) == 0);
}

TEST_CASE("degenerate scene operations navigation persistence and report agree")
{
    Fixture fixture;
    const auto path = fixture.write("shape.obj", "o cap\nv -1 0 0\nv 1 0 0\nv 0 .01 0\nf 1 2 3\n");
    UiState state;
    state.files.push_back(createUiFileState(path, loadObjMesh(path), 0));
    appendDefaultSceneNodesForFiles(state, 0);
    const auto id = createComparison(state);
    setComparisonObjects(state, {state.files[0].objectId}, ComparisonSide::a, true, id);
    setComparisonTranslation(state, id, {100,0,0});
    auto settings = comparisonSettings(state, id);
    settings.diagnosticCategory = DiagnosticCategory::degenerateTriangles;
    setComparisonSettings(state, settings, id);
    const auto input = comparisonWorldMesh(state, ComparisonSide::a, id);
    const auto result = compareMeshes(input, {});
    const auto signature = comparisonGeometrySignature(state, id);
    REQUIRE(result.original.degenerates.findings.size() == 1);
    CHECK(result.original.degenerates.findings[0].geometry[0][0] == -1); // No display offset.
    CHECK(result.original.diagnostics.degenerateTriangles == 0); // Legacy collapse semantics.
    selectComparisonDiagnostic(state, result, signature, 0, id);
    REQUIRE(findComparison(state, id)->diagnosticFocus);
    CHECK(state.camera.target[0] == doctest::Approx(100));
    navigateComparisonDiagnostic(state, result, signature, -1, id);
    CHECK(findComparison(state, id)->diagnosticFocus->index == 0);
    settings.degenerates.show = false;
    setComparisonSettings(state, settings, id);
    CHECK(findComparison(state, id)->diagnosticFocus.has_value());
    const auto clean = createSceneDocument(state);
    auto command = parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"},
        {"needleThresholdRatio", 2000}, {"capMinAngleDegrees", 179.5}, {"showDegenerateTriangles", true}});
    command.objectId = id;
    (void)applyControlSceneOperation(state, clean, command, [](auto value) { return std::to_string(value); }, 200, 800);
    CHECK(state.isDirty);
    CHECK(comparisonGeometrySignature(state, id) == signature);
    CHECK_FALSE(findComparison(state, id)->diagnosticFocus);
    selectComparisonDiagnostic(state, result, signature, 0, id);
    CHECK_FALSE(findComparison(state, id)->diagnosticFocus); // Old threshold results cannot be selected.
    ComparisonRuntime runtime;
    runtime.ready = true; runtime.resultSignature = signature;
    runtime.cache = {signature, requestedComparisonStages(settings, false)};
    CHECK_FALSE(comparisonResultsReady(runtime, state, id));
    const auto document = createSceneDocument(state);
    const auto saved = fixture.root/"saved.woby";
    writeSceneDocument(saved, document);
    CHECK(readSceneDocument(saved).comparisons[0].settings == comparisonSettings(state, id));
    std::ifstream file(saved); const std::string text((std::istreambuf_iterator<char>(file)), {});
    CHECK(text.find("version = 10") != std::string::npos);
    CHECK(text.find("findings") == std::string::npos);
    CHECK(text.find("diagnostic_focus") == std::string::npos);
    const auto json = controlComparisonResults(result, .05)["aToB"]["detectors"]["degenerate_tris"];
    CHECK(json["count"] == 1); CHECK(json["findings"][0]["triangleId"] == 1);
    CHECK(json["findings"][0]["reasons"]["cap"] == true);
    std::string report;
    for (const auto& line : comparisonReportLines("Test", "shape.obj", "", settings, result, {})) { report += line + "\n"; }
    CHECK(report.find("A degenerate triangles: 1 (complete)") != std::string::npos);
    CHECK(report.find("177.5") != std::string::npos);
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"}, {"degenerateTriangles", "true"}}));
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"}, {"needleThresholdRatio", std::numeric_limits<double>::infinity()}}));
    auto normalizedCommand = parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"},
        {"needleThresholdRatio", -10}, {"capMinAngleDegrees", 200}, {"degenerateTriangles", false}});
    CHECK(controlOperationParams(normalizedCommand)["needleThresholdRatio"] == -10);
    normalizedCommand.objectId = id;
    (void)applyControlSceneOperation(state, document, normalizedCommand, [](auto value) { return std::to_string(value); }, 200, 800);
    CHECK(comparisonSettings(state, id).degenerates.needleThresholdRatio == 1);
    CHECK(comparisonSettings(state, id).degenerates.capMinAngleDegrees == 180);
    CHECK_FALSE(comparisonSettings(state, id).degenerates.enabled);
}

TEST_CASE("degenerate scene migration and bounded JSON retain exact totals")
{
    Fixture fixture;
    for (int version = 2; version <= 8; ++version) {
        const auto path = fixture.write("old.woby", "version = " + std::to_string(version) + "\n[[analyses]]\nname = \"Old\"\n");
        CHECK(readSceneDocument(path).comparisons[0].settings.degenerates == DegenerateSettings{});
    }
    const auto invalid = fixture.write("normalized.woby", "version = 9\n[[analyses]]\nneedle_threshold_ratio = -4\ncap_min_angle_degrees = 12\n");
    const auto settings = readSceneDocument(invalid).comparisons[0].settings.degenerates;
    CHECK(settings.needleThresholdRatio == 1); CHECK(settings.capMinAngleDegrees == 90);
    auto source = sourceFor(Points{});
    auto data = std::make_shared<SourceMeshData>(*source.data);
    for (size_t i = 1; i < 120; ++i) { data->indices.insert(data->indices.end(), {0,1,2}); }
    source.data = data; source.parts[0].indexCount = data->indices.size();
    MeshComparison result;
    result.original.source.indices = {0,1,2};
    result.original.degenerates = inspectDegenerates({source}, {});
    auto json = controlComparisonResults(result, .05)["aToB"]["detectors"]["degenerate_tris"];
    CHECK(json["count"] == 120); CHECK(json["findings"].size() == 100);
    CHECK(json["findingsTruncated"] == true);
    CHECK(json["findings"][0]["edgeRatio"].is_null());
    result.original.degenerates.unavailableSources = 1;
    json = controlComparisonResults(result, .05)["aToB"]["detectors"]["degenerate_tris"];
    CHECK(json["count"].is_null()); CHECK(json["knownCount"] == 120); CHECK(json["status"] == "partial");
    setComparisonDegenerateSettings(result, DegenerateSettings{false});
    json = controlComparisonResults(result, .05)["aToB"]["detectors"]["degenerate_tris"];
    CHECK(json["count"].is_null()); CHECK(json["findings"].empty()); CHECK(json["reasonCounts"]["collapsed"] == 0);
}
