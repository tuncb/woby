#include "mesh_duplicates.h"
#include "mesh_comparison.h"
#include "comparison_scene.h"
#include "comparison_report.h"
#include "control_scene.h"
#include "obj_mesh.h"
#include "stl_mesh.h"
#include "importer_host.h"
#include "scene_history.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

namespace {
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-duplicates-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path write(const char* name, const char* contents) const {
        const auto path = root / name;
        std::ofstream out(path); out << contents;
        return path;
    }
};
woby::DuplicateInput inputFor(std::shared_ptr<const woby::SourceMeshData> data)
{
    woby::SourcePartInstance part;
    part.partId = 2;
    part.indexCount = data->indices.size();
    part.transform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    woby::DuplicateInput input;
    input.sources.push_back({1, "mesh.obj", std::move(data), true, {part}});
    return input;
}
std::shared_ptr<woby::SourceMeshData> triangle()
{
    auto data = std::make_shared<woby::SourceMeshData>();
    data->provenance = woby::SourceProvenance::objPositions;
    data->points = {{0,0,0}, {1,0,0}, {0,1,0}};
    data->indices = {0,1,2};
    return data;
}
woby::UiState scene(const std::filesystem::path& path)
{
    woby::UiState state;
    state.files.push_back(woby::createUiFileState(path, woby::loadObjMesh(path), 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    const auto id = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    return state;
}
}

TEST_CASE("source duplicate points use exact coordinates signed zero and source scopes")
{
    auto data = triangle();
    data->points.insert(data->points.end(), {{-0.0,0,0}, {0,0,0}, {1e-15,0,0}});
    auto input = inputFor(data);
    auto result = woby::inspectDuplicates(input);
    CHECK(result.points.duplicateCount == 2);
    REQUIRE(result.points.findings.size() == 1);
    CHECK(result.points.findings[0].members.size() == 3);
    CHECK(result.points.findings[0].members[0].id == 0);
    CHECK(result.points.findings[0].members[2].id == 4);
    CHECK(result.points.findings[0].geometry.size() == 1);
    input.sources[0].wholeFile = false;
    CHECK(woby::inspectDuplicates(input).points.duplicateCount == 0);
    input.sources[0].wholeFile = true;
    auto second = input.sources[0]; second.fileId = 10;
    input.sources.push_back(second);
    result = woby::inspectDuplicates(input);
    CHECK(result.points.duplicateCount == 4);
    REQUIRE(result.points.findings.size() == 2);
    CHECK(result.points.findings[0].fileId != result.points.findings[1].fileId);
}

TEST_CASE("source duplicate triangles include all permutations and repeated collapsed faces")
{
    auto data = triangle();
    std::array<uint32_t, 3> ids = {0,1,2};
    data->indices.clear();
    do { data->indices.insert(data->indices.end(), ids.begin(), ids.end()); } while (std::next_permutation(ids.begin(), ids.end()));
    data->points.insert(data->points.end(), {{0,0,0}, {1,0,0}, {0,1,0}});
    data->indices.insert(data->indices.end(), {3,4,5, 0,0,1, 1,0,0});
    const auto result = woby::inspectDuplicates(inputFor(data));
    CHECK(result.triangles.duplicateCount == 6);
    REQUIRE(result.triangles.findings.size() == 2);
    const auto& permutations = result.triangles.findings[0].members;
    REQUIRE(permutations.size() == 6);
    CHECK(std::count_if(permutations.begin(), permutations.end(), [](const auto& member) { return member.reversed; }) == 3);
    CHECK(result.triangles.findings[1].members.size() == 2);
    CHECK(result.triangles.findings[1].members[0].id == 7);
    CHECK(result.triangles.findings[0].geometry.size() == 3);
}

TEST_CASE("source duplicate status distinguishes STL unsupported disabled and invalid inputs")
{
    auto data = triangle();
    data->provenance = woby::SourceProvenance::stlCorners;
    data->points.push_back({0,0,0});
    auto input = inputFor(data);
    auto result = woby::inspectDuplicates(input);
    CHECK(result.points.duplicateCount == 0);
    CHECK(result.points.informationalCount == 1);
    CHECK(std::string(woby::duplicateStatus(result.triangles)) == "unavailable");
    auto other = inputFor(triangle()).sources[0]; other.fileId = 2;
    input.sources.push_back(other);
    CHECK(std::string(woby::duplicateStatus(woby::inspectDuplicates(input).triangles)) == "partial");
    input.settings.triangles = false;
    CHECK(std::string(woby::duplicateStatus(woby::inspectDuplicates(input).triangles)) == "disabled");
    data->points[0][0] = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS((void)woby::inspectDuplicates(input));
    data->points[0][0] = 0;
    data->indices[0] = 900;
    CHECK_THROWS((void)woby::inspectDuplicates(input));
    data->indices[0] = 0;
    input.sources[0].parts[0].indexCount = 4;
    CHECK_THROWS((void)woby::inspectDuplicates(input));
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS((void)woby::inspectDuplicates(inputFor(triangle()), stop.get_token()));
}

TEST_CASE("source duplicates retain each part transform without multiplying source counts")
{
    auto data = triangle();
    data->indices.insert(data->indices.end(), {0,1,2});
    auto input = inputFor(data);
    input.sources[0].parts[0].indexCount = 3;
    auto second = input.sources[0].parts[0];
    second.partId = 3; second.firstIndex = 3; second.transform[12] = 10;
    input.sources[0].parts.push_back(second);
    auto result = woby::inspectDuplicates(input);
    CHECK(result.triangles.duplicateCount == 1);
    CHECK(result.points.duplicateCount == 0);
    REQUIRE(result.triangles.findings.size() == 1);
    CHECK(result.triangles.findings[0].geometry.size() == 6);
    CHECK(result.triangles.findings[0].geometry.back()[0] == 11);
    input.sources[0].parts.pop_back();
    CHECK(woby::inspectDuplicates(input).triangles.duplicateCount == 0);
}

TEST_CASE("OBJ source duplicates survive seams compaction and include unused records")
{
    Fixture fixture;
    const auto path = fixture.write("seams.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nvt .5 .5\nf 1/1 2/2 3/3\nf 1/4 3/3 2/2\n");
    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.sourceData);
    CHECK(mesh.sourceData->points.size() == 4);
    CHECK(mesh.sourceData->indices == std::vector<uint32_t>{0,1,2,0,2,1});
    const auto result = woby::inspectDuplicates(inputFor(mesh.sourceData));
    CHECK(result.points.duplicateCount == 1);
    CHECK(result.triangles.duplicateCount == 1);
    CHECK(result.triangles.findings[0].members[1].reversed);
    const auto polygon = woby::loadObjMesh(fixture.write("quad.obj", "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n"));
    CHECK(polygon.sourceData->indices.size() == 6);
    CHECK(woby::inspectDuplicates(inputFor(polygon.sourceData)).triangles.duplicateCount == 0);
}

TEST_CASE("STL source corner records survive compaction")
{
    Fixture fixture;
    const auto mesh = woby::loadStlMesh(fixture.write("sample.stl", "solid test\n"
        "facet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\n"
        "facet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\nendsolid test\n"));
    REQUIRE(mesh.sourceData);
    CHECK(mesh.vertices.size() == 3);
    CHECK(mesh.sourceData->points.size() == 6);
    const auto result = woby::inspectDuplicates(inputFor(mesh.sourceData));
    CHECK(result.points.informationalCount == 3);
    CHECK(std::string(woby::duplicateStatus(result.triangles)) == "unavailable");
}

TEST_CASE("importer duplicate records are owned before buffers are released")
{
    std::vector<WobyImportVertex> vertices(4);
    vertices[1].position[0] = 1;
    vertices[2].position[1] = 1;
    std::vector<uint32_t> indices = {0,1,2, 2,1,0};
    WobyImportResult imported{};
    imported.struct_size = sizeof(imported);
    imported.vertex_count = static_cast<uint32_t>(vertices.size());
    imported.index_count = static_cast<uint32_t>(indices.size());
    imported.vertices = vertices.data(); imported.indices = indices.data();
    const auto mesh = woby::copyImportedMesh(imported);
    vertices.clear(); indices.clear();
    REQUIRE(mesh.sourceData);
    CHECK(mesh.sourceData->provenance == woby::SourceProvenance::importerVertices);
    CHECK(mesh.sourceData->points.size() == 4);
    const auto result = woby::inspectDuplicates(inputFor(mesh.sourceData));
    CHECK(result.points.duplicateCount == 1);
    CHECK(result.triangles.duplicateCount == 1);
}

TEST_CASE("duplicate analysis navigation persistence signatures and CLI agree")
{
    Fixture fixture;
    auto state = scene(fixture.write("parts.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\n"
        "o first\nf 1 2 3\no second\nf 1 3 2\n"));
    const auto id = state.activeComparisonId;
    state.files[0].groupSettings[1].translation = {10,0,0};
    woby::setComparisonTranslation(state, id, {100,0,0});
    const auto mesh = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    const auto result = woby::compareMeshes(mesh, {});
    REQUIRE(result.original.duplicates.triangles.findings.size() == 1);
    CHECK(result.original.duplicates.points.duplicateCount == 1);
    CHECK(result.original.duplicateTriangleBounds[0].b[0] == 11);
    const auto signature = woby::comparisonGeometrySignature(state, id);
    auto settings = woby::comparisonSettings(state, id);
    settings.diagnosticCategory = woby::DiagnosticCategory::duplicateTriangles;
    settings.duplicates.showPoints = false;
    settings.duplicates.showTriangles = false;
    woby::setComparisonSettings(state, settings, id);
    CHECK(woby::comparisonGeometrySignature(state, id) == signature);
    woby::selectComparisonDiagnostic(state, result, signature, 0, id);
    REQUIRE(woby::findComparison(state, id)->diagnosticFocus);
    CHECK(state.camera.target[0] > 100);
    woby::navigateComparisonDiagnostic(state, result, signature, 1, id);
    CHECK(woby::findComparison(state, id)->diagnosticFocus->index == 0);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    CHECK_FALSE(history.snapshots.front().content.comparisons.front().diagnosticFocus);
    const auto document = woby::createSceneDocument(state);
    const auto scenePath = fixture.root / "saved.woby";
    woby::writeSceneDocument(scenePath, document);
    CHECK(woby::readSceneDocument(scenePath).comparisons[0].settings == settings);
    const auto json = woby::controlComparisonResults(result, .05);
    CHECK(json["aToB"]["detectors"]["duplicate_points"]["count"] == 1);
    CHECK(json["aToB"]["detectors"]["duplicate_tris"]["count"] == 1);
    CHECK(json["aToB"]["diagnostics"]["duplicateTriangles"] == 0); // Separated world geometry.
    const auto operation = woby::parseControlOperation(*woby::findControlMethod("analysis.set"),
        {{"target", "analysis"}, {"duplicatePoints", false}, {"showDuplicateTriangles", true}});
    CHECK(operation.duplicatePoints == false);
    CHECK(operation.showDuplicateTriangles == true);
    auto command = operation; command.objectId = id;
    (void)woby::applyControlSceneOperation(state, document, command, [](auto value) { return std::to_string(value); }, 200, 800);
    CHECK_FALSE(woby::comparisonSettings(state, id).duplicates.points);
    CHECK(woby::comparisonGeometrySignature(state, id) != signature);
    CHECK_FALSE(woby::findComparison(state, id)->diagnosticFocus);
    const auto disabled = woby::compareMeshes(woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id), {});
    CHECK(std::string(woby::duplicateStatus(disabled.original.duplicates.points)) == "disabled");
    const auto disabledJson = woby::controlComparisonResults(disabled, .05);
    CHECK(disabledJson["aToB"]["detectors"]["duplicate_points"]["count"].is_null());
}

TEST_CASE("duplicate settings migrate old scenes and JSON findings are bounded")
{
    Fixture fixture;
    for (int version = 2; version <= 7; ++version) {
        const auto path = fixture.root / "old.woby";
        { std::ofstream out(path); out << "version = " << version << "\n[[analyses]]\nname = \"Old\"\n"; }
        const auto old = woby::readSceneDocument(path);
        REQUIRE(old.comparisons.size() == 1);
        CHECK(old.comparisons[0].settings.duplicates == woby::DuplicateSettings{});
    }
    auto data = triangle();
    for (size_t i = 0; i < 120; ++i) { data->indices.insert(data->indices.end(), {0,1,2}); }
    woby::MeshComparison result;
    result.original.source.indices = {0,1,2};
    result.original.duplicates = woby::inspectDuplicates(inputFor(data));
    const auto json = woby::controlComparisonResults(result, .05)["aToB"]["detectors"]["duplicate_tris"];
    CHECK(json["count"] == 120);
    CHECK(json["findings"][0]["members"].size() == 100);
    CHECK(json["findings"][0]["membersTruncated"] == true);
}

TEST_CASE("unused duplicate point highlights follow the file rather than a moved part")
{
    auto data = triangle();
    data->points.push_back({0,0,0});
    auto input = inputFor(data);
    input.sources[0].unusedPointTransform[12] = 3;
    input.sources[0].parts[0].transform[12] = 10;
    const auto result = woby::inspectDuplicates(input);
    REQUIRE(result.points.findings.size() == 1);
    CHECK(result.points.findings[0].geometry == std::vector<std::array<float, 3>>{{3,0,0}, {10,0,0}});
}

TEST_CASE("unified diagnostic rows navigate duplicate groups on either side and reset between categories")
{
    Fixture fixture;
    auto state = scene(fixture.write("navigation.obj",
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 3 0 0\nv 4 0 0\nv 3 1 0\n"
        "v 0 0 0\nv 1 0 0\nv 0 0 0\n"
        "f 1 2 3\nf 2 3 1\nf 3 2 1\nf 4 5 6\nf 6 5 4\n"));
    const auto id = state.activeComparisonId;
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::b, true, id);
    const auto world = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    const auto result = woby::compareMeshes(world, world);
    const auto signature = woby::comparisonGeometrySignature(state, id);
    for (const auto side : {woby::ComparisonSide::a, woby::ComparisonSide::b}) {
        for (const auto category : {woby::DiagnosticCategory::nonManifold, woby::DiagnosticCategory::duplicatePoints,
                woby::DiagnosticCategory::duplicateTriangles, woby::DiagnosticCategory::winding}) {
            auto settings = woby::comparisonSettings(state, id);
            settings.diagnosticSide = side;
            settings.diagnosticCategory = category;
            settings.mode = woby::ComparisonMode::overlay;
            woby::setComparisonSettings(state, settings, id);
            CHECK_FALSE(woby::findComparison(state, id)->diagnosticFocus);
            const auto& findings = woby::comparisonDiagnosticEdges(result, side, category);
            if (category == woby::DiagnosticCategory::duplicatePoints || category == woby::DiagnosticCategory::duplicateTriangles) {
                CHECK(woby::comparisonDuplicates(result, side, category).duplicateCount == 3);
                CHECK(findings.size() == 2); // Navigate groups, not the count of extra records.
            }
            if (findings.empty()) {
                woby::navigateComparisonDiagnostic(state, result, signature, 1, id);
                CHECK_FALSE(woby::findComparison(state, id)->diagnosticFocus);
                continue;
            }
            // Left starts at the last group, and right wraps to the first.
            woby::navigateComparisonDiagnostic(state, result, signature, -1, id);
            REQUIRE(woby::findComparison(state, id)->diagnosticFocus);
            CHECK(woby::findComparison(state, id)->diagnosticFocus->index == findings.size()-1);
            for (size_t index = 0; index < findings.size(); ++index) {
                woby::navigateComparisonDiagnostic(state, result, signature, 1, id);
                const auto& focus = *woby::findComparison(state, id)->diagnosticFocus;
                CHECK(focus.index == index);
                CHECK(focus.side == side);
                CHECK(focus.category == category);
                CHECK(woby::focusedComparisonDiagnostic(state, result, signature, id) == &findings[index]);
            }
            woby::frameComparison(state, id);
            CHECK_FALSE(woby::findComparison(state, id)->diagnosticFocus);
            CHECK(woby::effectiveComparisonSettings(state, id).mode == woby::ComparisonMode::overlay);
        }
    }
}
