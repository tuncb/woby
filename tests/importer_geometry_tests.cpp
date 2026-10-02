#include "automation_registry.h"
#include "background_load.h"
#include "freeform.h"
#include "freeform_gpu.h"
#include "importer_host.h"
#include "importer_geometry_fixture.h"
#include "scene_history.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <fstream>
#include <limits>
#include <set>

namespace {
struct GeometryFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-geometry-" + woby::automationRandomHex(8));
    GeometryFixture() { std::filesystem::create_directory(root); woby::unloadImporters(); woby::loadImporter(WOBY_TEST_GEOMETRY_IMPORTER); }
    ~GeometryFixture() { woby::unloadImporters(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path file(const std::string& name = "mixed") const {
        const auto path = root / (name + ".wgeom"); std::ofstream(path) << "fixture"; return path;
    }
    woby::Mesh mesh(const std::string& name = "mixed") const { return woby::importModel(file(name), {}, {}).mesh; }
};
woby::Mesh copy(const ImportGeometryData& data) {
    return woby::copyImportedMesh(data.result,&data.hierarchy,&data.lines,&data.pointIds,&data.points,&data.freeform);
}
}

TEST_CASE("Geometry importer owns points rational curves and trimmed surfaces with hierarchy and source identities")
{
    GeometryFixture f;
    const auto mesh = f.mesh(); // DLL allocation is freed before any inspection.
    REQUIRE(mesh.nodes.size() == 5);
    CHECK(mesh.hierarchy.size() == 6);
    CHECK(mesh.nodes[2].pointIndexCount == 2);
    CHECK(mesh.nodes[2].displayName == "Points");
    CHECK_FALSE(mesh.nodes[2].defaultVisible);
    CHECK(mesh.nodes[2].defaultColor == std::array<float,4>{1,0,0,.5f});
    CHECK(mesh.pointIndices == std::vector<uint32_t>{0,3});
    CHECK(mesh.nodes[3].lineIndexOffset == 2);
    CHECK(mesh.nodes[3].lineIndexCount == 64);
    CHECK(mesh.nodes[4].indexOffset == 3);
    CHECK(mesh.nodes[4].hasTexcoords);
    REQUIRE(mesh.freeform);
    REQUIRE(mesh.freeform->patches.size() == 2);
    const auto sample = woby::evaluateFreeform(mesh.freeform->patches[0],.5);
    CHECK(sample.position[0] == doctest::Approx(std::sqrt(.5)));
    CHECK(sample.position[1] == doctest::Approx(std::sqrt(.5)));
    const auto& grid = mesh.freeform->grids[1];
    REQUIRE_FALSE(grid.samples.empty());
    double area = 0;
    for (size_t i = 0; i < grid.triangles.size(); i += 3) {
        const auto a = grid.samples[grid.triangles[i]], b = grid.samples[grid.triangles[i+1]], c = grid.samples[grid.triangles[i+2]];
        area += ((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))*.5;
        const double u = (a[0]+b[0]+c[0])/3, v = (a[1]+b[1]+c[1])/3;
        CHECK_FALSE((u > .25 && u < .75 && v > .25 && v < .75));
    }
    CHECK(area == doctest::Approx(.75));
    for (size_t i = 0; i < grid.samples.size(); ++i) {
        const auto& vertex = mesh.vertices[grid.vertexOffset+i];
        CHECK(vertex.normal == std::array<float,3>{0,0,-1});
        CHECK(vertex.texcoord[0] == doctest::Approx(grid.samples[i][0]));
        CHECK(vertex.texcoord[1] == doctest::Approx(1-grid.samples[i][1]));
    }
    REQUIRE(mesh.sourceData);
    CHECK(mesh.sourceData->provenance == woby::SourceProvenance::importerVertices);
    CHECK(mesh.sourceData->indices == mesh.indices);
    const auto& ids = mesh.sourceData->originalPointIds;
    REQUIRE(ids.size() == mesh.vertices.size());
    CHECK(ids[0] == 0); CHECK(ids[1] == UINT64_MAX); CHECK(ids[2] == 2); CHECK(ids[3] == 0);
    CHECK(std::set<uint64_t>(ids.begin(),ids.end()).size() == ids.size()-1);
    CHECK_FALSE(woby::packFreeformGpu(mesh.freeform->patches[0],mesh.freeform->grids[0],mesh.origin).empty());
    CHECK_FALSE(woby::packFreeformGpu(mesh.freeform->patches[1],grid,mesh.origin).empty());
    REQUIRE(mesh.freeform->patches[1].trimming);
    CHECK(mesh.freeform->patches[1].trimming->regions[0].holes[0].segments[0].interval == std::array<double,2>{4,0});
}

TEST_CASE("Geometry importer supports point clouds freeform-only results and absent extensions")
{
    GeometryFixture f;
    const auto cloud = f.mesh("pointsonly");
    CHECK(cloud.nodes.size() == 1); CHECK(cloud.pointIndices.size() == 2);
    CHECK(cloud.indices.empty()); CHECK(cloud.lineIndices.empty()); CHECK_FALSE(cloud.freeform);
    const auto splines = f.mesh("freeformonly");
    CHECK(splines.nodes.size() == 2); CHECK(splines.pointIndices.empty());
    CHECK(splines.sourceData->originalPointIds.empty());
    CHECK(splines.sourceData->provenance == woby::SourceProvenance::importerVertices);
    const auto absent = f.mesh("absent");
    CHECK(absent.nodes.size() == 1); CHECK(absent.nodes[0].indexCount == 3);
    CHECK(absent.pointIndices.empty()); CHECK_FALSE(absent.freeform);
}

TEST_CASE("Geometry importer releases rejected results and cancels during host tessellation")
{
    GeometryFixture f;
    for (const auto* name : {"invalid_point","invalid_spline","invalid_trim","failure"}) {
        CHECK_THROWS_AS((void)f.mesh(name),std::runtime_error);
        CHECK(f.mesh("pointsonly").pointIndices.size() == 2);
    }
    size_t updates = 0; bool canceled = false;
    woby::ImportCallbacks callbacks;
    callbacks.stageProgress = [&](const woby::ModelLoadProgress&) { if (++updates == 6) { canceled = true; } };
    callbacks.canceled = [&] { return canceled; };
    const auto model = woby::importModel(f.file("freeformonly"),{},callbacks);
    CHECK(model.canceled); CHECK(woby::empty(model.mesh)); CHECK(updates == 6);
    CHECK(f.mesh("pointsonly").pointIndices.size() == 2);
    callbacks = {};
    callbacks.stageProgress = [](const woby::ModelLoadProgress&) { throw std::runtime_error("Host progress failed."); };
    CHECK_THROWS_WITH_AS((void)woby::importModel(f.file(),{},callbacks),"Host progress failed.",std::runtime_error);
    CHECK(f.mesh("pointsonly").pointIndices.size() == 2);
}

TEST_CASE("Geometry importer validates point partitions indexes names and limits")
{
    ImportGeometryData g;
    SUBCASE("short structure") { g.points.struct_size = 0; }
    SUBCASE("null indices") { g.points.indices = nullptr; }
    SUBCASE("out of range") { g.dots[0] = 4; }
    SUBCASE("null groups") { g.points.groups = nullptr; }
    SUBCASE("gap") { g.pointGroup.index_offset = 1; }
    SUBCASE("empty group") { g.pointGroup.index_count = 0; }
    SUBCASE("uncovered") { g.pointGroup.index_count = 1; }
    SUBCASE("overrun") { g.pointGroup.index_count = 3; }
    SUBCASE("duplicate name") { g.pointGroup.name = "arc"; }
    SUBCASE("default name conflict") { g.pointGroup.name = "Mesh"; }
    SUBCASE("unknown flags") { g.pointGroup.flags = 8; }
    SUBCASE("bad color") { g.pointGroup.color[0] = std::numeric_limits<float>::quiet_NaN(); }
    SUBCASE("too many groups") { g.points.group_count = 100001; }
    SUBCASE("combined groups") { g.points.group_count = 99999; }
    SUBCASE("byte budget before reading indices") { g.points.index_count = UINT32_MAX; }
    SUBCASE("generated points group") {
        g.points.groups = nullptr; g.points.group_count = 0;
        CHECK(copy(g).nodes[2].name == "Points"); return;
    }
    CHECK_THROWS_AS((void)copy(g),std::runtime_error);
}

TEST_CASE("Geometry importer validates spline buffers weights domains dimensions and attributes")
{
    ImportGeometryData g;
    auto& p = g.patches[0].spline;
    SUBCASE("short extension") { g.freeform.struct_size = 0; }
    SUBCASE("null patches") { g.freeform.patches = nullptr; }
    SUBCASE("too many patches") { g.freeform.patch_count = 100001; }
    SUBCASE("short spline") { p.struct_size = 0; }
    SUBCASE("unknown kind") { p.kind = 2; }
    SUBCASE("unknown flags") { p.flags = 8; }
    SUBCASE("null controls") { p.controls = nullptr; }
    SUBCASE("control count mismatch") { p.control_count = 2; }
    SUBCASE("control budget") { p.control_count = p.count_u = 2000001; p.knot_count_u = 2000004; }
    SUBCASE("degree") { p.degree_u = 9; }
    SUBCASE("invalid curve V") { p.count_v = 2; }
    SUBCASE("null knots") { p.knots_u = nullptr; }
    SUBCASE("knot count") { p.knot_count_u = 5; }
    SUBCASE("decreasing knots") { g.quadratic[3] = -1; }
    SUBCASE("invalid domain") { p.domain_u[1] = 2; }
    SUBCASE("NaN domain") { p.domain_u[0] = std::numeric_limits<double>::quiet_NaN(); }
    SUBCASE("zero weight") { g.arc[0].weight = 0; }
    SUBCASE("negative weight") { g.arc[0].weight = -1; }
    SUBCASE("infinite weight") { g.arc[0].weight = std::numeric_limits<double>::infinity(); }
    SUBCASE("invalid position") { g.arc[0].position[0] = 1e10; }
    SUBCASE("nonfinite normal") { g.surface[0].normal[0] = std::numeric_limits<double>::infinity(); }
    SUBCASE("invalid UV") { g.surface[0].texcoord[0] = std::numeric_limits<double>::quiet_NaN(); }
    SUBCASE("invalid surface V") { g.patches[1].spline.degree_v = 0; }
    SUBCASE("nonzero patch group range") { g.patches[0].group.index_count = 1; }
    CHECK_THROWS_AS((void)copy(g),std::runtime_error);
}

TEST_CASE("Geometry importer validates trim references intervals loops and metadata budgets")
{
    ImportGeometryData g;
    SUBCASE("null trim curves") { g.freeform.trim_curves = nullptr; }
    SUBCASE("too many trim curves") { g.freeform.trim_curve_count = 100001; }
    SUBCASE("trim control budget before dereference") { g.trim.control_count = g.trim.count_u = 4097; g.trim.knot_count_u = 4099; }
    SUBCASE("non UV trim") { g.boundary[0].position[2] = 1; }
    SUBCASE("trim attributes") { g.trim.flags = WOBY_IMPORT_HAS_TEXCOORDS; }
    SUBCASE("null regions") { g.patches[1].regions = nullptr; }
    SUBCASE("region budget") { g.patches[1].region_count = 100001; }
    SUBCASE("trim on curve") { g.patches[0].regions = &g.region; g.patches[0].region_count = 1; }
    SUBCASE("null holes") { g.region.holes = nullptr; }
    SUBCASE("empty hole") { g.hole.segment_count = 0; }
    SUBCASE("null segments") { g.hole.segments = nullptr; }
    SUBCASE("segment budget") { g.hole.segment_count = 100000; }
    SUBCASE("invalid reference") { g.trimSegment.curve_index = 1; }
    SUBCASE("invalid interval") { g.trimSegment.interval[0] = 5; }
    SUBCASE("empty interval") { g.trimSegment.interval[0] = 0; }
    SUBCASE("nonfinite interval") { g.trimSegment.interval[0] = std::numeric_limits<double>::quiet_NaN(); }
    SUBCASE("open loop") { g.boundary[4].position[0] = .3; }
    SUBCASE("outside surface") { g.boundary[1].position[0] = 2; }
    SUBCASE("crossing loop") { g.boundary[1].position[1] = .8; }
    CHECK_THROWS_AS((void)copy(g),std::runtime_error);
}

TEST_CASE("Geometry importer retains freeform precision before and after rebasing")
{
    ImportGeometryData g;
    g.result = {}; g.result.struct_size = sizeof(g.result);
    g.freeform.patch_count = 1; g.freeform.trim_curve_count = 0;
    for (auto& point : g.arc) { point.position[0] += 100000000.0; point.position[1] += 100000000.0; }
    const auto mesh = woby::copyImportedMesh(g.result,nullptr,nullptr,nullptr,nullptr,&g.freeform);
    CHECK(mesh.origin[0] == 100000000.5);
    CHECK(mesh.precisePositions.front()[0] == .5);
    CHECK(mesh.vertices.front().position[0] == .5f);
    CHECK(woby::evaluateFreeform(mesh.freeform->patches[0],.5).position[0] == doctest::Approx(100000000.0+std::sqrt(.5)));
    auto rebased = mesh; woby::rebaseMesh(rebased,{100000001,100000001,0});
    CHECK(rebased.precisePositions.front()[0] == 0);
    CHECK(rebased.freeform == mesh.freeform);
}

TEST_CASE("Geometry importer supports Bezier and rational untrimmed patches and large trim parameter domains")
{
    ImportGeometryData g;
    SUBCASE("nonrational Bezier curve") {
        g.arc[1].weight = 1;
        const auto mesh = copy(g);
        const auto point = woby::evaluateFreeform(mesh.freeform->patches[0],.5).position;
        CHECK(point == woby::Coordinate{.75,.75,0});
    }
    SUBCASE("rational untrimmed surface") {
        g.patches[1].regions = nullptr; g.patches[1].region_count = 0;
        for (size_t i = 0; i < 4; ++i) { g.surface[i].weight = double(i+1); }
        const auto mesh = copy(g);
        CHECK(mesh.freeform->grids[1].samples.empty());
        const auto point = woby::evaluateFreeform(mesh.freeform->patches[1],.5,.5).position;
        CHECK(point[0] == doctest::Approx(.6)); CHECK(point[1] == doctest::Approx(.7)); CHECK(point[2] == 1);
    }
    SUBCASE("UV parameters are not XYZ positions") {
        double knots[] = {1e10,1e10,1e10+1,1e10+1};
        g.patches[1].spline.knots_u = knots;
        g.patches[1].spline.domain_u[0] = 1e10; g.patches[1].spline.domain_u[1] = 1e10+1;
        for (auto& point : g.boundary) { point.position[0] += 1e10; }
        const auto mesh = copy(g);
        CHECK_FALSE(mesh.freeform->grids[1].samples.empty());
        CHECK(mesh.bounds.max[0] == 1);
    }
}

TEST_CASE("Geometry importer scene round trip preserves all primitive kinds and appearance")
{
    GeometryFixture f;
    auto loaded = woby::loadModelBatchCpu({f.file()},0,{},{});
    REQUIRE(loaded.files.size() == 1);
    woby::UiState state; state.files = std::move(loaded.files); woby::appendDefaultSceneNodesForFiles(state,0);
    woby::selectSceneObject(state,state.files[0].groupSettings[2].objectId);
    woby::setSelectedObjectsVisible(state,true);
    woby::setSelectedObjectProperty(state,woby::UiObjectProperty::red,.3f);
    const auto document = woby::createSceneDocument(state);
    CHECK(document.files[0].groups[2].pointGroup);
    CHECK(document.files[0].groups[3].lineGroup);
    CHECK_FALSE(document.files[0].groups[4].lineGroup);
    const auto path = f.root / "scene.woby";
    woby::writeSceneDocument(path,document);
    auto reopened = woby::loadSceneCpu(path,{},{});
    const auto restored = woby::prepareSceneReplacement(state,std::move(reopened.files),reopened.document);
    CHECK(woby::createSceneDocument(restored) == document);
    CHECK(restored.files[0].mesh.freeform->grids[1].triangles == state.files[0].mesh.freeform->grids[1].triangles);
}
