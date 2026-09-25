#include "mesh_intersections.h"
#include "mesh_comparison.h"
#include "comparison_scene.h"
#include "comparison_view.h"
#include <thread>
#include <functional>
#include "comparison_report.h"
#include "control_scene.h"
#include "obj_mesh.h"
#include "scene_history.h"
#include "scene_pick.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <set>

namespace {
using namespace woby;
using Point = std::array<double, 3>;
using Triangle = std::array<Point, 3>;
const Triangle base = {{{0,0,0}, {2,0,0}, {0,2,0}}};
const Triangle crossing = {{{.5,.5,-1}, {.5,.5,1}, {1.5,.5,0}}};
bool hit(const Triangle& a, const Triangle& b, std::array<size_t, 3> bIds = {3,4,5})
{
    return trianglesSelfIntersect(a,b,{0,1,2},bIds);
}
DuplicateSource sourceFor(std::vector<Point> points, std::vector<uint32_t> indices)
{
    auto data = std::make_shared<SourceMeshData>();
    data->points = std::move(points); data->indices = std::move(indices); data->provenance = SourceProvenance::objPositions;
    SourcePartInstance part; part.partId = 2; part.indexCount = data->indices.size();
    part.transform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    return {1,"test.obj",data,true,{part}};
}
DuplicateSource crossingSource()
{
    return sourceFor({base[0],base[1],base[2],crossing[0],crossing[1],crossing[2]}, {0,1,2,3,4,5});
}
Mesh meshFor(const DuplicateSource& source)
{
    Mesh mesh;
    for (const auto& p : source.data->points) {
        Vertex v; for (size_t k = 0; k < 3; ++k) { v.position[k] = static_cast<float>(p[k]); } mesh.vertices.push_back(v);
    }
    mesh.indices = source.data->indices; mesh.bounds = calculateBounds(mesh.vertices);
    auto input = std::make_shared<DuplicateInput>(); input->sources.push_back(source); mesh.duplicateInput = input;
    return mesh;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-intersections-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path write(const char* name, const std::string& text) const {
        const auto path = root/name; std::ofstream out(path); out << text; return path;
    }
};
}

TEST_CASE("exact intersections distinguish crossings coplanar overlap and shared features")
{
    CHECK(hit(base, crossing));
    CHECK(hit(crossing, base));
    CHECK(hit(base, base)); // Coincident faces, different IDs.
    CHECK(hit(base, base, {0,1,2})); // Source-ID duplicates also overlap.
    CHECK(hit(base, Triangle{{{.25,.25,0},{.5,.25,0},{.25,.5,0}}})); // Contained.
    CHECK_FALSE(hit(base, Triangle{{{3,0,0},{4,0,0},{3,1,0}}}));
    const Triangle neighbor = {{{0,0,0},{2,0,0},{0,-2,0}}};
    CHECK_FALSE(hit(base, neighbor, {0,1,3}));
    CHECK(hit(base, neighbor)); // Geometric contact with distinct topology.
    CHECK_FALSE(hit(base, Triangle{{{0,0,0},{2,0,0},{0,0,2}}}, {0,1,3}));
    CHECK(hit(base, Triangle{{{0,0,0},{2,0,0},{1,1,0}}}, {0,1,3})); // Beyond shared edge.
    CHECK_FALSE(hit(base, Triangle{{{0,0,0},{-1,0,1},{0,-1,1}}}, {0,3,4}));
    CHECK(hit(base, Triangle{{{0,0,0},{1,1,-1},{1,1,1}}}, {0,3,4})); // Beyond shared vertex.
    CHECK(hit(base, Triangle{{{1,0,0},{1,-1,1},{1,-1,-1}}})); // T contact.
    auto near = base;
    for (auto& p : near) { p[2] = std::numeric_limits<double>::denorm_min(); }
    CHECK_FALSE(hit(base, near)); // Never a user epsilon.
    CHECK_FALSE(hit(base, Triangle{{{0,0,0},{1,0,0},{2,0,0}}}));
    auto invalid = base; invalid[0][0] = std::numeric_limits<double>::infinity();
    CHECK_THROWS_AS(hit(base, invalid), std::invalid_argument);
}

TEST_CASE("exact intersection decisions survive permutations scales and near collinearity")
{
    for (const double scale : {1e-150, 1.0, 1e150}) {
        auto a = base, b = crossing;
        for (auto* triangle : {&a,&b}) { for (auto& p : *triangle) { for (auto& v : p) { v *= scale; } } }
        for (size_t i = 0; i < 3; ++i) {
            std::rotate(a.begin(), a.begin()+1, a.end());
            for (size_t j = 0; j < 3; ++j) {
                std::rotate(b.begin(), b.begin()+1, b.end()); CHECK(hit(a,b));
                std::swap(b[0],b[1]); CHECK(hit(a,b)); std::swap(b[0],b[1]);
            }
        }
    }
    const double tiny = std::numeric_limits<double>::denorm_min();
    CHECK_FALSE(exactTriangleCollapsed(Triangle{{{0,0,0},{tiny,0,0},{0,tiny,0}}}));
    CHECK(exactTriangleCollapsed(Triangle{{{0,0,0},{tiny,tiny,0},{2*tiny,2*tiny,0}}}));
}

TEST_CASE("intersection source scope topology capabilities transforms and cancellation")
{
    const auto source = crossingSource();
    auto topology = buildMeshTopology({source});
    const auto result = inspectIntersections(topology);
    REQUIRE(result.findings.size() == 1); CHECK(result.affectedFaces == 2);
    CHECK(result.findings[0].faces[0] == TopologyFaceReference{1,2,0});
    CHECK(result.findings[0].faces[1] == TopologyFaceReference{1,2,1});
    auto a = source, b = source; a.parts[0].indexCount = 3; b.parts[0].firstIndex = 3; b.parts[0].indexCount = 3; b.fileId = 3;
    CHECK(inspectIntersections(buildMeshTopology({a,b})).findings.empty());
    a.parts.push_back(source.parts[0]); a.parts[1].partId = 4; a.parts[1].firstIndex = 3; a.parts[1].indexCount = 3;
    CHECK(inspectIntersections(buildMeshTopology({a})).findings.size() == 1);
    a.parts[1].transform[12] = 20;
    CHECK(inspectIntersections(buildMeshTopology({a})).findings.empty());
    auto stl = source; auto data = std::make_shared<SourceMeshData>(*source.data); data->provenance = SourceProvenance::stlCorners; stl.data = data;
    CHECK(inspectIntersections(buildMeshTopology({stl})).findings.size() == 1);
    CHECK(std::string(intersectionStatus(inspectIntersections(buildMeshTopology({stl},TopologyMode::originalIndex)))) == "unavailable");
    const auto seam = sourceFor({{0,0,0},{2,0,0},{0,2,0},{0,0,0},{2,0,0},{0,-2,0}}, {0,1,2,3,4,5});
    CHECK(inspectIntersections(buildMeshTopology({seam})).findings.size() == 1);
    CHECK(inspectIntersections(buildMeshTopology({seam},TopologyMode::exactPosition)).findings.empty());
    auto collapsed = sourceFor({{0,0,0},{1,0,0},{2,0,0}}, {0,1,2});
    CHECK(inspectIntersections(buildMeshTopology({collapsed})).excludedCollapsedFaces == 1);
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS((void)inspectIntersections(topology, {true,true}, stop.get_token()));
    CHECK(std::string(intersectionStatus(inspectIntersections(topology, {false,true}))) == "complete");
}

TEST_CASE("intersection BVH agrees with analytic layered triangles and bounds dense output")
{
    // Parallel triangles intersect iff their integer layer matches. This oracle
    // is independent of both the BVH and the triangle intersection implementation.
    std::vector<Point> points; std::vector<uint32_t> indices;
    std::vector<size_t> layers;
    std::mt19937 random(73);
    for (uint32_t i = 0; i < 48; ++i) {
        const size_t layer = random()%12; layers.push_back(layer);
        for (auto p : base) { p[2] = static_cast<double>(layer); points.push_back(p); }
        indices.insert(indices.end(), {3*i,3*i+1,3*i+2});
    }
    const auto topology = buildMeshTopology({sourceFor(points,indices)});
    const auto result = inspectIntersections(topology);
    std::set<std::pair<size_t,size_t>> expected, actual;
    for (size_t i = 0; i < layers.size(); ++i) { for (size_t j = i+1; j < layers.size(); ++j) { if (layers[i] == layers[j]) { expected.emplace(i,j); } } }
    for (const auto& f : result.findings) { actual.emplace(f.faces[0].triangleId,f.faces[1].triangleId); }
    CHECK(actual == expected); CHECK_FALSE(result.truncated);
    const auto limited = inspectIntersections(topology, {true,true}, {}, {3,10000});
    CHECK(limited.findings.size() == 3); CHECK(limited.truncated);
    CHECK(std::string(intersectionStatus(limited)) == "partial");
    const auto workLimit = inspectIntersections(topology, {true,true}, {}, {10000,2});
    CHECK(workLimit.candidateTests == 2); CHECK(workLimit.truncated);
    CHECK(limited.truncationReason == "pair_limit"); CHECK(workLimit.truncationReason == "candidate_limit");
    const auto unlimited = inspectIntersections(topology, {true,true}, {}, {0,0});
    CHECK_FALSE(unlimited.truncated); CHECK(unlimited.findings.size() == expected.size());

}

TEST_CASE("exact intersection clipping agrees with an independent integer separating axis oracle")
{
    // Integer projections are exact at this coordinate range. SAT uses neither
    // segment clipping nor the production rational arithmetic implementation.
    using P = std::array<int64_t,3>;
    const auto sub = [](const P& a, const P& b) -> P { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; };
    const auto crossProduct = [](const P& a, const P& b) -> P { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; };
    const auto dotProduct = [](const P& a, const P& b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; };
    std::mt19937 random(291);
    for (size_t trial = 0; trial < 300; ++trial) {
        std::array<P,3> a{},b{},ae{},be{}; Triangle ap{},bp{};
        for (size_t i = 0; i < 3; ++i) {
            for (size_t k = 0; k < 3; ++k) {
                a[i][k] = static_cast<int64_t>(random()%9)-4; b[i][k] = static_cast<int64_t>(random()%9)-4;
                if (trial%2 == 0 && k == 2) { a[i][k] = b[i][k] = 0; }
                ap[i][k] = static_cast<double>(a[i][k]); bp[i][k] = static_cast<double>(b[i][k]);
            }
        }
        for (size_t i = 0; i < 3; ++i) { ae[i] = sub(a[(i+1)%3],a[i]); be[i] = sub(b[(i+1)%3],b[i]); }
        const auto an = crossProduct(ae[0],ae[1]), bn = crossProduct(be[0],be[1]);
        bool expected = an != P{} && bn != P{};
        std::vector<P> axes{an,bn};
        for (size_t i = 0; i < 3; ++i) {
            axes.push_back(crossProduct(an,ae[i])); axes.push_back(crossProduct(bn,be[i]));
            for (size_t j = 0; j < 3; ++j) { axes.push_back(crossProduct(ae[i],be[j])); }
        }
        for (const auto& axis : axes) {
            auto amin = dotProduct(axis,a[0]), amax = amin, bmin = dotProduct(axis,b[0]), bmax = bmin;
            for (size_t i = 1; i < 3; ++i) {
                amin = std::min(amin,dotProduct(axis,a[i])); amax = std::max(amax,dotProduct(axis,a[i]));
                bmin = std::min(bmin,dotProduct(axis,b[i])); bmax = std::max(bmax,dotProduct(axis,b[i]));
            }
            if (amax < bmin || bmax < amin) { expected = false; }
        }
        CAPTURE(trial);
        CHECK(hit(ap,bp) == expected);
    }
}

TEST_CASE("intersection stages preserve other results and reject stale topology")
{
    const auto mesh = meshFor(crossingSource());
    ComparisonSettings settings; settings.intersections.autoUpdate = true;
    const auto stages = requestedComparisonStages(settings,false);
    CHECK((stages & comparisonIntersections) != 0);
    ComparisonCacheStatus cache{7,0}; MeshComparison result;
    REQUIRE(applyComparisonStages(result,cache,computeComparisonStages(mesh,{},stages),7,stages));
    REQUIRE(result.original.intersections.findings.size() == 1);
    const auto* pairs = result.original.intersections.findings.data();
    settings.intersections.show = false; CHECK(requestedComparisonStages(settings,false) == stages);
    setComparisonIntersectionSettings(result,{false,false});
    auto json = controlComparisonResults(result,.05)["aToB"]["detectors"]["self_intersections"];
    CHECK(json["status"] == "complete"); CHECK(json["count"] == 1); CHECK(json["findings"].size() == 1);
    setComparisonIntersectionSettings(result,{true,true}); CHECK(result.original.intersections.findings.data() == pairs);
    json = controlComparisonResults(result,.05)["aToB"]["detectors"]["self_intersections"];
    CHECK(json["count"] == 1); CHECK(json["affectedFaceCount"] == 2); CHECK(json["findings"][0]["faces"][1]["triangleId"] == 2);
    result.original.intersections.unavailableSources = 1;
    json = controlComparisonResults(result,.05)["aToB"]["detectors"]["self_intersections"];
    CHECK(json["status"] == "partial"); CHECK(json["count"].is_null());
    result.original.intersections.unavailableSources = 0;
    result.original.intersections.findings.resize(101, result.original.intersections.findings.front());
    json = controlComparisonResults(result,.05)["aToB"]["detectors"]["self_intersections"];
    CHECK(json["count"] == 101); CHECK(json["findings"].size() == 100); CHECK(json["findingsTruncated"] == true);
    CHECK(json["detectionTruncated"] == false);
    result.original.intersections.findings.resize(1);
    result.original.intersections.truncated = true;
    json = controlComparisonResults(result,.05)["aToB"]["detectors"]["self_intersections"];
    CHECK(json["count"].is_null()); CHECK(json["affectedFaceCount"].is_null()); CHECK(json["knownCount"] == 1);
    CHECK(resetComparisonTopologyCache(cache,TopologyMode::exactPosition));
    CHECK((cache.completed & comparisonIntersections) == 0);
    CHECK_FALSE(applyComparisonStages(result,cache,computeComparisonStages(mesh,{},comparisonIntersections),7,comparisonIntersections));
    CHECK((cache.completed & comparisonSource) != 0);
}

TEST_CASE("intersection scene CLI navigation and reports preserve settings")
{
    Fixture fixture;
    const auto path = fixture.write("crossing.obj", "v 0 0 0\nv 2 0 0\nv 0 2 0\nv .5 .5 -1\nv .5 .5 1\nv 1.5 .5 0\nf 1 2 3\nf 4 5 6\n");
    UiState state; state.files.push_back(createUiFileState(path,loadObjMesh(path),0)); appendDefaultSceneNodesForFiles(state,0);
    const auto id = createComparison(state); setComparisonObjects(state,{state.files[0].objectId},ComparisonSide::a,true,id);
    auto command = parseControlOperation(*findControlMethod("analysis.set"), {{"target","analysis"},{"selfIntersections",true},{"showSelfIntersections",false}});
    CHECK(controlOperationParams(command)["selfIntersections"] == true); command.objectId = id;
    (void)applyControlSceneOperation(state,createSceneDocument(state),command,[](auto value){return std::to_string(value);},200,800);
    auto settings = comparisonSettings(state,id); CHECK(settings.intersections.autoUpdate); CHECK_FALSE(settings.intersections.show);
    settings.intersections.limits = {12345, 0};
    settings.diagnosticCategory = DiagnosticCategory::selfIntersections; setComparisonSettings(state,settings,id);
    setComparisonTranslation(state,id,{100,0,0});
    const auto signature = comparisonGeometrySignature(state,id);
    const auto result = computeComparisonStages(comparisonWorldMesh(state,ComparisonSide::a,id),{},requestedComparisonStages(settings,false));
    REQUIRE(result.original.intersections.findings.size() == 1);
    CHECK(result.original.intersections.findings[0].geometry[0][0] == 0);
    selectComparisonDiagnostic(state,result,signature,0,id);
    CHECK(findComparison(state,id)->diagnosticFocus.has_value());
    CHECK(focusedComparisonDiagnostic(state,result,signature,id) != nullptr);
    auto changed = settings; changed.topologyMode = TopologyMode::exactPosition; setComparisonSettings(state,changed,id);
    selectComparisonDiagnostic(state,result,signature,0,id); CHECK_FALSE(findComparison(state,id)->diagnosticFocus);
    setComparisonSettings(state,settings,id);
    auto visible = settings; visible.mode = ComparisonMode::original; visible.intersections.show = true;
    std::vector<ScenePickPart> pickParts; appendComparisonPickParts(pickParts,*findComparison(state,id),visible,result,false);
    CHECK(std::any_of(pickParts.begin(),pickParts.end(),[&](const auto& part){ return part.diagnosticEdges.data() == result.original.intersectionEdges.data(); }));
    const auto saved = fixture.root/"saved.woby"; writeSceneDocument(saved,createSceneDocument(state));
    CHECK(readSceneDocument(saved).comparisons[0].settings == settings);
    for (int version = 2; version <= 12; ++version) {
        const auto old = fixture.write("old.woby", "version = "+std::to_string(version)+"\n[[analyses]]\nname = \"old\"\n");
        CHECK_FALSE(readSceneDocument(old).comparisons[0].settings.intersections.autoUpdate);
    }
    auto views = readSceneDocument(fixture.write("view.woby", "version = 13\n[[analyses]]\nname = \"test\"\n[[views]]\nname = \"view\"\n"
        "[[views.objects]]\nkind = \"analysis\"\nindex = 0\nself_intersections_enabled = true\nshow_self_intersections = false\n"));
    REQUIRE(views.views.size() == 1); REQUIRE(views.views[0].objects.size() == 1);
    CHECK(views.views[0].objects[0].settings.comparison.intersections == IntersectionSettings{true,false});
    views.views[0].objects[0].settings.comparison.intersections.limits = {0, 123456};
    views.views[0].objects[0].settings.comparison.diagnosticCategory = DiagnosticCategory::selfIntersections;
    writeSceneDocument(fixture.root/"views.woby",views);
    CHECK(readSceneDocument(fixture.root/"views.woby").views[0].objects[0].settings.comparison == views.views[0].objects[0].settings.comparison);
    for (const auto* value : {"-1", "1.5", "2147483648", "true"}) {
        const auto invalid = fixture.write("invalid-budget.woby", std::string("version = 15\n[[analyses]]\nname = \"budget\"\nintersection_pair_limit = ") + value + "\n");
        CHECK_THROWS((void)readSceneDocument(invalid));
    }
    std::string report;
    for (const auto& line : comparisonReportLines("test","crossing.obj","",settings,result,{})) { report += line; }
    CHECK(report.find("self-intersections: 1 pairs; 2 known affected faces (complete)") != std::string::npos);
    settings.intersections.autoUpdate = false; setComparisonSettings(state,settings,id);
    CHECK_FALSE(findComparison(state,id)->diagnosticFocus);
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.set"), {{"target","analysis"},{"selfIntersections","true"}}));
}

namespace {
struct WorkflowFixture {
    Fixture files;
    UiState state;
    ComparisonRuntimes runtimes;
    SceneObjectId id = invalidSceneObjectId;
    bool initialized = false;
    WorkflowFixture() {
        bgfx::Init init; init.type = bgfx::RendererType::Noop;
        init.resolution.width = init.resolution.height = 1;
        initialized = bgfx::init(init);
        const auto path = files.write("crossing.obj", "v 0 0 0\nv 2 0 0\nv 0 2 0\nv .5 .5 -1\nv .5 .5 1\nv 1.5 .5 0\nf 1 2 3\nf 4 5 6\n");
        state.files.push_back(createUiFileState(path,loadObjMesh(path),0)); appendDefaultSceneNodesForFiles(state,0);
        id = createComparison(state); setComparisonObjects(state,{state.files[0].objectId},ComparisonSide::a,true,id);
        auto settings = comparisonSettings(state,id); settings.mode = ComparisonMode::original;
        setComparisonSettings(state,settings,id);
    }
    ~WorkflowFixture() { if (initialized) { destroyComparisonRuntimes(runtimes); bgfx::shutdown(); } }
    bool until(const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
        do {
            updateComparisonRuntimes(runtimes,state); bgfx::frame();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
};
}

TEST_CASE("changed intersection budgets invalidate only intersections and reach worker results")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    auto& runtime = f.runtimes.objects[f.id];
    auto settings = comparisonSettings(f.state, f.id);
    settings.intersections.autoUpdate = true; settings.intersections.limits = {1, 7};
    setComparisonSettings(f.state, settings, f.id);
    REQUIRE(f.until([&]{ return comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::selfIntersections); }));
    CHECK(runtime.result.original.intersections.limits == IntersectionLimits{1, 7});
    const auto revision = runtime.resultsRevision;
    const auto otherStages = runtime.cache.completed & ~comparisonIntersections;
    settings.intersections.limits = {0, 0}; setComparisonSettings(f.state, settings, f.id);
    CHECK_FALSE(comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::selfIntersections));
    REQUIRE(f.until([&]{ return comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::selfIntersections); }));
    CHECK(runtime.result.original.intersections.limits == IntersectionLimits{0, 0});
    CHECK(runtime.resultsRevision > revision);
    CHECK((runtime.cache.completed & otherStages) == otherStages);
    CHECK_FALSE(runtime.result.original.intersections.truncated);
    settings.intersections.autoUpdate = false; settings.intersections.limits = {2, 3};
    setComparisonSettings(f.state, settings, f.id); updateComparisonRuntimes(f.runtimes, f.state);
    CHECK_FALSE(comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::selfIntersections));
    CHECK_FALSE(runtime.intersection.worker.valid());
    requestComparisonIntersections(f.state, f.id);
    REQUIRE(f.until([&]{ return comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::selfIntersections); }));
    CHECK(runtime.result.original.intersections.limits == IntersectionLimits{2, 3});
}

TEST_CASE("manual intersection requests are transient and CLI actions validate detector names")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    const auto revision = f.state.sceneEditRevision;
    const auto document = createSceneDocument(f.state);
    for (const auto* key : diagnosticCategoryKeys) {
    for (const auto* method : {"analysis.run", "analysis.cancel"}) {
        auto operation = parseControlOperation(*findControlMethod(method), {{"target","analysis"},{"detector",key}});
        operation.objectId = f.id;
        CHECK(controlOperationParams(operation)["detector"] == key);
        (void)applyControlSceneOperation(f.state,document,operation,[](auto value){return std::to_string(value);},200,800);
        CHECK(f.state.sceneEditRevision == revision);
    }
    }
    for (const auto& request : findComparison(f.state, f.id)->detectorRequests) { CHECK(request.revision == 2); CHECK(request.cancel); }
    CHECK(findComparison(f.state,f.id)->intersectionRequestRevision == 2);
    CHECK(findComparison(f.state,f.id)->cancelIntersections);
    CHECK_FALSE(comparisonSettings(f.state,f.id).intersections.autoUpdate);
    const auto restored = prepareSceneReplacement(f.state,f.state.files,document);
    CHECK(restored.comparisons[0].intersectionRequestRevision == 0);
    const auto duplicate = duplicateComparison(f.state,f.id);
    CHECK(findComparison(f.state,duplicate)->intersectionRequestRevision == 0);
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.run"), {{"target","analysis"}}));
    CHECK_THROWS(parseControlOperation(*findControlMethod("analysis.run"), {{"target","analysis"},{"detector","typo"}}));
}

TEST_CASE("each detector publishes readiness without waiting for other stages")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    auto& runtime = f.runtimes.objects[f.id];
    const auto signature = comparisonGeometrySignature(f.state,f.id);
    runtime.cache = {signature,0};
    const auto mesh = comparisonWorldMesh(f.state,ComparisonSide::a,f.id);
    REQUIRE(applyComparisonStages(runtime.result,runtime.cache,computeComparisonStages(mesh,{},comparisonSource|comparisonTopology),signature,comparisonSource|comparisonTopology));
    runtime.resultSignature = signature; runtime.ready = true;
    runtime.uploadedStages = comparisonSource|comparisonTopology;
    runtime.result.original.intersections.phase = IntersectionPhase::running;
    CHECK(comparisonStagesReady(runtime,f.state,f.id,comparisonTopology));
    CHECK_FALSE(comparisonStagesReady(runtime,f.state,f.id,comparisonDuplicatePoints));
    CHECK_FALSE(comparisonResultsReady(runtime,f.state,f.id,true));
    auto visible = readyComparisonSettings(runtime,f.state,f.id);
    CHECK(visible.showBoundaries); CHECK_FALSE(visible.intersections.show); CHECK_FALSE(visible.duplicates.showPoints);
    navigateComparisonDiagnostic(f.state,runtime.result,signature,1,f.id);
    CHECK(findComparison(f.state,f.id)->diagnosticFocus.has_value());
    auto settings = comparisonSettings(f.state,f.id); settings.topologyMode = TopologyMode::exactPosition;
    setComparisonSettings(f.state,settings,f.id);
    CHECK_FALSE(comparisonStagesReady(runtime,f.state,f.id,comparisonTopology));
    CHECK(comparisonStagesReady(runtime,f.state,f.id,comparisonSource));
    CHECK(nextComparisonStage(comparisonSource|comparisonDistance|comparisonTopology) == comparisonSource);
    CHECK(nextComparisonStage(comparisonDistance|comparisonTopology) == comparisonTopology);
    CHECK(nextComparisonStage(comparisonIntersections) == 0);
}

TEST_CASE("manual checks update only on request and keep stale counts separate from current findings")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    SUBCASE("input A") {}
    SUBCASE("input B only") {
        setComparisonObjects(f.state,{f.state.files[0].objectId},ComparisonSide::a,false,f.id);
        setComparisonObjects(f.state,{f.state.files[0].objectId},ComparisonSide::b,true,f.id);
    }
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&]{ return comparisonResultsReady(runtime,f.state,f.id); }));
    CHECK(runtime.result.original.intersections.phase == IntersectionPhase::notChecked);
    CHECK(comparisonsReadyForScreenshot(f.state,f.runtimes));
    requestComparisonIntersections(f.state,f.id);
    CHECK_FALSE(comparisonsReadyForScreenshot(f.state,f.runtimes));
    REQUIRE(f.until([&]{ return comparisonStagesReady(runtime,f.state,f.id,comparisonIntersections,true); }));
    const bool hasA = enabledComparisonPartCount(f.state,ComparisonSide::a,f.id) != 0;
    const auto resultJson = [&]{ return controlComparisonResults(runtime.result,.05)[hasA ? "aToB" : "bToA"]["detectors"]["self_intersections"]; };
    CHECK(resultJson()["count"] == 1);
    auto navigation = comparisonSettings(f.state,f.id);
    navigation.diagnosticCategory = DiagnosticCategory::selfIntersections;
    navigation.diagnosticSide = hasA ? ComparisonSide::a : ComparisonSide::b;
    setComparisonSettings(f.state,navigation,f.id);
    selectComparisonDiagnostic(f.state,runtime.result,runtime.resultSignature,0,f.id);
    CHECK(findComparison(f.state,f.id)->diagnosticFocus.has_value());
    const auto fastStages = runtime.cache.completed & ~comparisonIntersections;
    auto settings = comparisonSettings(f.state,f.id); settings.intersections.show = false;
    setComparisonSettings(f.state,settings,f.id); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK(resultJson()["count"] == 1);
    CHECK_FALSE(runtime.intersection.worker.valid());
    settings.intersections.show = true; settings.topologyMode = TopologyMode::exactPosition;
    setComparisonSettings(f.state,settings,f.id);
    REQUIRE(f.until([&]{ return comparisonResultsReady(runtime,f.state,f.id); }));
    CHECK(runtime.result.original.intersections.phase == IntersectionPhase::outdated);
    CHECK(resultJson()["count"].is_null()); CHECK(resultJson()["previousCount"] == 1); CHECK(resultJson()["findings"].empty());
    CHECK_FALSE(readyComparisonSettings(runtime,f.state,f.id).intersections.show);
    CHECK(comparisonsReadyForScreenshot(f.state,f.runtimes));
    CHECK((runtime.cache.completed & fastStages) == fastStages);
    requestComparisonIntersections(f.state,f.id);
    REQUIRE(f.until([&]{ return comparisonStagesReady(runtime,f.state,f.id,comparisonIntersections); }));
    CHECK(resultJson()["count"] == 1);
    // Display offsets do not invalidate source geometry.
    setComparisonTranslation(f.state,f.id,{100,0,0}); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK(comparisonStagesReady(runtime,f.state,f.id,comparisonIntersections));
    requestComparisonIntersections(f.state,f.id,true); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK(runtime.result.original.intersections.phase == IntersectionPhase::canceled);
    CHECK(comparisonResultsReady(runtime,f.state,f.id));
    settings.intersections.autoUpdate = true; setComparisonSettings(f.state,settings,f.id);
    updateComparisonRuntimes(f.runtimes, f.state);
    CHECK(runtime.result.original.intersections.phase == IntersectionPhase::canceled);
    requestComparisonIntersections(f.state, f.id);
    REQUIRE(f.until([&]{ return comparisonStagesReady(runtime,f.state,f.id,comparisonIntersections); }));
}

TEST_CASE("cancel and failed intersection jobs do not block completed detectors or publish late results")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&]{ return comparisonResultsReady(runtime,f.state,f.id); }));
    const auto fastStages = runtime.cache.completed;
    std::promise<MeshComparison> pending;
    runtime.intersection.stop = std::stop_source{};
    runtime.intersection.worker = pending.get_future();
    runtime.intersection.workerSignature = runtime.cache.signature;
    runtime.intersection.workerRevision = runtime.intersection.revision;
    runtime.result.original.intersections.phase = IntersectionPhase::running;
    runtime.result.repaired.intersections.phase = IntersectionPhase::running;
    CHECK(comparisonResultsReady(runtime,f.state,f.id));
    CHECK(comparisonStagesReady(runtime,f.state,f.id,comparisonTopology,true));
    CHECK_FALSE(comparisonsReadyForScreenshot(f.state,f.runtimes));
    SUBCASE("canceled job finishes late") {
        requestComparisonIntersections(f.state,f.id,true); updateComparisonRuntimes(f.runtimes,f.state);
        CHECK(runtime.result.original.intersections.phase == IntersectionPhase::canceled);
        CHECK(comparisonsReadyForScreenshot(f.state,f.runtimes));
        pending.set_value(computeComparisonStages(comparisonWorldMesh(f.state,ComparisonSide::a,f.id),{},comparisonIntersections));
        updateComparisonRuntimes(f.runtimes,f.state);
        CHECK(runtime.result.original.intersections.phase == IntersectionPhase::canceled);
        CHECK((runtime.cache.completed & comparisonIntersections) == 0);
    }
    SUBCASE("new cheap work finishes while intersections remain running") {
        runtime.fullResultsRequested = true;
        REQUIRE(f.until([&]{ return comparisonStagesReady(runtime,f.state,f.id,comparisonQuality); }));
        CHECK(runtime.result.original.intersections.phase == IntersectionPhase::running);
        CHECK(comparisonResultsReady(runtime,f.state,f.id,true));
        pending.set_exception(std::make_exception_ptr(std::runtime_error("test failure")));
    }
    SUBCASE("failed job can be retried") {
        pending.set_exception(std::make_exception_ptr(std::runtime_error("test failure")));
        updateComparisonRuntimes(f.runtimes,f.state);
        CHECK(runtime.result.original.intersections.phase == IntersectionPhase::failed);
        CHECK(runtime.result.original.intersections.error == "test failure");
        CHECK(comparisonResultsReady(runtime,f.state,f.id));
        requestComparisonIntersections(f.state,f.id);
        REQUIRE(f.until([&]{ return comparisonStagesReady(runtime,f.state,f.id,comparisonIntersections); }));
    }
    CHECK((runtime.cache.completed & fastStages) == fastStages);
}


TEST_CASE("all detectors support manual runs with independent retained states")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    SUBCASE("input A") {}
    SUBCASE("input B") {
        setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, false, f.id);
        setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::b, true, f.id);
    }
    for (size_t i = 0; i < diagnosticCategoryCount; ++i) {
        setComparisonAutomaticUpdate(f.state, f.id, static_cast<DiagnosticCategory>(i), false);
    }
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime, f.state, f.id); }));
    CHECK(runtime.cache.completed == comparisonSource);
    const auto document = createSceneDocument(f.state);
    const auto revision = f.state.sceneEditRevision;
    for (size_t i = 0; i < diagnosticCategoryCount; ++i) {
        const auto category = static_cast<DiagnosticCategory>(i);
        CAPTURE(i);
        CHECK(comparisonDetectorStatus(runtime.result, category).phase == IntersectionPhase::notChecked);
        requestComparisonDetector(f.state, f.id, category);
        REQUIRE(f.until([&] { return comparisonDetectorReady(runtime, f.state, f.id, category, true); }));
        CHECK_FALSE(diagnosticAutoUpdate(comparisonSettings(f.state, f.id), category));
        CHECK(f.state.sceneEditRevision == revision);
        for (size_t other = i + 1; other < diagnosticCategoryCount; ++other) {
            CHECK(comparisonDetectorStatus(runtime.result, static_cast<DiagnosticCategory>(other)).phase == IntersectionPhase::notChecked);
        }
    }
    const auto visible = readyComparisonSettings(runtime, f.state, f.id);
    CHECK(visible.showBoundaries); CHECK(visible.showNonManifold); CHECK(visible.showWinding);
    CHECK(visible.duplicates.showPoints); CHECK(visible.duplicates.showTriangles);
    CHECK(visible.topologyInspection.showNonManifoldVertices); CHECK(visible.topologyInspection.showHoles);
    CHECK(visible.degenerates.show); CHECK(visible.intersections.show);
    auto thresholds = comparisonSettings(f.state, f.id);
    thresholds.topologyInspection.holeSizeRatioTolerance = .5f;
    setComparisonSettings(f.state, thresholds, f.id);
    updateComparisonRuntimes(f.runtimes, f.state);
    CHECK(comparisonDetectorStatus(runtime.result, DiagnosticCategory::holes).phase == IntersectionPhase::outdated);
    CHECK(comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::boundary));
    CHECK_FALSE(runtime.worker.valid());
    requestComparisonDetector(f.state, f.id, DiagnosticCategory::holes);
    REQUIRE(f.until([&] { return comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::holes, true); }));
    const auto oldCounts = runtime.result.detectors[0].knownCounts;
    setFileTranslation(f.state.files[0].fileSettings, {3, 0, 0});
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime, f.state, f.id); }));
    for (size_t i = 0; i < diagnosticCategoryCount; ++i) {
        CHECK(comparisonDetectorStatus(runtime.result, static_cast<DiagnosticCategory>(i)).phase == IntersectionPhase::outdated);
    }
    CHECK(runtime.result.detectors[0].knownCounts == oldCounts);
    const bool hasA = enabledComparisonPartCount(f.state, ComparisonSide::a, f.id) != 0;
    const auto json = controlComparisonResults(runtime.result, .05)[hasA ? "aToB" : "bToA"]["detectors"];
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        CHECK(json[diagnosticCategoryKeys[i]]["status"] == "out_of_date");
        CHECK(json[diagnosticCategoryKeys[i]]["count"].is_null());
        CHECK(json[diagnosticCategoryKeys[i]]["findings"].empty());
    }
    CHECK(json["boundary_edges"]["knownCount"] == oldCounts[hasA ? 0 : 1]);
    std::string report;
    for (const auto& line : comparisonReportLines("Manual", "A", "B", thresholds, runtime.result, {})) { report += line; }
    CHECK(report.find("boundary edges: out_of_date; previous result:") != std::string::npos);
    CHECK_FALSE(readyComparisonSettings(runtime, f.state, f.id).showBoundaries);
    CHECK(runtime.cache.completed == comparisonSource);
    CHECK_FALSE(runtime.worker.valid());
    const auto copy = duplicateComparison(f.state, f.id);
    for (const auto& request : findComparison(f.state, copy)->detectorRequests) { CHECK(request.revision == 0); }
    const auto loaded = prepareSceneReplacement(f.state, f.state.files, document);
    for (const auto& request : loaded.comparisons[0].detectorRequests) { CHECK(request.revision == 0); }
}

TEST_CASE("canceling one shared topology detector preserves the other worker results")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime, f.state, f.id); }));
    const auto boundary = static_cast<size_t>(DiagnosticCategory::boundary);
    const auto holes = static_cast<size_t>(DiagnosticCategory::holes);
    const auto signature = comparisonGeometrySignature(f.state, f.id);
    runtime.workerSignature = signature;
    runtime.workerStages = comparisonTopology;
    runtime.workerDetectors = (1u << boundary) | (1u << holes);
    runtime.result.detectors[boundary].phase = runtime.result.detectors[holes].phase = IntersectionPhase::running;
    std::promise<MeshComparison> work;
    runtime.worker = work.get_future();
    requestComparisonDetector(f.state, f.id, DiagnosticCategory::boundary, true);
    updateComparisonRuntimes(f.runtimes, f.state);
    CHECK(runtime.result.detectors[boundary].phase == IntersectionPhase::canceled);
    CHECK_FALSE(runtime.stop.stop_requested());
    work.set_value(computeComparisonStages(comparisonWorldMesh(f.state, ComparisonSide::a, f.id), {}, comparisonTopology));
    REQUIRE(f.until([&] { return !runtime.worker.valid(); }));
    CHECK(runtime.result.detectors[boundary].phase == IntersectionPhase::canceled);
    CHECK(comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::holes, true));
    CHECK_FALSE(readyComparisonSettings(runtime, f.state, f.id).showBoundaries);
    CHECK(readyComparisonSettings(runtime, f.state, f.id).topologyInspection.showHoles);
    for (int frame = 0; frame < 3; ++frame) { updateComparisonRuntimes(f.runtimes, f.state); }
    CHECK_FALSE(runtime.worker.valid());
    requestComparisonDetector(f.state, f.id, DiagnosticCategory::boundary);
    REQUIRE(f.until([&] { return comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::boundary, true); }));
}

TEST_CASE("failed and canceled automatic detectors wait for retry or relevant changes")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime, f.state, f.id); }));
    const auto index = static_cast<size_t>(DiagnosticCategory::degenerateTriangles);
    SUBCASE("failed worker") {
        runtime.workerSignature = comparisonGeometrySignature(f.state, f.id);
        runtime.workerStages = comparisonDegenerates;
        runtime.workerDetectors = 1u << index;
        runtime.result.detectors[index].phase = IntersectionPhase::running;
        std::promise<MeshComparison> work;
        runtime.worker = work.get_future();
        work.set_exception(std::make_exception_ptr(std::runtime_error("Detector failed")));
        updateComparisonRuntimes(f.runtimes, f.state);
        CHECK(runtime.result.detectors[index].phase == IntersectionPhase::failed);
        CHECK(runtime.result.detectors[index].error == "Detector failed");
    }
    SUBCASE("canceled") {
        requestComparisonDetector(f.state, f.id, DiagnosticCategory::degenerateTriangles, true);
        updateComparisonRuntimes(f.runtimes, f.state);
        CHECK(runtime.result.detectors[index].phase == IntersectionPhase::canceled);
    }
    const auto stopped = runtime.result.detectors[index].phase;
    for (int frame = 0; frame < 3; ++frame) { updateComparisonRuntimes(f.runtimes, f.state); }
    CHECK_FALSE(runtime.worker.valid());
    auto settings = comparisonSettings(f.state, f.id);
    settings.degenerates.show = !settings.degenerates.show;
    setComparisonSettings(f.state, settings, f.id);
    updateComparisonRuntimes(f.runtimes, f.state);
    CHECK(runtime.result.detectors[index].phase == stopped);
    settings.degenerates.needleThresholdRatio += 1;
    setComparisonSettings(f.state, settings, f.id);
    REQUIRE(f.until([&] { return comparisonDetectorReady(runtime, f.state, f.id, DiagnosticCategory::degenerateTriangles, true); }));
    CHECK(runtime.result.detectors[index].error.empty());
}

TEST_CASE("automatic detector preferences round trip scenes and saved views")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    for (size_t i = 0; i < diagnosticCategoryCount; ++i) {
        const auto category = static_cast<DiagnosticCategory>(i);
        CHECK(diagnosticAutoUpdate(comparisonSettings(f.state, f.id), category) == (category != DiagnosticCategory::selfIntersections));
        setComparisonAutomaticUpdate(f.state, f.id, category, category == DiagnosticCategory::selfIntersections);
    }
    auto command = parseControlOperation(*findControlMethod("analysis.set"), {{"target", "analysis"},
        {"autoUpdateBoundaries", false}, {"autoUpdateNonManifold", false}, {"autoUpdateWinding", false}});
    command.objectId = f.id;
    CHECK(controlOperationParams(command)["autoUpdateBoundaries"] == false);
    CHECK(controlOperationParams(command)["autoUpdateNonManifold"] == false);
    CHECK(controlOperationParams(command)["autoUpdateWinding"] == false);
    (void)applyControlSceneOperation(f.state, createSceneDocument(f.state), command, [](auto value) { return std::to_string(value); }, 200, 800);
    const auto settings = comparisonSettings(f.state, f.id);
    auto document = createSceneDocument(f.state);
    // Both scene and view records use the same settings mapping.
    SceneViewRecord view;
    view.name = "Manual detectors";
    SceneViewObjectRecord object;
    object.kind = ViewObjectKind::comparison; object.index = 0;
    object.settings.comparison = settings;
    view.objects.push_back(object); document.views.push_back(view);
    const auto path = f.files.root / "manual-detectors.woby";
    writeSceneDocument(path, document);
    const auto loaded = readSceneDocument(path);
    CHECK(loaded.comparisons[0].settings == settings);
    CHECK(loaded.views[0].objects[0].settings.comparison == settings);
    const auto legacy = readSceneDocument(f.files.write("legacy.woby", "version = 13\n[[analyses]]\nname = \"legacy\"\nduplicate_points_enabled = false\nanalysis_holes_enabled = false\n"));
    CHECK_FALSE(diagnosticAutoUpdate(legacy.comparisons[0].settings, DiagnosticCategory::duplicatePoints));
    CHECK_FALSE(diagnosticAutoUpdate(legacy.comparisons[0].settings, DiagnosticCategory::holes));
    CHECK(diagnosticAutoUpdate(legacy.comparisons[0].settings, DiagnosticCategory::boundary));
}


TEST_CASE("fin lifecycle filters cached patches and hides stale canceled and failed findings")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    f.state = {};
    const auto path = f.files.write("fins.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 -2 0\nv 0 0 4\nf 1 2 3\nf 2 1 4\nf 1 2 5\n");
    f.state.files.push_back(createUiFileState(path,loadObjMesh(path),0)); appendDefaultSceneNodesForFiles(f.state,0);
    f.id = createComparison(f.state); setComparisonObjects(f.state,{f.state.files[0].objectId},ComparisonSide::a,true,f.id);
    auto settings = comparisonSettings(f.state,f.id); settings.mode = ComparisonMode::original; setComparisonSettings(f.state,settings,f.id);
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime,f.state,f.id); }));
    REQUIRE(runtime.result.original.topology.fins.size() == 3);
    const auto* patches = runtime.result.original.topology.finPatches.data();
    settings = comparisonSettings(f.state,f.id); settings.topologyInspection.finMaxAreaRatio = .25f;
    setComparisonSettings(f.state,settings,f.id); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK_FALSE(runtime.worker.valid()); CHECK(runtime.result.original.topology.finPatches.data() == patches);
    CHECK(comparisonDetectorReady(runtime,f.state,f.id,DiagnosticCategory::fins,true));
    CHECK(runtime.result.original.topology.fins.size() == 1);
    CHECK(comparisonDetectorStatus(runtime.result,DiagnosticCategory::fins).knownCounts[0] == 1);
    settings.topologyInspection.showFins = false; setComparisonSettings(f.state,settings,f.id); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK_FALSE(runtime.worker.valid()); CHECK(runtime.result.original.topology.finPatches.data() == patches);
    CHECK_FALSE(readyComparisonSettings(runtime,f.state,f.id).topologyInspection.showFins);
    settings.topologyInspection.showFins = true; settings.topologyInspection.fins = false; settings.topologyInspection.finMaxAreaRatio = .5f;
    setComparisonSettings(f.state,settings,f.id); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK(comparisonDetectorStatus(runtime.result,DiagnosticCategory::fins).phase == IntersectionPhase::outdated);
    CHECK(comparisonDetectorReady(runtime,f.state,f.id,DiagnosticCategory::holes));
    CHECK_FALSE(readyComparisonSettings(runtime,f.state,f.id).topologyInspection.showFins);
    CHECK(controlComparisonResults(runtime.result,.05)["aToB"]["detectors"]["fins"]["findings"].empty());
    requestComparisonDetector(f.state,f.id,DiagnosticCategory::fins); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK_FALSE(runtime.worker.valid()); CHECK(runtime.result.original.topology.finPatches.data() == patches);
    CHECK(comparisonDetectorReady(runtime,f.state,f.id,DiagnosticCategory::fins,true));
    CHECK(runtime.result.original.topology.fins.size() == 2);
    requestComparisonDetector(f.state,f.id,DiagnosticCategory::fins,true); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK(comparisonDetectorStatus(runtime.result,DiagnosticCategory::fins).phase == IntersectionPhase::canceled);
    CHECK_FALSE(readyComparisonSettings(runtime,f.state,f.id).topologyInspection.showFins);
    requestComparisonDetector(f.state,f.id,DiagnosticCategory::fins); updateComparisonRuntimes(f.runtimes,f.state);
    CHECK(comparisonDetectorReady(runtime,f.state,f.id,DiagnosticCategory::fins,true));
    auto& status = runtime.result.detectors[static_cast<size_t>(DiagnosticCategory::fins)];
    status.phase = IntersectionPhase::failed; status.error = "fixture failure";
    CHECK_FALSE(readyComparisonSettings(runtime,f.state,f.id).topologyInspection.showFins);
    CHECK(controlComparisonResults(runtime.result,.05)["aToB"]["detectors"]["fins"]["status"] == "failed");
    setFileTranslation(f.state.files[0].fileSettings,{3,0,0});
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime,f.state,f.id); }));
    CHECK(comparisonDetectorStatus(runtime.result,DiagnosticCategory::fins).phase == IntersectionPhase::outdated);
    CHECK_FALSE(readyComparisonSettings(runtime,f.state,f.id).topologyInspection.showFins);
}

TEST_CASE("fast collapse filter agrees with rational determinants over floating point scales")
{
    using Exact = boost::multiprecision::cpp_rational;
    std::mt19937 random(9341);
    for (const int exponent : {-1074,-530,-100,0,100,510,1000}) {
        for (size_t trial = 0; trial < 40; ++trial) {
            Triangle triangle;
            for (auto& p : triangle) {
                for (auto& coordinate : p) { coordinate = std::scalbn(static_cast<double>(static_cast<int>(random()%17)-8), exponent); }
            }
            if (trial%3 == 0) {
                for (size_t k = 0; k < 3; ++k) { triangle[2][k] = triangle[0][k] + 2*(triangle[1][k]-triangle[0][k]); }
            }
            if (trial%3 == 1) { triangle[2][0] = std::nextafter(triangle[2][0], std::numeric_limits<double>::infinity()); }
            std::array<Exact,3> a, b;
            for (size_t k = 0; k < 3; ++k) {
                a[k] = Exact(triangle[1][k])-Exact(triangle[0][k]);
                b[k] = Exact(triangle[2][k])-Exact(triangle[0][k]);
            }
            const bool collapsed = a[0]*b[1] == a[1]*b[0] && a[1]*b[2] == a[2]*b[1] && a[2]*b[0] == a[0]*b[2];
            CHECK(woby::exactTriangleCollapsed(triangle) == collapsed);
        }
    }
}

TEST_CASE("detector batch retries unaffected work after threshold edits or a stage failure")
{
    WorkflowFixture f; REQUIRE(f.initialized);
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(f.until([&] { return comparisonResultsReady(runtime, f.state, f.id); }));
    runtime.workerSignature = runtime.cache.signature;
    runtime.workerStages = comparisonDetectors;
    runtime.workerDetectors = (1u << backgroundDetectorCount)-1;
    runtime.workerDetectorRequests = runtime.consumedDetectorRequests;
    runtime.stop = std::stop_source{};
    for (auto& status : runtime.result.detectors) { status.phase = IntersectionPhase::running; }
    std::promise<MeshComparison> pending;
    runtime.worker = pending.get_future();
    SUBCASE("threshold edit") {
        auto settings = comparisonSettings(f.state, f.id);
        settings.degenerates.needleThresholdRatio += 1;
        setComparisonSettings(f.state, settings, f.id);
        updateComparisonRuntimes(f.runtimes, f.state);
        REQUIRE(runtime.stop.stop_requested());
        pending.set_exception(std::make_exception_ptr(std::runtime_error("Analysis canceled.")));
    }
    SUBCASE("topology mode edit with explicit cancellation") {
        requestComparisonDetector(f.state, f.id, DiagnosticCategory::duplicatePoints, true);
        auto settings = comparisonSettings(f.state, f.id); settings.topologyMode = TopologyMode::exactPosition;
        setComparisonSettings(f.state, settings, f.id);
        updateComparisonRuntimes(f.runtimes, f.state);
        REQUIRE(runtime.stop.stop_requested());
        pending.set_value({}); // Even a worker that finishes after cancellation must be ignored.
    }
    SUBCASE("failed batch is isolated into individual stages") {
        pending.set_exception(std::make_exception_ptr(std::runtime_error("Injected batch failure.")));
        updateComparisonRuntimes(f.runtimes, f.state);
        CHECK(runtime.retryDetectorsSeparately);
        CHECK(runtime.workerStages == comparisonTopology);
    }
    REQUIRE(f.until([&] {
        if (runtime.worker.valid()) { return false; }
        return std::all_of(runtime.result.detectors.begin(), runtime.result.detectors.end(), [](const auto& status) {
            return status.phase == IntersectionPhase::complete || status.phase == IntersectionPhase::canceled;
        });
    }));
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        const auto expected = findComparison(f.state, f.id)->detectorRequests[i].cancel
            ? IntersectionPhase::canceled : IntersectionPhase::complete;
        CHECK(runtime.result.detectors[i].phase == expected);
    }
}
