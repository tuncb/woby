#include "importer_tetrahedral_fixture.h"
#include "importer_host.h"
#include "automation_registry.h"
#include "background_load.h"
#include "scene_history.h"
#include "ui_operations.h"
#include "tetrahedral_mesh.h"
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
void reverseCell(WobyImportTetrahedron& cell)
{
    const auto original = cell;
    constexpr std::array<size_t,10> order{0,2,1,3,6,5,4,7,9,8};
    for (size_t i = 0; i < order.size(); ++i) { cell.nodes[i] = original.nodes[order[i]]; }
}
void checkSource(const woby::Mesh& mesh, const ImportTetrahedralData& data)
{
    const auto& geometry = meshGeometry(mesh);
    REQUIRE(geometry.tetrahedra.size() == data.tetrahedra.cell_count);
    for (size_t i = 0; i < geometry.tetrahedra.size(); ++i) {
        const auto& source = data.tetrahedra.cells[i];
        CHECK(geometry.tetrahedra[i].nodeCount == source.node_count);
        for (size_t n = 0; n < 10; ++n) { CHECK(geometry.tetrahedra[i].nodes[n] == source.nodes[n]); }
    }
    REQUIRE(geometry.vertices.size() == data.result.vertex_count);
    for (uint32_t i = 0; i < data.result.vertex_count; ++i) {
        const auto& p = data.result.vertices[i].position;
        CHECK(woby::originalPosition(woby::meshPosition(mesh,i),mesh.origin) == woby::Coordinate{p[0],p[1],p[2]});
    }
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

TEST_CASE("Quadratic tetrahedra use curved geometry for the issue 119 boundary")
{
    // The first cell has a negative corner determinant and a positive center Jacobian.
    constexpr std::array<woby::Coordinate,14> positions{{
        {272.92035349877636,2390.2553623928857,134.8257624390649},
        {275.68479008902028,2422.1969739401738,131.49299325030989},
        {249.92428150424709,2389.2405633950011,133.39210350954471},
        {250.36109179433774,2405.6786404120467,131.61050521753958},
        {274.51974212207801,2404.9580438480252,133.3128868890646},
        {262.55069913101613,2405.4905326381022,132.43320981731009},
        {260.90926876260551,2396.3736128110108,133.34424713455465},
        {263.10233617363963,2402.1736267792639,132.85242384805875},
        {265.4491630780737,2413.232187389744,131.79252468655423},
        {248.67307998918102,2399.7493350454756,132.14985415444607},
        {259.03528859409022,2407.9724624938399,98.974339403384761},
        {253.54309888945508,2409.4945524815662,114.97725338178773},
        {268.29121230789019,2413.5896268827887,115.0747138146025},
        {254.3635074243922,2398.7790346086781,116.21018683577208}}};
    for (const bool translated : {false,true}) {
        for (unsigned reversed = 0; reversed < 4; ++reversed) {
            CAPTURE(translated); CAPTURE(reversed);
            ImportTetrahedralData data;
            data.tetrahedra.group_count = 0;
            data.cells[1] = {10,{3,1,2,10,8,5,9,11,12,13}};
            for (size_t i = 0; i < positions.size(); ++i) {
                for (size_t k = 0; k < 3; ++k) { data.vertices[i].position[k] = positions[i][k] + (translated ? 100000000.125 : 0); }
            }
            if (reversed & 1u) { reverseCell(data.cells[0]); }
            if (reversed & 2u) { reverseCell(data.cells[1]); }
            const auto mesh = copy(data);
            const auto& g = meshGeometry(mesh);
            REQUIRE(g.indices.size() == 72);
            CHECK(g.nodes[0].indexCount == 72);
            const std::set<uint32_t> shared{1,2,3,5,8,9};
            for (size_t i = 0; i < g.indices.size(); i += 3) {
                CHECK_FALSE((shared.contains(g.indices[i]) && shared.contains(g.indices[i+1]) && shared.contains(g.indices[i+2])));
            }
            checkSource(mesh,data);
        }
    }
}

TEST_CASE("Quadratic tetrahedra retain internal face removal with negative or zero corner volume")
{
    ImportTetrahedralData data;
    SUBCASE("negative corner volume") { data.vertices[3].position[2] = -1; }
    SUBCASE("zero corner volume") { data.vertices[3].position[2] = 0; }
    const auto mesh = copy(data);
    CHECK(meshGeometry(mesh).indices.size() == 72);
    checkSource(mesh,data);
}

TEST_CASE("Tetrahedral imports keep every face when orientation is unknown")
{
    ImportTetrahedralData data;
    SUBCASE("zero quadratic center Jacobian with positive corner volume") {
        for (size_t i = 4; i < 10; ++i) { data.vertices[i].position[2] = 0; }
    }
    SUBCASE("zero quadratic center Jacobian with negative corner volume") {
        for (size_t i = 4; i < 10; ++i) { data.vertices[i].position[2] = 0; }
        data.vertices[3].position[2] = -1;
    }
    SUBCASE("repeated quadratic edge node with nonzero center Jacobian") { data.cells[0].nodes[9] = 7; }
    SUBCASE("repeated quadratic corner node") { data.cells[0].nodes[3] = 0; }
    SUBCASE("zero linear corner volume") {
        for (auto& cell : data.cells) { cell.node_count = 4; }
        data.vertices[3].position[2] = 0;
    }
    SUBCASE("repeated linear corner node") {
        for (auto& cell : data.cells) { cell.node_count = 4; }
        data.cells[0].nodes[3] = 0;
    }
    const auto mesh = copy(data);
    const auto& g = meshGeometry(mesh);
    CHECK(g.indices.size() == (data.cells[0].node_count == 10 ? 96u : 24u));
    // Unknown orientation retains the first cell's source winding.
    CHECK(g.indices[0] == 0);
    CHECK(g.indices[1] == (data.cells[0].node_count == 10 ? 4u : 1u));
    CHECK(g.indices[2] == (data.cells[0].node_count == 10 ? 7u : data.cells[0].nodes[3]));
    checkSource(mesh,data);
}

TEST_CASE("Tetrahedral boundaries accept nonfinite computed determinants from finite positions")
{
    for (const uint32_t nodeCount : {4u,10u}) {
        ImportTetrahedralData data;
        woby::MeshGeometry g;
        for (const auto& vertex : data.vertices) {
            g.precisePositions.push_back({vertex.position[0]*1e200,vertex.position[1]*1e200,vertex.position[2]*1e200});
        }
        g.nodes.resize(1);
        g.tetrahedra = {{nodeCount,{0,1,2,3,4,5,6,7,8,9},0},{nodeCount,{0,2,1,10,6,5,4,11,12,13},0}};
        const auto positions = g.precisePositions;
        woby::appendTetrahedralBoundary(g);
        CHECK(g.indices.size() == (nodeCount == 10 ? 96u : 24u));
        CHECK(g.indices[1] == (nodeCount == 10 ? 4u : 1u));
        CHECK(g.precisePositions == positions);
        CHECK(g.tetrahedra.size() == 2);
    }
}

TEST_CASE("Tetrahedral imports preserve duplicate and overlapping cells")
{
    for (const uint32_t nodeCount : {4u,10u}) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            CAPTURE(nodeCount); CAPTURE(mode);
            ImportTetrahedralData data;
            if (mode < 2) {
                data.cells[1] = data.cells[0];
                if (mode == 1) { reverseCell(data.cells[1]); }
            } else {
                // Put the neighbor on the same side of the shared face.
                for (size_t i = 10; i < 14; ++i) { data.vertices[i].position[2] *= -1; }
            }
            for (auto& cell : data.cells) { cell.node_count = nodeCount; }
            const auto mesh = copy(data);
            const auto& g = meshGeometry(mesh);
            CHECK(g.indices.size() == (nodeCount == 10 ? 96u : 24u));
            CHECK(g.nodes[0].indexCount == (nodeCount == 10 ? 48u : 12u));
            CHECK(g.nodes[1].indexCount == (nodeCount == 10 ? 48u : 12u));
            checkSource(mesh,data);
        }
    }
}

TEST_CASE("Tetrahedral imports retain all faces with more than two incident cells")
{
    for (const uint32_t nodeCount : {4u,10u}) {
        for (size_t rotation = 0; rotation < 3; ++rotation) {
            CAPTURE(nodeCount); CAPTURE(rotation);
            ImportTetrahedralData data;
            std::vector<WobyImportVertex> vertices(std::begin(data.vertices),std::end(data.vertices));
            vertices.insert(vertices.end(), {{{0,0,2},{},{}},{{0,0,1},{},{}},{{.5,0,1},{},{}},{{0,.5,1},{},{}}});
            data.result.vertices = vertices.data(); data.result.vertex_count = static_cast<uint32_t>(vertices.size());
            std::array<WobyImportTetrahedron,3> cells{data.cells[0],data.cells[1],WobyImportTetrahedron{10,{0,1,2,14,4,5,6,15,16,17}}};
            for (auto& cell : cells) { cell.node_count = nodeCount; }
            std::rotate(cells.begin(),cells.begin()+rotation,cells.end());
            data.tetrahedra.cells = cells.data(); data.tetrahedra.cell_count = 3; data.tetrahedra.group_count = 0;
            const auto mesh = copy(data);
            CHECK(meshGeometry(mesh).indices.size() == (nodeCount == 10 ? 144u : 36u));
            checkSource(mesh,data);
        }
    }
}

TEST_CASE("Linear tetrahedra retain corner-based outward winding")
{
    for (const bool reversed : {false,true}) {
        ImportTetrahedralData data;
        data.tetrahedra.cell_count = 1; data.tetrahedra.group_count = 0;
        data.cells[0].node_count = 4;
        if (reversed) { reverseCell(data.cells[0]); }
        const auto mesh = copy(data);
        const auto& g = meshGeometry(mesh);
        REQUIRE(g.indices.size() == 12);
        const auto center = woby::relativePosition({.25,.25,.25},mesh.origin);
        for (size_t i = 0; i < g.indices.size(); i += 3) { CHECK(orientation(mesh,i,center) < 0); }
        checkSource(mesh,data);
    }
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
    SUBCASE("nonfinite coordinate") { data.vertices[4].position[1] = std::numeric_limits<double>::infinity(); }
    SUBCASE("NaN coordinate") { data.vertices[0].position[0] = std::numeric_limits<double>::quiet_NaN(); }
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
