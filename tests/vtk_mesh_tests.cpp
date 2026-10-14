#include "vtk_mesh.h"
#include "importer_tetrahedral_fixture.h"
#include "automation_registry.h"
#include "background_load.h"
#include "file_discovery.h"
#include "model_load.h"
#include "scene_history.h"
#include "ui_operations.h"
#include <doctest/doctest.h>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
std::string vtk(const std::vector<std::vector<uint32_t>>& cells = {{0,1,2,3,4,5,6,7,8,9}},
    const std::vector<uint32_t>& types = {24}, bool modern = false)
{
    const ImportTetrahedralData data;
    std::ostringstream text; text << std::setprecision(17);
    text << "# vtk DataFile Version " << (modern ? "5.1" : "3.0") << "\nTetrahedral test\nASCII\nDATASET UNSTRUCTURED_GRID\nPOINTS 14 double\n";
    for (const auto& vertex : data.vertices) { text << vertex.position[0] << ' ' << vertex.position[1] << ' ' << vertex.position[2] << '\n'; }
    size_t size = 0; for (const auto& cell : cells) { size += cell.size(); }
    text << "CELLS " << cells.size()+(modern ? 1u : 0u) << ' ' << size+(modern ? 0u : cells.size()) << '\n';
    if (modern) {
        text << "OFFSETS vtktypeint64\n0 ";
        size_t offset = 0; for (const auto& cell : cells) { offset += cell.size(); text << offset << ' '; }
        text << "\nCONNECTIVITY vtktypeint64\n";
    }
    for (const auto& cell : cells) {
        if (!modern) { text << cell.size() << ' '; }
        for (const auto index : cell) { text << index << ' '; }
        text << '\n';
    }
    text << "CELL_TYPES " << types.size() << '\n';
    for (const auto type : types) { text << type << '\n'; }
    return text.str();
}
woby::Mesh parse(const std::string& text)
{
    std::istringstream input(text); return woby::loadVtkMesh(input);
}
void replace(std::string& text, const std::string& from, const std::string& to)
{
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at,from.size(),to);
}
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / ("woby-vtk-" + woby::automationRandomHex(8));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
    std::filesystem::path write(const std::string& name, const std::string& contents) const {
        const auto path = root / name; std::ofstream(path,std::ios::binary) << contents; return path;
    }
};
}

TEST_CASE("VTK reader supports legacy and modern ASCII tetrahedral connectivity")
{
    for (const bool modern : {false,true}) {
        const auto mesh = parse(vtk({{0,1,2,3,4,5,6,7,8,9},{0,2,1,10,6,5,4,11,12,13}},{24,24},modern));
        const auto& g = meshGeometry(mesh);
        CHECK(g.indices.size() == 72); REQUIRE(g.tetrahedra.size() == 2);
        CHECK(g.tetrahedra[0].nodeCount == 10);
        CHECK(g.tetrahedra[1].nodes == std::array<uint32_t,10>{0,2,1,10,6,5,4,11,12,13});
        CHECK(g.nodes[0].name == "Tetrahedra"); CHECK(mesh.sourceData->indices == g.indices);
        CHECK(woby::originalPosition(woby::meshPosition(mesh,10),mesh.origin) == woby::Coordinate{0,0,-1});
    }
    CHECK(meshGeometry(parse(vtk({{0,1,2,3},{0,2,1,10}},{10,10}))).indices.size() == 18);
}

TEST_CASE("VTK reader supports mixed point line surface and tetrahedral cells")
{
    for (const bool modern : {false,true}) {
        const auto mesh = parse(vtk({{0},{0,1},{0,1},{0,1,2},{0,1,2},{0,1,2,3},{0,1,2,4,5,6},{0,2,1,10},{0,1,2,3,4,5,6,7,8,9}},
            {1,2,3,4,5,9,22,10,24},modern));
        const auto& g = meshGeometry(mesh);
        CHECK(g.pointIndices == std::vector<uint32_t>{0,0,1});
        CHECK(g.lineIndices == std::vector<uint32_t>{0,1,0,1,1,2});
        CHECK(g.indices.size() == 81);
        REQUIRE(g.nodes.size() == 4);
        CHECK(g.nodes[0].indexCount == 21); CHECK(g.nodes[3].indexOffset == 21); CHECK(g.nodes[3].indexCount == 60);
        CHECK(g.tetrahedra[0].groupIndex == 3);
    }
    CHECK(meshGeometry(parse(vtk({{0,1,2}},{2}))).pointIndices.size() == 3);
    CHECK(meshGeometry(parse(vtk({{0,1,2}},{4}))).lineIndices.size() == 4);
    CHECK(meshGeometry(parse(vtk({{0,1,2}},{5}))).indices.size() == 3);
}

TEST_CASE("VTK reader accepts CRLF scientific coordinates and trailing attributes")
{
    auto text = vtk();
    replace(text,"POINTS 14 double\n0 0 0", "POINTS 14 float\n+0.0 0e0 -0.0");
    text += "POINT_DATA 14\nSCALARS stress float\nLOOKUP_TABLE default\n";
    std::string crlf; for (const auto c : text) { if (c == '\n') { crlf += '\r'; } crlf += c; }
    CHECK(meshGeometry(parse(crlf)).indices.size() == 48);
    auto older = vtk(); replace(older,"Version 3.0","Version 2.0"); CHECK(meshGeometry(parse(older)).indices.size() == 48);
    replace(older,"Version 2.0","Version 4.2"); CHECK(meshGeometry(parse(older)).indices.size() == 48);
}

TEST_CASE("VTK reader rejects malformed or unsupported geometry")
{
    auto text = vtk();
    SUBCASE("binary") { replace(text,"ASCII","BINARY"); }
    SUBCASE("XML") { text = "<VTKFile type=\"UnstructuredGrid\">"; }
    SUBCASE("version") { replace(text,"Version 3.0","Version 9.0"); }
    SUBCASE("dataset") { replace(text,"UNSTRUCTURED_GRID","POLYDATA"); }
    SUBCASE("point type") { replace(text,"14 double","14 int"); }
    SUBCASE("nonfinite coordinates") { replace(text,"0 0 0\n","nan 0 0\n"); }
    SUBCASE("large coordinates") { replace(text,"0 0 0\n","1e10 0 0\n"); }
    SUBCASE("truncated points") { text.resize(text.find("CELLS")-4); }
    SUBCASE("invalid point index") { text = vtk({{0,1,2,3,4,5,6,7,8,99}},{24}); }
    SUBCASE("negative point index") { replace(text,"10 0 1 2","10 -1 1 2"); }
    SUBCASE("wrong list size") { replace(text,"CELLS 1 11","CELLS 1 12"); }
    SUBCASE("list size overflow") { replace(text,"CELLS 1 11","CELLS 1 4294967296"); }
    SUBCASE("wrong type count") { replace(text,"CELL_TYPES 1","CELL_TYPES 2"); }
    SUBCASE("truncated types") { text.resize(text.find("CELL_TYPES")); }
    SUBCASE("wrong tetrahedron node count") { text = vtk({{0,1,2,3}},{24}); }
    SUBCASE("unsupported cell") { text = vtk({{0,1,2,3,4,5,6,7}},{12}); }
    SUBCASE("wrong triangle count") { text = vtk({{0,1}},{5}); }
    SUBCASE("short polyline") { text = vtk({{0}},{4}); }
    SUBCASE("empty cell") { text = vtk({{}},{2}); }
    SUBCASE("empty mesh") { text = vtk({},{}); }
    SUBCASE("unknown tail") { text += "POLYGONS 0 0\n"; }
    SUBCASE("oversized header") { text.insert(text.find("Tetrahedral test"),300,'a'); }
    CHECK_THROWS_AS((void)parse(text),std::runtime_error);
}

TEST_CASE("VTK reader validates modern offsets and connectivity")
{
    auto text = vtk({{0,1,2,3,4,5,6,7,8,9}},{24},true);
    SUBCASE("wrong first offset") { replace(text,"\n0 10 ","\n1 10 "); }
    SUBCASE("wrong last offset") { replace(text,"\n0 10 ","\n0 9 "); }
    SUBCASE("offset beyond connectivity") { replace(text,"\n0 10 ","\n0 11 "); }
    SUBCASE("empty offset list") { replace(text,"CELLS 2 10","CELLS 0 10"); }
    SUBCASE("offset type") { replace(text,"OFFSETS vtktypeint64","OFFSETS float"); }
    SUBCASE("missing connectivity") { replace(text,"CONNECTIVITY","BROKEN"); }
    SUBCASE("connectivity index") { replace(text,"0 1 2 3 4 5 6 7 8 9","0 1 2 3 4 5 6 7 8 99"); }
    SUBCASE("decreasing offsets") {
        text = vtk({{0,1,2,3},{0,2,1,10}},{10,10},true); replace(text,"\n0 4 8 ","\n0 4 3 ");
    }
    CHECK_THROWS_AS((void)parse(text),std::runtime_error);
}

TEST_CASE("Native VTK loading supports discovery deferred preparation cancellation and scene reload")
{
    Fixture fixture;
    const auto path = fixture.write("tetra.VTK",vtk());
    CHECK(woby::isVtkPath(path)); CHECK(woby::isModelPath(path)); CHECK_FALSE(woby::isObjPath(path));
    CHECK(woby::collectModelPathsRecursive(fixture.root) == std::vector<std::filesystem::path>{path});
    woby::ImportCallbacks callbacks; callbacks.deferPreparation = true;
    const auto model = woby::loadModel(path,{},callbacks);
    CHECK(model.importerId.empty()); CHECK(model.mesh.preparationDeferred); CHECK(model.mesh.storage);
    CHECK(meshGeometry(model.mesh).tetrahedra.size() == 1);
    bool canceled = false; callbacks.canceled = [&] { return canceled; };
    callbacks.stageProgress = [&](const auto&) { canceled = true; };
    CHECK(woby::loadModel(path,{},callbacks).canceled);
    CHECK(meshGeometry(woby::loadModel(path).mesh).indices.size() == 48);
    auto loaded = woby::loadModelBatchCpu({path},0,{},{});
    REQUIRE(loaded.files.size() == 1);
    woby::UiState state; state.files = std::move(loaded.files); woby::appendDefaultSceneNodesForFiles(state,0);
    woby::setGroupTranslation(state.files[0].groupSettings[0],{2,3,4});
    const auto document = woby::createSceneDocument(state);
    const auto scenePath = fixture.root / "scene.woby";
    woby::writeSceneDocument(scenePath,document);
    auto reopened = woby::loadSceneCpu(scenePath,{},{});
    REQUIRE(reopened.files.size() == 1);
    CHECK(meshGeometry(reopened.files[0].mesh).tetrahedra[0].nodeCount == 10);
    const auto restored = woby::prepareSceneReplacement(state,std::move(reopened.files),reopened.document);
    CHECK(woby::createSceneDocument(restored) == document);
}
