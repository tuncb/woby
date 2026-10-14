#include "importer_tetrahedral_fixture.h"
#include "importer_host.h"
#include "automation_registry.h"
#include "background_load.h"
#include "scene_history.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <algorithm>
#include <fstream>
#include <limits>
#include <set>

namespace {
woby::Mesh copy(const ImportTetrahedralData& data, bool hierarchy = false)
{
    return woby::copyImportedMesh(data.result,hierarchy ? &data.hierarchy : nullptr,nullptr,nullptr,nullptr,nullptr,{},false,&data.tetrahedra);
}
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby-tetrahedra-" + woby::automationRandomHex(8));
    Fixture() { std::filesystem::create_directory(root); woby::unloadImporters(); }
    ~Fixture() { woby::unloadImporters(); std::error_code ignored; std::filesystem::remove_all(root,ignored); }
    std::filesystem::path file(const std::string& name = "mesh") const { const auto path = root / (name+".wtet"); std::ofstream(path) << "fixture"; return path; }
};
double orientation(const woby::Mesh& mesh, size_t index, const woby::Coordinate& inside)
{
    const auto& indices = meshGeometry(mesh).indices;
    auto a = woby::meshPosition(mesh,indices[index]), b = woby::meshPosition(mesh,indices[index+1]), c = woby::meshPosition(mesh,indices[index+2]);
    woby::Coordinate u{},v{},w{};
    for (size_t k = 0; k < 3; ++k) { u[k] = b[k]-a[k]; v[k] = c[k]-a[k]; w[k] = inside[k]-a[k]; }
    return (u[1]*v[2]-u[2]*v[1])*w[0] + (u[2]*v[0]-u[0]*v[2])*w[1] + (u[0]*v[1]-u[1]*v[0])*w[2];
}
}

TEST_CASE("Quadratic tetrahedra use all VTK nodes and outward boundary triangles")
{
    ImportTetrahedralData data;
    data.tetrahedra.cell_count = 1; data.tetrahedra.group_count = 0;
    SUBCASE("VTK order") {}
    SUBCASE("reversed corner order") { data.cells[0] = {10,{0,2,1,3,6,5,4,7,9,8}}; }
    const auto mesh = copy(data);
    const auto& g = meshGeometry(mesh);
    REQUIRE(g.indices.size() == 48);
    CHECK(std::set<uint32_t>(g.indices.begin(),g.indices.end()) == std::set<uint32_t>{0,1,2,3,4,5,6,7,8,9});
    const auto center = woby::relativePosition({.25,.25,.25},mesh.origin);
    for (size_t i = 0; i < g.indices.size(); i += 3) { CHECK(orientation(mesh,i,center) < 0); }
    REQUIRE(g.tetrahedra.size() == 1);
    CHECK(g.tetrahedra[0].nodes == std::array<uint32_t,10>{data.cells[0].nodes[0],data.cells[0].nodes[1],data.cells[0].nodes[2],3,
        data.cells[0].nodes[4],data.cells[0].nodes[5],data.cells[0].nodes[6],7,data.cells[0].nodes[8],data.cells[0].nodes[9]});
    CHECK(g.nodes[0].name == "Tetrahedra");
    CHECK(mesh.sourceData->indices == g.indices);
    CHECK(mesh.storage);
}

TEST_CASE("Quadratic tetrahedral boundaries use displaced mid-edge positions and retain precision")
{
    ImportTetrahedralData data;
    data.tetrahedra.cell_count = 1; data.tetrahedra.group_count = 0;
    data.vertices[4].position[2] = -.125;
    for (auto& vertex : data.vertices) { vertex.position[0] += 100000000.125; }
    const auto mesh = copy(data);
    CHECK(woby::originalPosition(woby::meshPosition(mesh,4),mesh.origin)[0] == 100000000.625);
    CHECK(woby::originalPosition(woby::meshPosition(mesh,4),mesh.origin)[2] == -.125);
    CHECK(std::count(meshGeometry(mesh).indices.begin(),meshGeometry(mesh).indices.end(),4) == 6);
    auto moved = mesh; woby::rebaseMesh(moved,{100000001,0,0});
    CHECK(woby::originalPosition(woby::meshPosition(moved,4),moved.origin)[2] == -.125);
    CHECK(meshGeometry(moved).tetrahedra[0].nodes == meshGeometry(mesh).tetrahedra[0].nodes);
}

TEST_CASE("Tetrahedral meshes remove matching internal faces across groups")
{
    ImportTetrahedralData data;
    SUBCASE("quadratic cells") {}
    SUBCASE("linear cells") { for (auto& cell : data.cells) { cell.node_count = 4; } }
    const auto mesh = copy(data,true);
    const auto& g = meshGeometry(mesh);
    const uint32_t count = data.cells[0].node_count == 10 ? 36u : 9u;
    REQUIRE(g.nodes.size() == 2);
    CHECK(g.indices.size() == 2*count);
    CHECK(g.nodes[0].indexOffset == 0); CHECK(g.nodes[0].indexCount == count);
    CHECK(g.nodes[1].indexOffset == count); CHECK(g.nodes[1].indexCount == count);
    CHECK(g.nodes[0].defaultColor == std::array<float,4>{1,0,0,.5f}); CHECK_FALSE(g.nodes[1].defaultVisible);
    CHECK(g.nodes[0].displayName == "Upper cell"); CHECK(g.hierarchy.size() == 3);
    CHECK(g.tetrahedra[0].groupIndex == 0); CHECK(g.tetrahedra[1].groupIndex == 1);
}

TEST_CASE("Tetrahedral faces match edge associations and shared vertex indexes")
{
    ImportTetrahedralData data;
    SUBCASE("different mid-edge IDs") { data.cells[1].nodes[6] = 13; data.cells[1].nodes[9] = 4; }
    SUBCASE("different edge association with the same IDs") { std::swap(data.cells[1].nodes[4],data.cells[1].nodes[5]); }
    SUBCASE("mixed linear and quadratic faces") { data.cells[1].node_count = 4; }
    const auto mesh = copy(data);
    CHECK(meshGeometry(mesh).indices.size() == (data.cells[1].node_count == 4 ? 60u : 96u));
}

TEST_CASE("Tetrahedral import validates cells groups and buffer limits")
{
    ImportTetrahedralData data;
    SUBCASE("short extension") { data.tetrahedra.struct_size = 0; }
    SUBCASE("null cells") { data.tetrahedra.cells = nullptr; }
    SUBCASE("null groups") { data.tetrahedra.groups = nullptr; }
    SUBCASE("too many cells before dereference") { data.tetrahedra.cell_count = UINT32_MAX; }
    SUBCASE("too many groups") { data.tetrahedra.group_count = 100001; }
    SUBCASE("wrong node count") { data.cells[0].node_count = 9; }
    SUBCASE("invalid corner") { data.cells[0].nodes[0] = 99; }
    SUBCASE("invalid mid-edge node") { data.cells[0].nodes[9] = 99; }
    SUBCASE("repeated mid-edge node") { data.cells[0].nodes[9] = 4; }
    SUBCASE("zero corner volume") { data.vertices[3].position[2] = 0; }
    SUBCASE("nonfinite coordinate") { data.vertices[4].position[1] = std::numeric_limits<double>::infinity(); }
    SUBCASE("duplicate cell") { data.cells[1] = data.cells[0]; }
    SUBCASE("unordered groups") { data.groups[0].index_offset = 1; }
    SUBCASE("empty group") { data.groups[0].index_count = 0; }
    SUBCASE("incomplete groups") { data.tetrahedra.group_count = 1; }
    SUBCASE("duplicate names") { data.groups[1].name = "upper"; }
    SUBCASE("invalid group color") { data.groups[0].color[0] = 2; }
    CHECK_THROWS_AS((void)copy(data),std::runtime_error);
}

TEST_CASE("Tetrahedral imports combine ordinary primitives with cell groups")
{
    ImportTetrahedralData data;
    uint32_t triangle[3]{0,1,2}, segment[2]{0,3}, point[1]{3};
    data.result.indices = triangle; data.result.index_count = 3;
    const WobyImportLines lines{sizeof(WobyImportLines),segment,2,nullptr,0};
    const WobyImportPoints points{sizeof(WobyImportPoints),point,1,nullptr,0};
    const auto mesh = woby::copyImportedMesh(data.result,nullptr,&lines,nullptr,&points,nullptr,{},false,&data.tetrahedra);
    const auto& g = meshGeometry(mesh);
    REQUIRE(g.nodes.size() == 5);
    CHECK(g.nodes[3].indexOffset == 3); CHECK(g.nodes[4].indexOffset == 39);
    CHECK(g.indices.size() == 75); CHECK(g.lineIndices.size() == 2); CHECK(g.pointIndices.size() == 1);
    CHECK(g.tetrahedra[0].groupIndex == 3);
}

TEST_CASE("Tetrahedral importer extension owns cells and releases failed and canceled results")
{
    Fixture fixture; woby::loadImporter(WOBY_TEST_TETRAHEDRAL_IMPORTER);
    const auto load = [&](const std::string& name) { return woby::importModel(fixture.file(name),{},{}); };
    const auto mesh = load("mesh").mesh;
    CHECK(meshGeometry(mesh).indices.size() == 72); CHECK(meshGeometry(mesh).tetrahedra.size() == 2);
    CHECK(meshGeometry(load("linear").mesh).indices.size() == 18);
    CHECK(meshGeometry(load("absent").mesh).indices.size() == 3);
    CHECK(meshGeometry(load("absent").mesh).tetrahedra.empty());
    for (const auto* name : {"invalid","failure"}) { CHECK_THROWS_AS((void)load(name),std::runtime_error); CHECK(meshGeometry(load("mesh").mesh).indices.size() == 72); }
    bool canceled = false;
    woby::ImportCallbacks callbacks; callbacks.progress = [&](float) { canceled = true; }; callbacks.canceled = [&] { return canceled; };
    CHECK(woby::importModel(fixture.file(),{},callbacks).canceled);
    CHECK(meshGeometry(load("mesh").mesh).indices.size() == 72);
    callbacks = {}; callbacks.canceled = [&] { return canceled; };
    callbacks.stageProgress = [&](const auto&) { canceled = true; }; canceled = false;
    CHECK(woby::importModel(fixture.file(),{},callbacks).canceled);
    CHECK(meshGeometry(load("mesh").mesh).indices.size() == 72);
    woby::unloadImporters(); CHECK(meshGeometry(mesh).tetrahedra[1].nodes[3] == 10);
}

TEST_CASE("Tetrahedral importer scene reload preserves hierarchy and cell group settings")
{
    Fixture fixture; woby::loadImporter(WOBY_TEST_TETRAHEDRAL_IMPORTER);
    auto loaded = woby::loadModelBatchCpu({fixture.file()},0,{},{});
    REQUIRE(loaded.files.size() == 1);
    woby::UiState state; state.files = std::move(loaded.files); woby::appendDefaultSceneNodesForFiles(state,0);
    woby::setGroupTranslation(state.files[0].groupSettings[0],{1,2,3});
    const auto document = woby::createSceneDocument(state);
    const auto path = fixture.root / "scene.woby";
    woby::writeSceneDocument(path,document);
    auto reloaded = woby::loadSceneCpu(path,{},{});
    REQUIRE(reloaded.files.size() == 1);
    CHECK(meshGeometry(reloaded.files[0].mesh).tetrahedra.size() == 2);
    const auto restored = woby::prepareSceneReplacement(state,std::move(reloaded.files),reloaded.document);
    CHECK(woby::createSceneDocument(restored) == document);
}

TEST_CASE("Interior tetrahedral groups retain their identity with no exposed faces")
{
    ImportTetrahedralData data;
    data.vertices[11].position[0] = 0; data.vertices[11].position[1] = -1; data.vertices[11].position[2] = 0;
    data.vertices[12].position[0] = -1; data.vertices[12].position[1] = 0; data.vertices[12].position[2] = 0;
    data.vertices[13].position[0] = 1; data.vertices[13].position[1] = 1; data.vertices[13].position[2] = 1;
    const WobyImportTetrahedron cells[5] = {{4,{0,1,2,3}},{4,{0,2,1,10}},{4,{0,1,3,11}},{4,{2,0,3,12}},{4,{1,2,3,13}}};
    const WobyImportGroup groups[2] = {{"inside",0,1,0,{}},{"outside",1,4,0,{}}};
    data.tetrahedra.cells = cells; data.tetrahedra.cell_count = 5; data.tetrahedra.groups = groups;
    const auto mesh = copy(data);
    REQUIRE(meshGeometry(mesh).nodes.size() == 2);
    CHECK(meshGeometry(mesh).nodes[0].name == "inside"); CHECK(meshGeometry(mesh).nodes[0].indexCount == 0);
    CHECK(meshGeometry(mesh).nodes[1].indexCount == 36);
    Fixture fixture;
    const auto state = woby::createUiFileState(fixture.root / "interior.wtet",mesh,0);
    CHECK(state.groupSettings.size() == 2);
}
