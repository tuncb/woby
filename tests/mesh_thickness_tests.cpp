#include "mesh_thickness.h"
#include "mesh_comparison.h"
#include "comparison_runtime.h"
#include "comparison_scene.h"
#include "analysis_results.h"
#include "control_scene.h"
#include "ui_operations.h"
#include "scene_history.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace {
using namespace woby;
using Point = std::array<double,3>;
void box(SourceMeshData& data, Point low, Point high, bool reverse = false)
{
    auto& positions = editSourcePositions(data); const auto first = static_cast<uint32_t>(positions.size());
    for (const Point p : {Point{low[0],low[1],low[2]},Point{high[0],low[1],low[2]},Point{high[0],high[1],low[2]},Point{low[0],high[1],low[2]},
            Point{low[0],low[1],high[2]},Point{high[0],low[1],high[2]},Point{high[0],high[1],high[2]},Point{low[0],high[1],high[2]}}) { positions.push_back(p); }
    const std::array<uint32_t,36> indices{0,2,1,0,3,2,4,5,6,4,6,7,0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7};
    for (size_t i = 0; i < indices.size(); i += 3) { for (size_t k = 0; k < 3; ++k) { data.indices.push_back(first+indices[i+(reverse ? 2-k : k)]); } }
}
DuplicateSource source(std::shared_ptr<SourceMeshData> data)
{
    SourcePartInstance part; part.partId = 2; part.indexCount = data->indices.size(); part.transform = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    data->provenance = SourceProvenance::objPositions; return {1,"thickness.obj",std::move(data),true,{part}};
}
DuplicateSource plate(double thickness = 1)
{
    auto data = std::make_shared<SourceMeshData>(); box(*data,{0,0,0},{20,20,thickness}); return source(data);
}
DuplicateSource web()
{
    auto data = std::make_shared<SourceMeshData>();
    const std::array<double,4> x{0,10,12,22}, half{5,5,.5,.5};
    for (size_t i = 0; i < x.size(); ++i) { for (const Point p : {Point{x[i],0,-half[i]},Point{x[i],10,-half[i]},Point{x[i],10,half[i]},Point{x[i],0,half[i]}}) { editSourcePositions(*data).push_back(p); } }
    const auto quad = [&](uint32_t a,uint32_t b,uint32_t c,uint32_t d) { data->indices.insert(data->indices.end(),{a,b,c,a,c,d}); };
    quad(3,2,1,0); quad(12,13,14,15);
    for (uint32_t i = 0; i < 3; ++i) { for (uint32_t k = 0; k < 4; ++k) { quad(4*i+k,4*i+(k+1)%4,4*(i+1)+(k+1)%4,4*(i+1)+k); } }
    return source(data);
}
Mesh meshFor(const DuplicateSource& src)
{
    Mesh mesh;
    for (const auto& p : sourcePositions(*src.data)) { Vertex v; for (size_t k = 0; k < 3; ++k) { v.position[k] = static_cast<float>(p[k]); } editMeshGeometry(mesh).vertices.push_back(v); }
    editMeshGeometry(mesh).indices = src.data->indices;
    mesh.bounds = calculateBounds(meshGeometry(mesh).vertices);
    auto input = std::make_shared<DuplicateInput>(); input->sources.push_back(src); mesh.duplicateInput = std::move(input); return mesh;
}
ThicknessSettings absolute(double maximum = 1.01)
{
    ThicknessSettings settings; settings.absoluteEnabled = true; settings.relativeEnabled = false; settings.maximumThickness = static_cast<float>(maximum); return settings;
}
struct Files {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())/ ("woby-thickness-"+std::to_string(std::random_device{}())+"-"+std::to_string(std::random_device{}()));
    Files() { std::filesystem::create_directories(root); }
    ~Files() { std::error_code ignored; std::filesystem::remove_all(root,ignored); }
};
}

TEST_CASE("thickness measures plates and joins opposite faces into a region")
{
    const auto measured = measureMeshThickness({plate()});
    REQUIRE(measured->availableSources == 1); REQUIRE(measured->sources[0].faces.size() == 12);
    for (size_t f = 0; f < 4; ++f) {
        const auto& value = measured->sources[0].faces[f];
        CHECK(value.minimum == doctest::Approx(1)); CHECK(value.typical == doctest::Approx(1)); CHECK(value.confidence >= 3.0/7);
        CHECK(std::hypot(value.to[0]-value.from[0],value.to[1]-value.from[1],value.to[2]-value.from[2]) == doctest::Approx(1));
    }
    const auto result = filterThinRegions(measured,absolute());
    REQUIRE(result.regions.size() == 1); const auto& region = result.regions.front();
    CHECK(region.faces.size() == 4); CHECK(region.area == doctest::Approx(800));
    CHECK(region.suggestedSize == doctest::Approx(1.0/3));
    auto filters = absolute(); filters.maximumThickness = .9f;
    const auto filtered = filterThinRegions(measured,filters); CHECK(filtered.measurements == measured); CHECK(filtered.regions.empty());
    filters = absolute(); filters.minimumRegionArea = 801; CHECK(filterThinRegions(measured,filters).regions.empty());
}

TEST_CASE("thickness finds a thin web relative to the thick body")
{
    const auto measured = measureMeshThickness({web()});
    REQUIRE(measured->availableSources == 1);
    const auto result = filterThinRegions(measured,{});
    REQUIRE_FALSE(result.regions.empty());
    // Rays from the end cap can cross the taper as well as the 10-unit body.
    CHECK(measured->sources[0].reference >= 10); CHECK(measured->sources[0].reference < 22);
    for (const auto& region : result.regions) {
        CHECK(region.minimum == doctest::Approx(1)); CHECK(region.relativeThickness <= .2);
        CHECK(region.minimumBound[0] >= 10); CHECK(region.maximumBound[0] == 22);
    }
    auto filters = absolute(.9); filters.relativeEnabled = true;
    CHECK(filterThinRegions(measured,filters).regions.size() == result.regions.size()); // Either enabled limit.
}

TEST_CASE("thickness handles reversed winding nested cavities and separate solids")
{
    for (const bool reverse : {false,true}) {
        auto data = std::make_shared<SourceMeshData>(); box(*data,{0,0,0},{10,10,10},reverse); box(*data,{1,1,1},{9,9,9},!reverse);
        const auto measured = measureMeshThickness({source(data)});
        REQUIRE(measured->availableSources == 1);
        for (const auto& face : measured->sources[0].faces) { CHECK(face.minimum == doctest::Approx(1)); }
    }
    auto data = std::make_shared<SourceMeshData>(); box(*data,{0,0,0},{20,20,1}); box(*data,{0,0,1.1},{20,20,2.1},true);
    const auto measured = measureMeshThickness({source(data)});
    REQUIRE(measured->availableSources == 1);
    const auto result = filterThinRegions(measured,absolute()); REQUIRE(result.regions.size() == 2);
    for (const auto& region : result.regions) { CHECK(region.minimum == doctest::Approx(1)); }
}

TEST_CASE("thickness rejects open conflicting and intersecting boundaries")
{
    for (const int defect : {0,1,2,3,4}) {
        auto data = std::make_shared<SourceMeshData>(); box(*data,{0,0,0},{20,20,1});
        if (defect == 0) { data->indices.resize(data->indices.size()-3); }
        if (defect == 1) { std::swap(data->indices[0],data->indices[1]); }
        if (defect == 2) { box(*data,{.5,.5,.5},{2,2,2}); }
        if (defect == 3) { data->indices.insert(data->indices.end(),{0,0,0}); }
        if (defect == 4) { box(*data,{20,20,1},{21,21,2}); }
        const auto measured = measureMeshThickness({source(data)});
        CHECK(measured->unavailableSources == 1); CHECK(measured->measuredFaces == 0);
        REQUIRE(measured->sources.size() == 1); CHECK_FALSE(measured->sources[0].unavailableReason.empty());
        CHECK(filterThinRegions(measured,absolute()).regions.empty());
    }
    auto unavailable = plate(); unavailable.data.reset();
    CHECK(measureMeshThickness({unavailable})->unavailableSources == 1);
}

TEST_CASE("thickness uses source doubles exact seams transforms and separate batches")
{
    for (const double scale : {1e-6,1.0,1e6}) {
        auto src = plate(); src.parts[0].transform[0] = src.parts[0].transform[5] = src.parts[0].transform[10] = scale;
        src.parts[0].transform[12] = 1e5*scale;
        const auto measured = measureMeshThickness({src}); REQUIRE(measured->availableSources == 1);
        CHECK(measured->sources[0].faces[0].minimum == doctest::Approx(scale));
    }
    auto split = plate(); auto data = std::make_shared<SourceMeshData>();
    for (const auto index : split.data->indices) { data->indices.push_back(static_cast<uint32_t>(sourcePositions(*data).size())); editSourcePositions(*data).push_back(sourcePositions(*split.data)[index]); }
    split = source(data); CHECK(measureMeshThickness({split})->availableSources == 1);
    editSourcePositions(*data).push_back({1e30,1e30,1e30});
    CHECK(measureMeshThickness({split})->sources[0].faces[0].minimum == doctest::Approx(1));
    auto a = plate(), b = plate(); b.fileId = 9;
    const auto measured = measureMeshThickness({a,b}); CHECK(measured->availableSources == 2);
    CHECK(filterThinRegions(measured,absolute()).regions.size() == 2);
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS((void)measureMeshThickness({a},stop.get_token()));
    CHECK_THROWS((void)filterThinRegions(measured,absolute(),stop.get_token()));
}

TEST_CASE("thickness stage caches measurements and publishes only current results")
{
    const auto mesh = meshFor(plate());
    const auto stage = comparisonSource | comparisonThickness;
    auto update = computeComparisonStages(mesh,{},stage,{}, {},TopologyMode::originalIndex,{}, {},topologyAllFindings,{},absolute());
    REQUIRE(update.original.thickness.regions.size() == 1);
    CHECK(update.original.thinRegionBounds.size() == 1); CHECK(update.original.thinRegionFill.size() == 12);
    REQUIRE(update.original.thinRegionFillRanges.size() == 1);
    CHECK(update.original.thinRegionFillRanges[0] == std::array<uint32_t,2>{0,12});
    CHECK(update.original.thicknessMap.size() == 36);
    const auto measurements = update.original.thickness.measurements;
    ComparisonCacheStatus cache{7,0}; cache.thickness = absolute(); MeshComparison result;
    CHECK_FALSE(applyComparisonStages(result,cache,update,8,stage));
    REQUIRE(applyComparisonStages(result,cache,std::move(update),7,stage));
    CHECK(result.original.thinRegionFillRanges[0] == std::array<uint32_t,2>{0,12});
    auto filters = absolute(.5); CHECK(resetComparisonThicknessCache(cache,filters));
    CHECK_FALSE(applyComparisonStages(result,cache,result,7,comparisonThickness));
    auto filtered = computeComparisonStages(mesh,{},comparisonThickness,{}, {},TopologyMode::originalIndex,{},comparisonAnalysisInputs(result),0,{},filters);
    CHECK(filtered.original.thickness.measurements == measurements); CHECK(filtered.original.thickness.regions.empty());
    REQUIRE(applyComparisonStages(result,cache,std::move(filtered),7,comparisonThickness));
    CHECK(result.original.thinRegionFillRanges.empty());
    ComparisonRuntime runtime; runtime.results.signature = 7; runtime.results.cache = cache; runtime.results.value = result;
    ComparisonSettings settings; settings.thickness = filters;
    CHECK(comparisonDetectorReady(runtime,settings,7,DiagnosticCategory::thinRegions));
    settings.thickness.maximumThickness = 2; CHECK_FALSE(comparisonDetectorReady(runtime,settings,7,DiagnosticCategory::thinRegions));
    filters.show = false; filters.showMap = true; CHECK_FALSE(resetComparisonThicknessCache(cache,filters));
}

TEST_CASE("thickness settings persist in scenes and views and validate through the control API")
{
    Files files; UiState state; const auto id = createComparison(state);
    const auto params = nlohmann::json{{"target","analysis"},{"thinRegions",true},{"showThinRegions",false},{"thicknessMap",true},
        {"thicknessAbsoluteEnabled",true},{"thicknessRelativeEnabled",false},{"maximumThickness",.25},
        {"maximumRelativeThickness",.125},{"minimumThinRegionArea",5},{"thicknessElements",4}};
    auto command = parseControlOperation(*findControlMethod("analysis.set"),params); command.objectId = id;
    CHECK(controlOperationParams(command)["thicknessElements"] == 4);
    (void)applyControlSceneOperation(state,createSceneDocument(state),command,[](auto value) { return std::to_string(value); },200,800);
    auto settings = comparisonSettings(state,id); settings.diagnosticCategory = DiagnosticCategory::thinRegions; setComparisonSettings(state,settings,id);
    CHECK(settings.thickness.maximumThickness == .25f); CHECK(settings.thickness.elementsThroughThickness == 4);
    auto document = createSceneDocument(state);
    SceneViewRecord view; view.name = "Review";
    SceneViewObjectRecord object; object.kind = ViewObjectKind::comparison; object.index = 0; object.settings.comparison = settings;
    view.objects.push_back(object); document.views.push_back(view);
    const auto path = files.root/"analysis.woby"; writeSceneDocument(path,document);
    const auto loaded = readSceneDocument(path);
    REQUIRE(loaded.comparisons.size() == 1); CHECK(loaded.comparisons[0].settings == settings);
    REQUIRE(loaded.views.size() == 1); CHECK(loaded.views[0].objects[0].settings.comparison == settings);
    CHECK_THROWS((void)parseControlOperation(*findControlMethod("analysis.set"),{{"target","analysis"},{"thicknessElements",1.5}}));
    auto invalid = settings.thickness; invalid.maximumThickness = -1; invalid.maximumRelativeThickness = 2; invalid.elementsThroughThickness = 0;
    invalid.minimumRegionArea = std::numeric_limits<float>::infinity(); invalid = normalizedThicknessSettings(invalid);
    CHECK(invalid.maximumThickness == 0); CHECK(invalid.maximumRelativeThickness == 1); CHECK(invalid.elementsThroughThickness == 1); CHECK(invalid.minimumRegionArea == 0);
    setComparisonDiagnosticsVisible(state,false,id); CHECK(countVisibleComparisonDiagnostics(comparisonSettings(state,id)) == 0);
    setComparisonDiagnosticsVisible(state,true,id); CHECK(countVisibleComparisonDiagnostics(comparisonSettings(state,id)) == diagnosticCategoryCount);
}

TEST_CASE("thickness exports measurements source references and complete nested findings")
{
    auto result = computeComparisonStages(meshFor(plate()),{},comparisonSource | comparisonThickness,{}, {},TopologyMode::originalIndex,{}, {},0,{},absolute());
    auto summary = analysisDetectorSummary(result,ComparisonSide::a,"thin_regions");
    CHECK(summary["algorithm"] == "woby-thickness-rays-v1"); CHECK(summary["knownCount"] == 1);
    CHECK(summary["findings"][0]["minimumThickness"].get<double>() == doctest::Approx(1));
    auto& region = result.original.thickness.regions[0]; region.faces.resize(125,region.faces.front());
    CHECK(analysisDetectorSummary(result,ComparisonSide::a,"thin_regions")["findings"][0]["facesTruncated"] == true);
    const auto page = analysisResultPage(result,ComparisonSide::a,"thin_regions","/findings/0/faces",100,100);
    CHECK(page["items"].size() == 25); CHECK(page["items"][0]["partId"] == "2");
    std::ostringstream out; writeAnalysisResults(out,result,{{"aToB",{{"detectors",nlohmann::json::object()}}}});
    const auto json = nlohmann::json::parse(out.str());
    CHECK(json["aToB"]["detectors"]["thin_regions"]["findings"][0]["faces"].size() == 125);
}

TEST_CASE("thickness background export retains both sides after live results are replaced")
{
    Files files;
    AnalysisExportRuntime runtime;
    auto result = computeComparisonStages(meshFor(plate()),meshFor(plate(2)),comparisonSource | comparisonThickness,
        {}, {},TopologyMode::originalIndex,{}, {},0,{},absolute(2.1));
    REQUIRE(result.original.thickness.regions.size() == 1);
    REQUIRE(result.repaired.thickness.regions.size() == 1);
    result.original.thickness.coordinateOrigin = {100,200,300};
    auto& region = result.original.thickness.regions[0];
    region.faces.resize(125,region.faces.front());
    const nlohmann::json summary{{"aToB",{{"detectors",nlohmann::json::object()}}},
        {"bToA",{{"detectors",nlohmann::json::object()}}}};
    std::ostringstream expected;
    writeAnalysisResults(expected,result,summary);
    const auto path = files.root/"thickness.json";
    startAnalysisExport(runtime,result,summary,"thickness",path);
    result = {}; // The background job must own its regions and immutable measurements.
    runtime.worker.wait();
    REQUIRE(analysisExportStatus(runtime)["state"] == "complete");
    std::ifstream input(path);
    const auto exported = nlohmann::json::parse(input);
    CHECK(exported == nlohmann::json::parse(expected.str()));
    const auto& a = exported["aToB"]["detectors"]["thin_regions"];
    const auto& b = exported["bToA"]["detectors"]["thin_regions"];
    CHECK(a["knownCount"] == 1); CHECK(b["knownCount"] == 1);
    CHECK(a["findings"][0]["faces"].size() == 125);
    CHECK(a["findings"][0]["minimumThickness"].get<double>() == doctest::Approx(1));
    CHECK(b["findings"][0]["minimumThickness"].get<double>() == doctest::Approx(2));
}
