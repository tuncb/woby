#include "background_load.h"
#include "analysis_results.h"
#include "comparison_scene.h"
#include "control_scene.h"
#include <nlohmann/json.hpp>
#include "importer_host.h"
#include "obj_mesh.h"
#include "scene_dimensions.h"
#include "scene_history.h"
#include "surface_annotation.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>

namespace {
struct CoordinateFixture {
    std::filesystem::path root;
    CoordinateFixture()
    {
        const auto prefix = "woby_coordinates_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0;; ++i) {
            root = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~CoordinateFixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
    std::filesystem::path obj(const char* name, double offset, double z = 0)
    {
        const auto path = root / name;
        std::ofstream out(path);
        out << std::setprecision(17);
        for (const woby::Coordinate p : {woby::Coordinate{0,0,z}, {0.125,0,z}, {0,0.125,z}, {0,0,z}, {0.01,0,z}}) {
            out << "v " << offset+p[0] << ' ' << -offset+p[1] << ' ' << offset+p[2] << '\n';
        }
        out << "f 1 2 3\n";
        return path;
    }
};
woby::UiState coordinateScene(const std::filesystem::path& a, const std::filesystem::path& b)
{
    woby::UiState state;
    for (const auto& path : {a,b}) { state.files.push_back(woby::createUiFileState(path, woby::loadObjMesh(path), 0)); }
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::recalculateSceneBounds(state);
    const auto comparison = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, comparison);
    woby::setComparisonObjects(state, {state.files[1].objectId}, woby::ComparisonSide::b, true, comparison);
    return state;
}
}

TEST_CASE("coordinate origin preserves small OBJ features before float conversion")
{
    CoordinateFixture fixture;
    const auto mesh = woby::loadObjMesh(fixture.obj("far.obj", 8000000));
    CHECK(mesh.origin[0] != 0);
    CHECK(mesh.bounds.radius < 1);
    REQUIRE(mesh.sourceData->points.size() == 5);
    CHECK(mesh.sourceData->points[4][0] - mesh.sourceData->points[0][0] == doctest::Approx(.01).epsilon(1e-7));
    CHECK(mesh.vertices[0].position != mesh.vertices[1].position);
    const auto bounds = woby::originalMeshBounds(mesh);
    CHECK(bounds[0][0] == 8000000);
    CHECK(bounds[1][0] == 8000000.125);
    CHECK(mesh.precisePositions.size() == mesh.vertices.size());
}

TEST_CASE("coordinate origin makes detectors and measurements translation invariant")
{
    CoordinateFixture fixture;
    auto near = coordinateScene(fixture.obj("a.obj",0),fixture.obj("b.obj",0,.0625));
    auto far = coordinateScene(fixture.obj("far_a.obj",8000000),fixture.obj("far_b.obj",8000000,.0625));
    const auto inspect = [](const woby::UiState& state) {
        return woby::computeComparisonStages(woby::comparisonWorldMesh(state,woby::ComparisonSide::a),
            woby::comparisonWorldMesh(state,woby::ComparisonSide::b), woby::comparisonSource | woby::comparisonDetectors | woby::comparisonDistance | woby::comparisonQuality | woby::comparisonIntersections);
    };
    const auto a = inspect(near), b = inspect(far);
    CHECK(a.original.mean == doctest::Approx(.0625));
    CHECK(b.original.mean == doctest::Approx(a.original.mean).epsilon(1e-10));
    CHECK(b.original.duplicates.points.duplicateCount == 1);
    CHECK(b.original.duplicates.points.duplicateCount == a.original.duplicates.points.duplicateCount);
    CHECK(b.original.degenerates.collapsedCount == 0);
    const auto report = woby::analysisResultPage(b,woby::ComparisonSide::a,"boundary_edges","/findings/0/endpoints",0,10);
    CHECK(report.dump().find("8000000") != std::string::npos);
    CHECK(b.original.topology.sources.size() == a.original.topology.sources.size());
    REQUIRE(a.original.quality.triangles.size() == 1);
    REQUIRE(b.original.quality.triangles.size() == 1);
    CHECK(b.original.quality.triangles[0].area == doctest::Approx(a.original.quality.triangles[0].area));
    far.selectedSceneObjects = {far.files[0].objectId};
    const auto parts = woby::scenePickParts(far);
    const auto dimensions = woby::sceneDimensions(parts);
    REQUIRE(dimensions);
    CHECK(dimensions->lengths[0] == doctest::Approx(.125));
    CHECK(dimensions->lengths[1] == doctest::Approx(.125));
    const auto origin = far.coordinateOrigin;
    const auto added = fixture.obj("added.obj",8000000,.125);
    far.files.push_back(woby::createUiFileState(added,woby::loadObjMesh(added),0));
    woby::appendDefaultSceneNodesForFiles(far,2);
    CHECK(far.coordinateOrigin == origin);
    CHECK(far.files[2].fileSettings.coordinateOffset[2] == doctest::Approx(.125));
    woby::setFileVisible(far.files[0],false);
    woby::recalculateSceneBounds(far);
    CHECK(far.coordinateOrigin == origin);
}

TEST_CASE("coordinate origin is preserved through scene save load and history")
{
    CoordinateFixture fixture;
    auto state = coordinateScene(fixture.obj("a.obj",8000000),fixture.obj("b.obj",8000000,.0625));
    state.camera.target = {.25f,.5f,.75f};
    const auto path = fixture.root/"scene.woby";
    auto document = woby::createSceneDocument(state);
    woby::writeSceneDocument(path,document);
    auto loaded = woby::loadSceneCpu(path,{},{});
    auto restored = woby::prepareSceneReplacement({},std::move(loaded.files),loaded.document);
    CHECK(restored.coordinateOrigin == state.coordinateOrigin);
    CHECK(restored.camera == state.camera);
    CHECK(restored.files[0].mesh.origin == state.files[0].mesh.origin);
    CHECK(restored.files[1].fileSettings.coordinateOffset == state.files[1].fileSettings.coordinateOffset);
    woby::SceneHistory history;
    woby::resetSceneHistory(history,state);
    state.files[0].fileSettings.translation[0] = 2;
    woby::markSceneDirty(state);
    CHECK(woby::recordSceneHistory(history,state,0));
    CHECK(history.snapshots.back().content.coordinateOrigin == state.coordinateOrigin);
    document.coordinateOrigin = woby::Coordinate{std::numeric_limits<double>::infinity(),0,0};
    CHECK_THROWS(woby::writeSceneDocument(fixture.root/"bad.woby",document));
    std::ofstream(fixture.root/"bad.woby") << "version = 17\ncoordinate_origin = [nan, 0, 0]\n";
    CHECK_THROWS(static_cast<void>(woby::readSceneDocument(fixture.root/"bad.woby")));
    std::ofstream(fixture.root/"bad.woby") << "version = 17\n[[files]]\npath = \"a.obj\"\ncoordinate_origin = [0, inf, 0]\n";
    CHECK_THROWS(static_cast<void>(woby::readSceneDocument(fixture.root/"bad.woby")));
}

TEST_CASE("coordinate origin preserves double importer positions")
{
    WobyImportVertex vertices[] = {{{8000000,8000000,8000000},{},{}},
        {{8000000.01,8000000,8000000},{},{}},{{8000000,8000000.01,8000000},{},{}}};
    uint32_t indices[] = {0,1,2};
    WobyImportResult input{};
    input.struct_size = sizeof(input); input.vertices = vertices; input.vertex_count = 3;
    input.indices = indices; input.index_count = 3;
    const auto imported = woby::copyImportedMesh(input);
    CHECK(imported.bounds.radius < .02);
    CHECK(imported.precisePositions[1][0]-imported.precisePositions[0][0] == doctest::Approx(.01).epsilon(1e-7));
}

TEST_CASE("coordinate origin aligns independently loaded meshes for direct comparisons")
{
    CoordinateFixture fixture;
    const auto a = woby::loadObjMesh(fixture.obj("a.obj",8000000));
    const auto b = woby::loadObjMesh(fixture.obj("b.obj",8000000,.0625));
    CHECK(a.origin != b.origin);
    const auto result = woby::compareMeshes(a,b);
    CHECK(result.original.mean == doctest::Approx(.0625).epsilon(1e-10));
    const auto original = woby::originalPosition(woby::meshPosition(result.repaired.source,0),result.repaired.source.origin);
    CHECK(original[2] == 8000000.0625);
}

TEST_CASE("coordinate origin triangulates small distant concave polygons")
{
    CoordinateFixture fixture;
    const auto path = fixture.root/"polygon.obj";
    std::ofstream(path) << "v 8000000 8000000 0\nv 8000000.02 8000000 0\nv 8000000.02 8000000.02 0\n"
        "v 8000000.01 8000000.01 0\nv 8000000 8000000.02 0\nf 1 2 3 4 5\n";
    const auto mesh = woby::loadObjMesh(path);
    CHECK(mesh.indices.size() == 9);
    const auto quality = woby::inspectSurfaceMeshQuality(mesh);
    CHECK(quality.degenerateTriangles == 0);
    double area = 0;
    for (const auto& face : quality.triangles) { area += face.area; }
    CHECK(area == doctest::Approx(.0003).epsilon(1e-6));
}

TEST_CASE("coordinate origin preserves annotation attachments and original vertex readouts")
{
    CoordinateFixture fixture;
    auto state = coordinateScene(fixture.obj("a.obj",8000000),fixture.obj("b.obj",8000000,.0625));
    auto& mesh = state.files[0].mesh;
    woby::prepareAnnotationMeshCache(mesh);
    woby::AnnotationGeometry geometry;
    woby::coordinateIdentity(geometry.projector.data());
    geometry.fingerprint = woby::annotationFingerprint(mesh,0,3);
    geometry.start = {mesh.vertices[0].position[0],mesh.vertices[0].position[1]};
    geometry.end = {mesh.vertices[1].position[0],mesh.vertices[1].position[1]};
    woby::AnnotationSegment segment;
    segment.a = {1,0,0}; segment.b = {0,1,0}; geometry.segments.push_back(segment);
    const auto id = woby::createAnnotation(state,state.files[0].groupSettings[0].objectId,geometry);
    const auto vertices = woby::annotationOriginalVertices(state,*woby::findAnnotation(state,id));
    REQUIRE(vertices.size() == 2);
    CHECK(vertices[0][0] == 8000000);
    CHECK(vertices[1][0] == 8000000.125);
    auto document = woby::createSceneDocument(state);
    SUBCASE("saved origins") {}
    SUBCASE("legacy projectors migrate after matching the old fingerprint") {
        auto legacy = mesh;
        woby::rebaseMesh(legacy,{});
        auto& old = document.annotations[0].geometry;
        old.fingerprint = woby::annotationFingerprint(legacy,0,3);
        for (size_t row = 0; row < 4; ++row) {
            for (size_t k = 0; k < 3; ++k) { old.projector[12+row] -= old.projector[k*4+row]*mesh.origin[k]; }
        }
        document.coordinateOrigin.reset();
        for (auto& file : document.files) { file.coordinateOrigin.reset(); }
    }
    const auto path = fixture.root/"annotations.woby";
    woby::writeSceneDocument(path,document);
    auto loaded = woby::loadSceneCpu(path,{},{});
    for (auto& file : loaded.files) { woby::prepareAnnotationMeshCache(file.mesh); }
    auto restored = woby::prepareSceneReplacement({},std::move(loaded.files),loaded.document);
    REQUIRE(restored.annotations.size() == 1);
    CHECK(restored.annotations[0].targetValid);
    CHECK(woby::annotationOriginalVertices(restored,restored.annotations[0]) == vertices);
}

TEST_CASE("coordinate origin preserves each part's original bounds and radius")
{
    CoordinateFixture fixture;
    const auto path = fixture.obj("parts.obj", 8000000);
    std::ofstream(path, std::ios::app) << "g second\nv 8000005 -8000000 8000000\n"
        "v 8000005.125 -8000000 8000000\nv 8000005 -7999999.875 8000000\nf 6 7 8\n";
    woby::UiState state;
    state.files.push_back(woby::createUiFileState(path, woby::loadObjMesh(path), 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    const auto& file = state.files[0];
    REQUIRE(file.groupSettings.size() == 2);
    const auto& group = file.groupSettings[0];
    REQUIRE(file.mesh.bounds.radius > 2);
    const auto details = woby::controlObjectDetails(state, group.objectId,
        [](woby::SceneObjectId id) { return std::to_string(id); });
    CHECK(details["localBounds"]["radius"].get<double>() == doctest::Approx(group.localBounds.radius));
    CHECK(details["localBounds"]["min"][0].get<double>() == 8000000);
    CHECK(details["localBounds"]["max"][0].get<double>() == 8000000.125);
}
