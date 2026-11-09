#include "tetrahedral_quality.h"
#include "tetrahedral_mesh.h"
#include "analysis_results.h"
#include "comparison_scene.h"
#include "comparison_report.h"
#include "control_scene.h"
#include "ui_operations.h"
#include "automation_registry.h"
#include "scene_history.h"
#include <doctest/doctest.h>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>

namespace {
using namespace woby;
constexpr std::array<Coordinate,10> reference{{{0,0,0},{1,0,0},{0,1,0},{0,0,1},
    {.5,0,0},{.5,.5,0},{0,.5,0},{0,0,.5},{.5,0,.5},{0,.5,.5}}};
Mesh mesh(std::vector<Coordinate> positions, bool quadratic = false) {
    Mesh result; auto& g = editMeshGeometry(result);
    g.precisePositions = std::move(positions); g.vertices.resize(g.precisePositions.size());
    for (size_t i = 0; i < g.vertices.size(); ++i) { for (size_t k = 0; k < 3; ++k) { g.vertices[i].position[k] = static_cast<float>(g.precisePositions[i][k]); } }
    g.nodes.push_back({"Cells",0,0});
    TetrahedralCell cell; cell.nodeCount = quadratic ? 10 : 4; std::iota(cell.nodes.begin(),cell.nodes.end(),0); cell.sourceCell = 42;
    g.tetrahedra.push_back(cell); appendTetrahedralBoundary(g); result.bounds = calculateBounds(g.vertices); publishMeshGeometry(result); return result;
}
Mesh regular(bool quadratic = false) {
    const std::array<Coordinate,4> p{{{0,0,0},{1,0,0},{.5,std::sqrt(3.0)*.5,0},{.5,std::sqrt(3.0)/6,std::sqrt(2.0/3.0)}}};
    std::vector<Coordinate> nodes;
    for (const auto& r : reference) {
        Coordinate point{}; for (size_t k = 0; k < 3; ++k) { point[k] = r[0]*p[1][k]+r[1]*p[2][k]+r[2]*p[3][k]; } nodes.push_back(point);
        if (!quadratic && nodes.size() == 4) { break; }
    }
    return mesh(nodes,quadratic);
}
template<class Mapping> Mesh curved(Mapping mapping) {
    std::vector<Coordinate> nodes; for (const auto& p : reference) { nodes.push_back(mapping(p)); } return mesh(nodes,true);
}
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())/("woby-tet-quality-"+automationRandomHex(8));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
}

TEST_CASE("Tetrahedral quality matches analytical regular and right cells") {
    for (const bool quadratic : {false,true}) {
        const auto m = regular(quadratic); const auto q = inspectTetrahedralQuality(m); REQUIRE(q.cells.size() == 1);
        const auto& c = q.cells[0]; CHECK(c.validity == TetrahedralValidity::positive);
        CHECK(c.values[0] == doctest::Approx(std::sqrt(2.0)/12));
        CHECK(c.values[1] == doctest::Approx(1)); CHECK(c.values[2] == doctest::Approx(1));
        CHECK(c.values[3] == doctest::Approx(std::acos(1.0/3)*180/std::acos(-1.0))); CHECK(c.values[4] == doctest::Approx(c.values[3]));
        CHECK(c.values[5] <= std::sqrt(.5)); CHECK(c.values[6] >= std::sqrt(.5)); CHECK(c.values[7] == doctest::Approx(1));
        CHECK(q.quadraticCells == size_t(quadratic)); CHECK(c.sourceCell == 42);
        const auto display = tetrahedralQualityDisplay(m,q); CHECK(display.indices.size() == (quadratic ? 48 : 12));
        CHECK(display.faces[0].size() == display.indices.size()/3); CHECK(display.faces[1].size() == display.faces[0].size()); CHECK(display.edges.size() == display.indices.size()*2);
        for (const auto index : display.indices) { CHECK(index < meshGeometry(m).vertices.size()); }
    }
    const auto q = inspectTetrahedralQuality(mesh({reference[0],reference[1],reference[2],reference[3]}));
    CHECK(q.cells[0].values[0] == doctest::Approx(1.0/6)); CHECK(q.cells[0].values[1] == doctest::Approx(1/std::sqrt(2.0)));
}

TEST_CASE("Tetrahedral quality separates reversal collapse slivers and invalid nodes") {
    auto m = regular(true); auto& g = editMeshGeometry(m);
    constexpr std::array<size_t,10> order{0,2,1,3,6,5,4,7,9,8}; const auto original = g.tetrahedra[0].nodes;
    for (size_t i = 0; i < order.size(); ++i) { g.tetrahedra[0].nodes[i] = original[order[i]]; }
    const auto reversed = inspectTetrahedralQuality(m).cells[0]; CHECK(reversed.validity == TetrahedralValidity::reversed);
    CHECK(reversed.values[0] < 0); CHECK(reversed.values[1] == doctest::Approx(-1)); CHECK(reversed.values[2] == doctest::Approx(1)); CHECK(reversed.values[7] == doctest::Approx(1));
    auto flat = mesh({{0,0,0},{1,0,0},{0,1,0},{.25,.25,0}});
    const auto collapsed = inspectTetrahedralQuality(flat); CHECK(collapsed.cells[0].validity == TetrahedralValidity::collapsed);
    CHECK(collapsed.cells[0].values[0] == doctest::Approx(0)); CHECK(collapsed.validityFindings == std::vector<uint32_t>{0});
    CHECK(tetrahedralQualityDisplay(flat,collapsed).faces[0][0].values[0] == -1);
    auto sliver = mesh({{1,0,1e-5},{0,1,-1e-5},{-1,0,1e-5},{0,-1,-1e-5}});
    const auto c = inspectTetrahedralQuality(sliver).cells[0]; CHECK(c.values[2] < .01); CHECK(c.values[3] < 1); CHECK(c.values[4] > 179);
    editMeshGeometry(flat).tetrahedra[0].nodes[3] = 0;
    CHECK(inspectTetrahedralQuality(flat).cells[0].validity == TetrahedralValidity::repeatedNodes);
    editMeshGeometry(flat).tetrahedra[0].nodes[3] = 999;
    CHECK_THROWS_AS((void)inspectTetrahedralQuality(flat),std::invalid_argument);
}

TEST_CASE("Quadratic validity finds a fold missed by corner and center samples") {
    // det J = (r-.37)^2 - .01*(s+.01). Positive at all reference corners
    // and the center, but negative near r=.37, including inside the tet.
    const auto m = curved([](Coordinate p) -> Coordinate { const auto r = p[0], s = p[1]; return {.5*r*r-.37*r+.01*s,(r-.37)*s+.01*r,p[2]}; });
    for (size_t i = 0; i < 4; ++i) { const auto p = reference[i]; CHECK((p[0]-.37)*(p[0]-.37)-.01*(p[1]+.01) > 0); }
    CHECK((.25-.37)*(.25-.37)-.01*(.25+.01) > 0);
    const auto q = inspectTetrahedralQuality(m); CHECK(q.cells[0].validity == TetrahedralValidity::folded);
    CHECK(q.cells[0].values[5] < 0); CHECK(q.cells[0].values[6] > 0); CHECK(q.validityFindings.size() == 1);
}

TEST_CASE("Quadratic bounds prove curved validity and retain unresolved singularities") {
    const auto positive = curved([](Coordinate p) -> Coordinate { return {p[0],p[1],p[2]*(1+.25*p[0])}; });
    const auto q = inspectTetrahedralQuality(positive); CHECK(q.cells[0].validity == TetrahedralValidity::positive);
    // Integral of 1+.25*r over the reference tetrahedron.
    CHECK(q.cells[0].values[0] == doctest::Approx(17.0/96));
    CHECK(q.cells[0].values[5] > 0); CHECK(q.cells[0].values[7] <= .8+1e-12);
    const auto singular = curved([](Coordinate p) -> Coordinate { const auto r = p[0]; return {.5*r*r-.37*r,(r-.37)*p[1],p[2]}; });
    const auto uncertain = inspectTetrahedralQuality(singular);
    CHECK(uncertain.cells[0].validity == TetrahedralValidity::unresolved);
    CHECK(tetrahedralQualityDisplay(singular,uncertain).faces[0][0].values[0] == -2);
    std::stop_source stop; stop.request_stop(); CHECK_THROWS((void)inspectTetrahedralQuality(positive,stop.get_token()));
    CHECK_THROWS((void)tetrahedralQualityDisplay(positive,q,stop.get_token()));
}

TEST_CASE("Tetrahedral metrics retain double precision and scale invariance") {
    for (const double scale : {1e-4,1.0,1e4}) {
        auto m = regular(true); auto& g = editMeshGeometry(m);
        for (auto& p : g.precisePositions) { const auto old = p; p = {old[2]*scale+1e6,old[0]*scale-2e6,old[1]*scale+3e6}; }
        const auto q = inspectTetrahedralQuality(m).cells[0]; CHECK(q.validity == TetrahedralValidity::positive);
        CHECK(q.values[2] == doctest::Approx(1).epsilon(1e-5)); CHECK(q.values[1] == doctest::Approx(1).epsilon(1e-5));
        CHECK(q.values[0]/(scale*scale*scale) == doctest::Approx(std::sqrt(2.0)/12).epsilon(1e-5));
    }
}

TEST_CASE("Quadratic signed volume integrates cubic Jacobians") {
    constexpr double a = .2, b = .3, c = .4;
    const auto m = curved([](Coordinate p) -> Coordinate { return {p[0]+a*p[0]*p[0],p[1]+b*p[1]*p[1],p[2]+c*p[2]*p[2]}; });
    const auto q = inspectTetrahedralQuality(m).cells[0]; CHECK(q.validity == TetrahedralValidity::positive);
    CHECK(q.values[0] == doctest::Approx(1.0/6+(a+b+c)/12+(a*b+a*c+b*c)/30+a*b*c/90));
}

TEST_CASE("Quadratic subdivision proves a positive interior minimum") {
    // det J = (r-.37)^2 + .01 + .1*s. Root Bernstein coefficients
    // straddle zero, although the actual determinant stays positive.
    const auto m = curved([](Coordinate p) -> Coordinate {
        const auto r = p[0], s = p[1];
        return {.5*r*r-.37*r-.1*s,(r-.37)*s+.1*r,p[2]};
    });
    double scale = 0;
    const auto& positions = meshGeometry(m).precisePositions;
    for (const auto& a : positions) { for (const auto& b : positions) {
        scale = std::max(scale,std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]));
    }}
    const auto q = inspectTetrahedralQuality(m).cells[0];
    CHECK(q.validity == TetrahedralValidity::positive);
    CHECK(q.values[5] > 0);
    CHECK(q.values[5] <= .01/(scale*scale*scale));
    CHECK(q.values[6] >= (.63*.63+.01)/(scale*scale*scale));
}

TEST_CASE("Corner quality does not change when quadratic mid-edge nodes move") {
    auto m = regular(true); const auto original = inspectTetrahedralQuality(m).cells[0];
    editMeshGeometry(m).precisePositions[4][2] = 1e8;
    const auto changed = inspectTetrahedralQuality(m).cells[0];
    for (size_t i = 1; i <= 4; ++i) { CHECK(changed.values[i] == doctest::Approx(original.values[i])); }
}

TEST_CASE("Tetrahedral analysis retains nodes that are absent from the exterior") {
    Fixture fixture; UiState state; auto m = mesh({{0,0,0},{1,0,0},{0,1,0},{0,0,1},{.25,.25,.25}});
    auto& g = editMeshGeometry(m); g.indices.clear(); g.nodes[0].indexCount = 0; g.tetrahedra.clear();
    const std::array<std::array<uint32_t,4>,4> cells{{{0,1,2,4},{0,1,4,3},{0,4,2,3},{4,1,2,3}}};
    for (size_t i = 0; i < cells.size(); ++i) { TetrahedralCell cell; std::copy(cells[i].begin(),cells[i].end(),cell.nodes.begin()); cell.sourceCell = i+10; g.tetrahedra.push_back(cell); }
    appendTetrahedralBoundary(g); CHECK(std::find(g.indices.begin(),g.indices.end(),4) == g.indices.end()); publishMeshGeometry(m);
    state.files.push_back(createUiFileState(fixture.root/"star.vtk",m,0)); appendDefaultSceneNodesForFiles(state,0);
    setGroupScale(state.files[0].groupSettings[0],2); setGroupTranslation(state.files[0].groupSettings[0],{3,4,5});
    const auto id = createAnalysisFromObjects(state,AnalysisTask::meshQuality,{state.files[0].objectId});
    const auto world = comparisonWorldMesh(state,ComparisonSide::a,id); REQUIRE(meshGeometry(world).tetrahedra.size() == 4);
    CHECK(meshGeometry(world).vertices.size() == 5);
    const auto q = inspectTetrahedralQuality(world); CHECK(q.cells.size() == 4);
    double volume = 0; for (const auto& cell : q.cells) { volume += std::abs(cell.values[0]); }
    CHECK(volume == doctest::Approx(8.0/6));
    auto settings = comparisonSettings(state,id);
    CHECK((requestedComparisonStages(settings,false) & comparisonTetrahedralQuality) == 0);
    settings.tetrahedral.enabled = true;
    CHECK((requestedComparisonStages(settings,false) & comparisonTetrahedralQuality) != 0);
    CHECK((requestedComparisonStages(settings,false) & comparisonQuality) == 0);
    CHECK(nextComparisonStage(comparisonTetrahedralQuality) == comparisonTetrahedralQuality);
}

TEST_CASE("Cell quality survives analysis preparation and settings round trips") {
    Fixture fixture; const auto path = fixture.root/"mesh.vtk"; std::ofstream(path) << "fixture";
    UiState state; state.files.push_back(createUiFileState(path,regular(true),0)); appendDefaultSceneNodesForFiles(state,0);
    const auto id = createAnalysisFromObjects(state,AnalysisTask::meshQuality,{state.files[0].objectId}); REQUIRE(id != invalidSceneObjectId);
    auto settings = comparisonSettings(state,id); settings.tetrahedral.enabled = true; settings.tetrahedral.metric = TetrahedralQualityMetric::jacobianRatio;
    settings.tetrahedral.minimumEnabled = settings.tetrahedral.maximumEnabled = true; settings.tetrahedral.minimum = .8f; settings.tetrahedral.maximum = .7f; settings.tetrahedral.selectedOnly = true;
    const auto signature = comparisonGeometrySignature(state,id); setComparisonSettings(state,settings,id);
    CHECK(comparisonGeometrySignature(state,id) == signature); CHECK(comparisonSettings(state,id).tetrahedral.maximum == .8f);
    const auto snapshot = snapshotComparisonInputs(state,id); const auto world = comparisonWorldMesh(snapshot,ComparisonSide::a);
    REQUIRE(meshGeometry(world).tetrahedra.size() == 1); CHECK(meshGeometry(world).tetrahedra[0].sourceCell == 42);
    const auto result = computeComparisonStages(world,{},comparisonSource | comparisonTetrahedralQuality);
    REQUIRE(result.original.tetrahedralQuality); CHECK(result.original.tetrahedralQuality->cells[0].partId == state.files[0].groupSettings[0].objectId);
    selectTetrahedralCell(state,world,signature,0,id); REQUIRE(findComparison(state,id)->tetrahedralFocus);
    CHECK(findComparison(state,id)->tetrahedralFocus->index == 0);
    const auto document = createSceneDocument(state); const auto scenePath = fixture.root/"scene.woby"; writeSceneDocument(scenePath,document);
    CHECK(readSceneDocument(scenePath).comparisons == document.comparisons);
    const auto clone = duplicateComparison(state,id); CHECK_FALSE(findComparison(state,clone)->tetrahedralFocus);
    CHECK(comparisonSettings(state,clone).tetrahedral == comparisonSettings(state,id).tetrahedral);
    const auto limits = tetrahedralQualityOutsideLimits(*result.original.tetrahedralQuality,comparisonSettings(state,id).tetrahedral); CHECK(limits[0] == 0); CHECK(limits[1] == 1);
    ComparisonCacheStatus cache; resetComparisonCache(cache,signature);
    MeshComparison applied; CHECK_FALSE(applyComparisonStages(applied,cache,result,signature+1,comparisonTetrahedralQuality));
    CHECK(applyComparisonStages(applied,cache,result,signature,comparisonTetrahedralQuality)); REQUIRE(applied.original.tetrahedralQuality);
}

TEST_CASE("Tetrahedral quality JSON pages and full exports retain cell results") {
    const auto m = regular(true); auto result = computeComparisonStages(m,{},comparisonSource | comparisonTetrahedralQuality);
    const auto page = analysisResultPage(result,ComparisonSide::a,"tetrahedral_quality","/cells",0,100);
    CHECK(page["total"] == 1); CHECK(page["items"][0]["sourceCell"] == 43); CHECK(page["items"][0]["validity"] == "positive");
    const auto summary = controlComparisonResults(result,0,false,[](uint64_t id) { return std::to_string(id); });
    CHECK(summary["aToB"]["tetrahedralMeshQuality"]["cellCount"] == 1);
    std::ostringstream stream; writeAnalysisResults(stream,result,summary);
    const auto full = nlohmann::json::parse(stream.str()); CHECK(full["aToB"]["tetrahedralCells"][0] == page["items"][0]);
    const auto operation = parseControlOperation(*findControlMethod("analysis.set"),{{"target","1"},{"tetrahedral",true},{"tetrahedralMetric","minimum_dihedral"},
        {"tetrahedralMinimumEnabled",true},{"tetrahedralMaximumEnabled",false},{"tetrahedralMinimum",5},{"tetrahedralMaximum",175},{"tetrahedralSelectedOnly",true}});
    CHECK(operation.tetrahedral == true); CHECK(operation.tetrahedralMetric == "minimum_dihedral"); CHECK(operation.tetrahedralMinimum == 5);
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.set"),{{"target","1"},{"tetrahedralMetric","unknown"}}));
    TetrahedralQualitySettings invalid; invalid.metric = static_cast<TetrahedralQualityMetric>(99); invalid.minimum = std::numeric_limits<float>::quiet_NaN();
    CHECK(normalizedTetrahedralQualitySettings(invalid).metric == TetrahedralQualityMetric::meanRatio); CHECK(normalizedTetrahedralQualitySettings(invalid).minimum == .2f);
}
