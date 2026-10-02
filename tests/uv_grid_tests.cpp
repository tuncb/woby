#include "ui_operations.h"
#include "scene_history.h"
#include "control_scene.h"
#include "automation_registry.h"
#include "command_line.h"
#include "comparison_scene.h"
#include "comparison_view.h"
#include "mesh_comparison.h"
#include "uv_analysis.h"
#include "uv_quality.h"
#include "scene_pick.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <limits>
#include <thread>

TEST_CASE("UV quality distinguishes scale distortion collapse and mixed orientation")
{
    woby::Mesh mesh;
    mesh.vertices = {{{0,0,0},{},{0,0}},{{1,0,0},{},{2,0}},{{0,1,0},{},{0,2}}};
    mesh.indices = {0,1,2};
    mesh.nodes = {{"patch",0,3}};
    mesh.nodes[0].hasTexcoords = true;
    mesh.nodes[0].sourceObjectId = 42;
    auto q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::angle);
    CHECK(q.triangles[0].angleDegrees == doctest::Approx(0));
    CHECK(q.triangles[0].areaLog2 == doctest::Approx(0));
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::absolute,woby::UvQualityMetric::area);
    CHECK(q.triangles[0].areaLog2 == doctest::Approx(2));
    mesh.vertices[1].texcoord[0] = -2;
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::angle);
    CHECK(q.triangles[0].orientation == -1);
    CHECK(q.mixedOrientationPatches == 0);
    CHECK(q.triangles[0].angleDegrees == doctest::Approx(0));
    mesh.vertices[1].texcoord[0] = 4;
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::angle);
    CHECK(q.triangles[0].angleDegrees == doctest::Approx(18.4349488));
    mesh.indices.insert(mesh.indices.end(),{0,2,1}); mesh.nodes[0].indexCount = 6;
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::orientation);
    CHECK(q.mixedOrientationPatches == 1);
    CHECK(q.triangles[0].mixedOrientation);
    CHECK(q.triangles[1].triangle == 2);
    CHECK(q.triangles[1].partId == 42);
    // Opposite orientation in independent patches is not a mixed-domain finding.
    mesh.nodes[0].indexCount = 3;
    mesh.nodes.push_back(mesh.nodes[0]); mesh.nodes[1].indexOffset = 3;
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::angle);
    CHECK(q.mixedOrientationPatches == 0);
    mesh.vertices[2].texcoord = {0,0};
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::angle);
    CHECK(q.collapsed == 2);
    CHECK(std::isfinite(q.triangles[0].areaLog2));
    mesh.nodes[1].hasTexcoords = false;
    mesh.vertices[2].position = {0,0,0};
    q = woby::analyzeUvQuality(mesh,woby::UvAreaNormalization::perPatch,woby::UvQualityMetric::angle);
    CHECK(q.missing == 1);
    CHECK(q.degenerateSurface == 1);
}

namespace {
struct UvFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-uv-" + woby::automationRandomHex(8));
    woby::UiState state;
    UvFixture()
    {
        std::filesystem::create_directory(root);
        woby::Mesh mesh;
        mesh.vertices = {{{0, 0, 0}, {0, 0, 1}, {0, 0}},
            {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
        mesh.indices = {0, 1, 2, 0, 1, 2, 0, 1, 2};
        mesh.nodes = {{"UV A", 0, 3}, {"UV B", 3, 3}, {"No UV", 6, 3}};
        mesh.nodes[0].hasTexcoords = mesh.nodes[1].hasTexcoords = true;
        mesh.bounds = woby::calculateBounds(mesh.vertices);
        state.files.push_back(woby::createUiFileState(root / "mesh.obj", std::move(mesh), 0));
        woby::appendFolderTreeSceneNode(state, root, 0, 1);
        woby::clearSceneDirty(state);
    }
    ~UvFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

void checkUvBounds(const woby::UiState& state, woby::SceneObjectId id)
{
    const auto bounds = woby::comparisonDisplayBounds(state, id);
    const auto mesh = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    REQUIRE(bounds);
    const auto* comparison = woby::findComparison(state, id);
    REQUIRE(comparison);
    for (size_t k = 0; k < 3; ++k) {
        CHECK(bounds->min[k] == mesh.bounds.min[k] + comparison->translation[k]);
        CHECK(bounds->max[k] == mesh.bounds.max[k] + comparison->translation[k]);
    }
    if (comparison->settings.uvSeparated) { CHECK(bounds->radius == mesh.bounds.radius); }
}

struct UvRuntimeFixture {
    UvFixture fixture;
    woby::ComparisonRuntimes runtimes;
    woby::SceneObjectId id;
    bool initialized = false;
    UvRuntimeFixture()
    {
        woby::graphics::Init init;
        init.type = woby::graphics::RendererType::Noop;
        init.resolution.width = init.resolution.height = 1;
        initialized = woby::graphics::init(init);
        auto& state = fixture.state;
        id = woby::createComparison(state, woby::AnalysisType::uvQuality);
        woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    }
    ~UvRuntimeFixture()
    {
        woby::destroyComparisonRuntimes(runtimes);
        if (initialized) { woby::graphics::shutdown(); }
    }
    bool ready()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            woby::updateComparisonRuntimes(runtimes, fixture.state);
            woby::graphics::frame();
            if (woby::comparisonResultsReady(runtimes.objects[id], fixture.state, id)) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
};
}

TEST_CASE("UV bounds cache ignores presentation and quality edits and is absent from documents")
{
    UvFixture f;
    auto& state = f.state;
    const auto id = woby::createComparison(state, woby::AnalysisType::uvQuality);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    auto settings = woby::comparisonSettings(state, id);
    settings.uvView = woby::UvView::layout;
    settings.uvSeparated = true;
    woby::setComparisonSettings(state, settings, id);
    const auto document = woby::createSceneDocument(state);
    checkUvBounds(state, id);
    const auto cached = woby::findComparison(state, id)->boundsCache;
    REQUIRE(cached);
    woby::recalculateSceneBounds(state);
    woby::selectSceneObject(state, id);
    CHECK(woby::selectedSceneBounds(state).has_value());
    CHECK(woby::findComparison(state, id)->boundsCache == cached);
    CHECK(woby::createSceneDocument(state) == document);
    for (const auto axis : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        woby::setSceneUpAxis(state, axis);
        checkUvBounds(state, id);
        CHECK(woby::findComparison(state, id)->boundsCache == cached);
    }
    settings.uvMetric = woby::UvQualityMetric::area;
    settings.uvNormalization = woby::UvAreaNormalization::absolute;
    settings.uvGrid.densityU = 17;
    settings.showEdges = !settings.showEdges;
    woby::setComparisonSettings(state, settings, id);
    woby::setComparisonTranslation(state, id, {8, -9, 13});
    checkUvBounds(state, id);
    CHECK(woby::findComparison(state, id)->boundsCache == cached);
}

TEST_CASE("UV bounds cache invalidates for source and layout changes without changing mesh bounds")
{
    UvFixture f;
    auto& state = f.state;
    const auto id = woby::createComparison(state, woby::AnalysisType::uv);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    checkUvBounds(state, id);
    const auto cached = woby::findComparison(state, id)->boundsCache;
    SUBCASE("group transform") { state.files[0].groupSettings[0].rotationDegrees = {13, 27, -19}; }
    SUBCASE("file transform") { state.files[0].fileSettings.translation = {7, 11, 13}; }
    SUBCASE("folder transform") { state.sceneNodes[0].settings.rotationDegrees = {11, 43, 17}; }
    SUBCASE("geometry replacement") {
        auto replacement = state.files[0].mesh;
        replacement.vertices[1].position = {2, 3, 4};
        replacement.vertices[1].texcoord = {7, -3};
        state.files[0].mesh = std::move(replacement);
    }
    SUBCASE("membership") { woby::isolateUvObjects(state, {state.files[0].groupSettings[1].objectId}, id); }
    SUBCASE("separation") {
        auto settings = woby::comparisonSettings(state, id);
        settings.uvSeparated = true;
        woby::setComparisonSettings(state, settings, id);
    }
    checkUvBounds(state, id);
    CHECK(woby::findComparison(state, id)->boundsCache != cached);
    const auto refreshed = woby::findComparison(state, id)->boundsCache;
    checkUvBounds(state, id);
    CHECK(woby::findComparison(state, id)->boundsCache == refreshed);
}

TEST_CASE("Separated UV bounds match irregular patches including missing and constant UVs")
{
    UvFixture f;
    auto& state = f.state;
    auto& mesh = state.files[0].mesh;
    mesh.vertices.insert(mesh.vertices.end(), {{{3, -4, 7}, {}, {-2, 5}}, {{9, 2, 1}, {}, {8, 5}}, {{-2, 5, 6}, {}, {-2, 11}}});
    mesh.indices.insert(mesh.indices.end(), {3, 4, 5});
    mesh.nodes.push_back({"Irregular UV", 9, 3});
    mesh.nodes.back().hasTexcoords = true;
    SUBCASE("unequal extents") {}
    SUBCASE("constant coordinates") { for (auto& vertex : mesh.vertices) { vertex.texcoord = {2, -3}; } }
    state.files[0] = woby::createUiFileState(f.root / "patches.obj", std::move(mesh), 0);
    state.sceneNodes.clear();
    woby::appendDefaultSceneNodesForFiles(state, 0);
    state.files[0].groupSettings[1].rotationDegrees = {17, 41, -23};
    const auto id = woby::createComparison(state, woby::AnalysisType::uvQuality);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    for (const auto axis : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        woby::setSceneUpAxis(state, axis);
        for (const bool separated : {false, true}) {
            auto settings = woby::comparisonSettings(state, id);
            settings.uvView = woby::UvView::layout;
            settings.uvSeparated = separated;
            woby::setComparisonSettings(state, settings, id);
            checkUvBounds(state, id);
        }
    }
}

TEST_CASE("UV bounds cache handles missing UVs removed sources and copied scenes")
{
    UvFixture f;
    auto& state = f.state;
    const auto id = woby::createComparison(state, woby::AnalysisType::uv);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    checkUvBounds(state, id);
    const auto original = woby::findComparison(state, id)->boundsCache;
    auto copy = state;
    copy.files[0].fileSettings.translation = {13, 17, -7};
    checkUvBounds(copy, id);
    CHECK(woby::findComparison(copy, id)->boundsCache != original);
    CHECK(woby::findComparison(state, id)->boundsCache == original);
    woby::isolateUvObjects(state, {state.files[0].groupSettings[2].objectId}, id);
    CHECK_FALSE(woby::comparisonDisplayBounds(state, id));
    const auto empty = woby::findComparison(state, id)->boundsCache;
    REQUIRE(empty);
    CHECK_FALSE(woby::comparisonDisplayBounds(state, id));
    CHECK(woby::findComparison(state, id)->boundsCache == empty);
    state.files.clear();
    CHECK_FALSE(woby::comparisonDisplayBounds(state, id));
}

TEST_CASE("UV worker snapshot owns inputs and prepares matching geometry quality normals and edges")
{
    UvFixture f;
    const auto id = woby::createComparison(f.state, woby::AnalysisType::uvQuality);
    woby::setComparisonObjects(f.state, {f.state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    auto settings = woby::comparisonSettings(f.state, id);
    settings.uvView = woby::UvView::layout;
    settings.uvSeparated = true;
    woby::setComparisonSettings(f.state, settings, id);
    const auto expected = woby::comparisonWorldMesh(f.state, woby::ComparisonSide::a, id);
    const auto snapshot = woby::snapshotComparisonInputs(f.state, id);
    REQUIRE(snapshot.files.size() == 1);
    f.state.files.clear();
    f.state.sceneNodes.clear();
    const auto prepared = woby::prepareUvComparisonInputs(snapshot);
    const auto& actual = (*prepared.meshes)[0];
    REQUIRE(actual.uvQuality);
    CHECK(actual.uvQuality->missing == expected.uvQuality->missing);
    CHECK(actual.indices == expected.indices);
    CHECK(actual.precisePositions == expected.precisePositions);
    CHECK(actual.bounds.min == expected.bounds.min);
    CHECK(actual.bounds.max == expected.bounds.max);
    auto smooth = expected.vertices;
    woby::generateSmoothNormals(smooth, expected.indices);
    REQUIRE(actual.vertices.size() == smooth.size());
    for (size_t i = 0; i < smooth.size(); ++i) {
        CHECK(actual.vertices[i].normal == smooth[i].normal);
        CHECK(actual.vertices[i].position == smooth[i].position);
    }
    const auto quality = woby::uvQualityVertices(expected);
    REQUIRE(prepared.buffers[0].quality.size() == quality.size());
    for (size_t i = 0; i < quality.size(); ++i) {
        CHECK(prepared.buffers[0].quality[i].position == quality[i].position);
        CHECK(prepared.buffers[0].quality[i].normal == quality[i].normal);
        CHECK(prepared.buffers[0].quality[i].texcoord == quality[i].texcoord);
    }
    const std::vector<uint32_t> expectedLines{0,1,1,2,2,0,3,4,4,5,5,3};
    CHECK(prepared.buffers[0].lines == expectedLines);
    CHECK((*prepared.meshes)[1].indices.empty());
    std::stop_source stop;
    stop.request_stop();
    CHECK_THROWS((void)woby::prepareUvComparisonInputs(snapshot, stop.get_token()));
    CHECK_THROWS((void)woby::uvLayoutMesh(expected, true, stop.get_token()));
    CHECK_THROWS((void)woby::analyzeUvQuality(expected, settings.uvNormalization, settings.uvMetric, stop.get_token()));
    CHECK_THROWS((void)woby::uvQualityVertices(expected, stop.get_token()));
}

TEST_CASE("UV runtime prepares asynchronously retains warm buffers and refreshes edited metrics")
{
    UvRuntimeFixture f;
    REQUIRE(f.initialized);
    auto& state = f.fixture.state;
    woby::updateComparisonRuntimes(f.runtimes, state);
    auto& runtime = f.runtimes.objects[f.id];
    CHECK(runtime.preparationWorker.valid());
    CHECK_FALSE(runtime.inputs);
    REQUIRE(f.ready());
    REQUIRE(runtime.inputs);
    CHECK_FALSE(runtime.prepared); // Upload staging memory is released after copying to the GPU.
    const auto inputs = runtime.inputs;
    const auto revision = runtime.resultsRevision;
    for (const auto axis : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        woby::setSceneUpAxis(state, axis);
        woby::updateComparisonRuntimes(f.runtimes, state);
        CHECK(runtime.inputs == inputs);
        CHECK(runtime.resultsRevision == revision);
        CHECK_FALSE(runtime.preparationWorker.valid());
    }
    CHECK(runtime.inputs == inputs);
    CHECK(runtime.resultsRevision == revision);
    CHECK_FALSE(runtime.preparationWorker.valid());
    auto settings = woby::comparisonSettings(state, f.id);
    settings.uvMetric = woby::UvQualityMetric::area;
    woby::setComparisonSettings(state, settings, f.id);
    REQUIRE(f.ready());
    CHECK(runtime.inputs != inputs);
    CHECK(runtime.result.original.source.uvQuality->metric == woby::UvQualityMetric::area);
    CHECK(runtime.resultsRevision != revision);
}

TEST_CASE("UV runtime rejects stale preparation results and survives removal during preparation")
{
    UvRuntimeFixture f;
    REQUIRE(f.initialized);
    auto& state = f.fixture.state;
    auto& runtime = f.runtimes.objects[f.id];
    auto stale = std::make_shared<woby::PreparedComparisonInputs>(woby::prepareUvComparisonInputs(woby::snapshotComparisonInputs(state, f.id)));
    const auto signature = woby::comparisonGeometrySignature(state, f.id);
    runtime.cache.signature = runtime.preparationSignature = signature;
    std::promise<std::shared_ptr<const woby::PreparedComparisonInputs>> completion;
    runtime.preparationWorker = completion.get_future();
    completion.set_value(stale);
    state.files[0].groupSettings[0].translation = {7, 0, 0};
    woby::updateComparisonRuntimes(f.runtimes, state);
    CHECK(runtime.prepared != stale);
    REQUIRE(f.ready());
    CHECK(runtime.resultSignature == woby::comparisonGeometrySignature(state, f.id));
    CHECK(runtime.result.original.source.vertices[0].position[0] == 7);
    auto settings = woby::comparisonSettings(state, f.id);
    settings.uvMetric = woby::UvQualityMetric::orientation;
    woby::setComparisonSettings(state, settings, f.id);
    woby::updateComparisonRuntimes(f.runtimes, state);
    REQUIRE(runtime.preparationWorker.valid());
    state.comparisons.clear();
    state.files.clear();
    woby::updateComparisonRuntimes(f.runtimes, state);
    CHECK(f.runtimes.objects.empty());
}

TEST_CASE("UV runtime reports preparation failure once and recovers after geometry replacement")
{
    UvRuntimeFixture f;
    REQUIRE(f.initialized);
    auto& state = f.fixture.state;
    auto valid = state.files[0].mesh;
    state.files[0].mesh.indices[0] = UINT32_MAX;
    woby::updateComparisonRuntimes(f.runtimes, state);
    auto& runtime = f.runtimes.objects[f.id];
    REQUIRE(runtime.preparationWorker.valid());
    runtime.preparationWorker.wait();
    woby::updateComparisonRuntimes(f.runtimes, state);
    CHECK_FALSE(runtime.error.empty());
    CHECK((runtime.failedStages & woby::comparisonSource) != 0);
    CHECK_FALSE(runtime.ready);
    woby::updateComparisonRuntimes(f.runtimes, state);
    CHECK_FALSE(runtime.preparationWorker.valid());
    state.files[0].mesh = std::move(valid);
    REQUIRE(f.ready());
    CHECK(runtime.error.empty());
}

TEST_CASE("UV edits expand parents skip missing UVs and preserve per-child overrides")
{
    UvFixture f;
    auto& state = f.state;
    auto& parts = state.files[0].groupSettings;
    woby::selectSceneObject(state, state.sceneNodes[0].objectId);
    REQUIRE(woby::selectedObjectProperty(state, woby::UiObjectProperty::uvGrid).available);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvGrid, 1);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvDensityU, 17);
    CHECK(parts[0].uvGrid == woby::UvGridSettings{true, 17, 10});
    CHECK(parts[1].uvGrid == parts[0].uvGrid);
    CHECK(parts[2].uvGrid == woby::UvGridSettings{});
    woby::selectSceneObject(state, parts[1].objectId);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvDensityU, 4);
    woby::selectSceneObject(state, state.files[0].objectId);
    CHECK(woby::selectedObjectProperty(state, woby::UiObjectProperty::uvDensityU).mixed);
    const auto saved = woby::createSceneDocument(state);
    woby::applySceneFileRecord(state.files[0], saved.files[0]);
    CHECK(parts[0].uvGrid.densityU == 17);
    CHECK(parts[1].uvGrid.densityU == 4);
    woby::selectSceneObject(state, parts[2].objectId);
    CHECK_FALSE(woby::selectedObjectProperty(state, woby::UiObjectProperty::uvGrid).available);
    const auto before = woby::createSceneDocument(state);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::uvGrid, 1);
    CHECK(woby::createSceneDocument(state) == before);
    woby::selectSceneObject(state, parts[0].objectId);
    woby::resetSelectedObjectProperties(state, woby::UiPropertyGroup::appearance);
    CHECK(parts[0].uvGrid == woby::UvGridSettings{});
    CHECK(parts[1].uvGrid.densityU == 4);
}

TEST_CASE("UV density validation and no-op notifications are consistent")
{
    UvFixture f;
    auto& state = f.state;
    REQUIRE(woby::setObjectUvGrid(state, {}, true, -2.0f, 1e9f));
    CHECK(state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, .1f, 1000});
    const auto revision = state.sceneEditRevision;
    REQUIRE(woby::setObjectUvGrid(state, {}, true, .1f, 1000.0f));
    CHECK(state.sceneEditRevision == revision);
    CHECK_FALSE(woby::setObjectUvGrid(state, {}, false, std::numeric_limits<float>::infinity(), {}));
    CHECK(state.sceneEditRevision == revision);
    CHECK(woby::normalizedUvGrid({true, std::numeric_limits<float>::quiet_NaN(), -1}) == woby::UvGridSettings{true, 10, .1f});
}

TEST_CASE("UV settings round trip through scenes saved views and history")
{
    UvFixture f;
    auto& state = f.state;
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    REQUIRE(woby::setObjectUvGrid(state, {}, true, 7.0f, 13.0f));
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK_FALSE(state.files[0].groupSettings[0].uvGrid.enabled);
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, 7, 13});
    const auto view = woby::createView(state);
    REQUIRE(woby::setObjectUvGrid(state, {}, false, 20.0f, 30.0f));
    woby::applyView(state, view);
    CHECK(state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, 7, 13});
    const auto document = woby::createSceneDocument(state);
    const auto path = f.root / "uv.woby";
    woby::writeSceneDocument(path, document);
    auto restored = woby::readSceneDocument(path);
    for (auto& file : restored.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
    CHECK(restored == document);
    std::ofstream(path) << "version = 6\n[[files]]\npath = \"mesh.obj\"\n[[files.groups]]\nname = \"UV A\"\n";
    CHECK(woby::readSceneDocument(path).files[0].groups[0].settings.uvGrid == woby::UvGridSettings{});
    std::ofstream(path) << "version = 6\n[[files]]\npath = \"mesh.obj\"\n[[files.groups]]\nname = \"UV A\"\n"
        "uv_grid = true\nuv_density_u = -4\nuv_density_v = 100000\n";
    CHECK(woby::readSceneDocument(path).files[0].groups[0].settings.uvGrid == woby::UvGridSettings{true, .1f, 1000});
}

TEST_CASE("UV control commands parse report settings and reject unavailable targets atomically")
{
    UvFixture f;
    std::vector<std::string> words = {"woby", "ctl", "--instance", "uv", "render", "set", "scene",
        "--uv-grid", "true", "--uv-density-u", "5", "--uv-density-v", "9"};
    std::vector<char*> argv;
    for (auto& word : words) { argv.push_back(word.data()); }
    const auto parsed = woby::parseCommandLine(static_cast<int>(argv.size()), argv.data());
    const auto& method = woby::controlMethod(woby::ControlAction::render);
    const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
    const auto clean = woby::createSceneDocument(f.state);
    auto command = woby::parseControlOperation(method, {{"target", "scene"}, {"uvGrid", true}, {"uvDensityU", 5}, {"uvDensityV", 9}});
    CHECK(parsed.control.command == woby::ControlCommand::operation);
    CHECK(parsed.control.operation.uvGrid == true);
    CHECK(parsed.control.operation.uvDensityU == 5);
    CHECK(parsed.control.operation.uvDensityV == 9);
    CHECK(woby::controlOperationParams(command)["uvDensityV"] == 9);
    (void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800);
    CHECK(f.state.files[0].groupSettings[0].uvGrid == woby::UvGridSettings{true, 5, 9});
    CHECK_FALSE(f.state.files[0].groupSettings[2].uvGrid.enabled);
    command.objectId = f.state.files[0].groupSettings[2].objectId;
    command.target = format(command.objectId);
    command.solid = false;
    const auto before = woby::createSceneDocument(f.state);
    CHECK_THROWS_AS((void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800), std::invalid_argument);
    CHECK(woby::createSceneDocument(f.state) == before);
    CHECK_THROWS(woby::parseControlOperation(method, {{"target", "scene"}, {"uvDensityU", "nan"}}));
}

TEST_CASE("UV layout preserves seams tile offsets and proportions while omitting unavailable parts")
{
    UvFixture f;
    auto source = f.state.files[0].mesh;
    source.vertices[0].texcoord = {-1, 0};
    source.vertices[1].texcoord = {3, 0};
    source.vertices[2].texcoord = {-1, 2};
    // A seam has coincident source positions but a distinct UV coordinate.
    source.vertices.push_back(source.vertices[0]);
    source.vertices.back().texcoord = {1, 1};
    source.indices[3] = 3;
    const auto result = woby::uvLayoutMesh(source);
    REQUIRE(result.nodes.size() == 2);
    REQUIRE(result.indices.size() == 6);
    REQUIRE(result.vertices.size() == 4);
    CHECK(result.nodes[1].indexOffset == 3);
    CHECK(result.nodes[0].hasTexcoords);
    CHECK(result.vertices[0].texcoord == std::array<float, 2>{-1, 0});
    CHECK(result.vertices[0].position != result.vertices[3].position);
    CHECK(result.vertices[0].position[2] > result.vertices[2].position[2]);
    CHECK(result.bounds.max[0] - result.bounds.min[0] == doctest::Approx(1));
    CHECK(result.bounds.max[2] - result.bounds.min[2] == doctest::Approx(.5));
    CHECK(result.bounds.min[1] == result.bounds.max[1]);
    CHECK(source.vertices[0].position == source.vertices[3].position);
    CHECK(result.precisePositions.size() == result.vertices.size());
    for (auto& node : source.nodes) { node.hasTexcoords = false; }
    CHECK(woby::uvLayoutMesh(source).indices.empty());
    source.nodes[0].hasTexcoords = true;
    for (auto& vertex : source.vertices) { vertex.texcoord = {0, 0}; }
    const auto constant = woby::uvLayoutMesh(source);
    REQUIRE(constant.indices.size() == 3);
    CHECK(woby::finitePosition(constant.bounds.min));
    CHECK(woby::finitePosition(constant.bounds.max));
}

TEST_CASE("UV analyses keep source appearance independent and use only source computation")
{
    UvFixture f;
    auto& state = f.state;
    const auto fileId = state.files[0].objectId;
    const auto before = woby::createSceneDocument(state).files;
    woby::selectSceneObject(state, fileId);
    REQUIRE(woby::compareSceneSelection(state, woby::AnalysisType::uv));
    const auto id = state.comparisons.back().objectId;
    CHECK(state.comparisons.back().b.empty());
    CHECK(woby::createSceneDocument(state).files == before);
    auto settings = woby::comparisonSettings(state, id);
    CHECK(settings.type == woby::AnalysisType::uv);
    CHECK(woby::effectiveComparisonSettings(state, id).mode == woby::ComparisonMode::original);
    CHECK(woby::requestedComparisonStages(settings, true, true) == woby::comparisonSource);
    for (size_t i = 0; i < woby::diagnosticCategoryCount; ++i) {
        CHECK_FALSE(woby::diagnosticAutoUpdate(settings, static_cast<woby::DiagnosticCategory>(i)));
    }
    const auto layout = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    REQUIRE(layout.nodes.size() == 2);
    const auto bounds = woby::comparisonDisplayBounds(state, id);
    REQUIRE(bounds);
    for (size_t k = 0; k < 3; ++k) {
        CHECK(bounds->min[k] == doctest::Approx(layout.bounds.min[k] + state.comparisons.back().translation[k]));
        CHECK(bounds->max[k] == doctest::Approx(layout.bounds.max[k] + state.comparisons.back().translation[k]));
    }
    const auto signature = woby::comparisonGeometrySignature(state, id);
    settings.uvGrid.densityU = -1;
    settings.uvGrid.densityV = 1e9f;
    woby::setComparisonSettings(state, settings, id);
    CHECK(woby::comparisonSettings(state, id).uvGrid == woby::UvGridSettings{true, .1f, 1000});
    CHECK(woby::comparisonGeometrySignature(state, id) == signature);
    settings.uvView = woby::UvView::surface;
    woby::setComparisonSettings(state, settings, id);
    CHECK(woby::comparisonGeometrySignature(state, id) != signature);
    const auto surface = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
    REQUIRE(surface.nodes.size() == 3);
    CHECK(surface.nodes[0].hasTexcoords);
    CHECK_FALSE(surface.nodes[2].hasTexcoords);
    CHECK(surface.vertices[1].texcoord == state.files[0].mesh.vertices[1].texcoord);
    woby::setComparisonObjects(state, {fileId}, woby::ComparisonSide::b, true, id);
    woby::swapComparisonGroups(state, id);
    CHECK(state.comparisons.back().b.empty());
    CHECK(woby::createSceneDocument(state).files == before);
}

TEST_CASE("UV analysis type and view survive persistence saved views and undo redo")
{
    UvFixture f;
    auto& state = f.state;
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history;
    woby::resetSceneHistory(history, state);
    const auto id = woby::createComparison(state, woby::AnalysisType::uv);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK(state.comparisons.empty());
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
    REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(woby::comparisonSettings(state, id).type == woby::AnalysisType::uv);
    const auto view = woby::createView(state);
    auto settings = woby::comparisonSettings(state, id);
    settings.uvView = woby::UvView::surface;
    settings.uvGrid = {false, 7, 13};
    woby::setComparisonSettings(state, settings, id);
    woby::applyView(state, view);
    CHECK(woby::comparisonSettings(state, id).uvView == woby::UvView::layout);
    CHECK(woby::comparisonSettings(state, id).uvGrid == woby::UvGridSettings{true, 10, 10});
    woby::setComparisonSettings(state, settings, id);
    const auto document = woby::createSceneDocument(state);
    const auto path = f.root / "uv-analysis.woby";
    woby::writeSceneDocument(path, document);
    auto restored = woby::readSceneDocument(path);
    for (auto& file : restored.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
    CHECK(restored == document);
    std::ofstream(path) << "version = 17\n[[analyses]]\nname = \"Legacy\"\nanalysis_enabled = true\nanalysis_mode = \"distance\"\n";
    const auto legacy = woby::readSceneDocument(path);
    REQUIRE(legacy.comparisons.size() == 1);
    CHECK(legacy.comparisons[0].settings.type == woby::AnalysisType::mesh);
}

TEST_CASE("UV gradients preserve child overrides ranges views and saved scenes")
{
    UvFixture f;
    auto& state = f.state;
    REQUIRE(woby::setObjectUvGrid(state,{},true,{},{},woby::UvColorMode::u,-2.0f,3.0f));
    const auto child = state.files[0].groupSettings[1].objectId;
    REQUIRE(woby::setObjectUvGrid(state,{child},{},{},{},woby::UvColorMode::v,-1.0f,4.0f));
    woby::selectSceneObject(state,state.files[0].objectId);
    CHECK(woby::selectedObjectProperty(state,woby::UiObjectProperty::uvColorMode).mixed);
    woby::setSelectedObjectProperty(state,woby::UiObjectProperty::uvDensityU,8);
    CHECK(state.files[0].groupSettings[1].uvGrid.mode == woby::UvColorMode::v);
    CHECK(state.files[0].groupSettings[1].uvGrid.minimum == -1);
    const auto id = woby::createComparison(state,woby::AnalysisType::uvQuality);
    woby::setComparisonObjects(state,{state.files[0].objectId},woby::ComparisonSide::a,true,id);
    auto settings = woby::comparisonSettings(state,id);
    settings.uvSeparated = true; settings.uvLinkedSelection = false;
    settings.uvMetric = woby::UvQualityMetric::area; settings.uvNormalization = woby::UvAreaNormalization::absolute;
    woby::setComparisonSettings(state,settings,id);
    woby::isolateUvObjects(state,{child},id);
    const auto document = woby::createSceneDocument(state);
    const auto view = woby::createView(state);
    woby::isolateUvObjects(state,{},id);
    settings.uvSeparated = false; woby::setComparisonSettings(state,settings,id);
    REQUIRE(woby::setObjectUvGrid(state,{},false,{},{},woby::UvColorMode::grid));
    woby::applyView(state,view);
    CHECK(woby::comparisonSettings(state,id).uvSeparated);
    CHECK(woby::comparisonMemberIds(state,woby::ComparisonSide::a,id) == std::vector<woby::SceneObjectId>{child});
    CHECK(state.files[0].groupSettings[1].uvGrid.mode == woby::UvColorMode::v);
    auto saved = woby::createSceneDocument(state);
    const auto path = f.root / "extended-uv.woby";
    woby::writeSceneDocument(path,saved);
    auto restored = woby::readSceneDocument(path);
    for (auto& file : restored.files) { file.path = woby::sceneAbsolutePath(path,file.path); }
    CHECK(restored == saved);
    woby::SceneHistory history; woby::resetSceneHistory(history,state);
    settings = woby::comparisonSettings(state,id);
    settings.uvSeparated = false; settings.uvMetric = woby::UvQualityMetric::angle;
    woby::setComparisonSettings(state,settings,id);
    woby::isolateUvObjects(state,{},id);
    REQUIRE(woby::setObjectUvGrid(state,{},true,{},{},woby::UvColorMode::grid));
    const auto changed = woby::createSceneDocument(state);
    REQUIRE(woby::recordSceneHistory(history,state));
    auto step = woby::prepareSceneHistoryStep(history,state,document,false);
    REQUIRE(step);
    woby::commitSceneHistoryStep(history,state,std::move(*step),false);
    CHECK(woby::createSceneDocument(state) == saved);
    step = woby::prepareSceneHistoryStep(history,state,document,true);
    REQUIRE(step);
    woby::commitSceneHistoryStep(history,state,std::move(*step),true);
    CHECK(woby::createSceneDocument(state) == changed);
    CHECK_FALSE(woby::setObjectUvGrid(state,{},true,{},{},woby::UvColorMode::u,9.0f,2.0f));
    CHECK(woby::createSceneDocument(state) == changed);
    CHECK(woby::normalizedUvGrid({true,10,10,woby::UvColorMode::u,5,4}).minimum == 0);
}

TEST_CASE("Separated UV patches preserve source identities scale and UVs for linked picking")
{
    UvFixture f;
    auto& state = f.state;
    const auto id = woby::createComparison(state,woby::AnalysisType::uvQuality);
    woby::setComparisonObjects(state,{state.files[0].objectId},woby::ComparisonSide::a,true,id);
    auto settings = woby::comparisonSettings(state,id);
    settings.uvView = woby::UvView::layout; settings.uvSeparated = true;
    woby::setComparisonSettings(state,settings,id);
    const auto mesh = woby::comparisonWorldMesh(state,woby::ComparisonSide::a,id);
    REQUIRE(mesh.nodes.size() == 2);
    REQUIRE(mesh.vertices.size() == 6);
    CHECK(mesh.vertices[0].position != mesh.vertices[3].position);
    CHECK(mesh.vertices[0].texcoord == mesh.vertices[3].texcoord);
    CHECK(mesh.vertices[1].position[0]-mesh.vertices[0].position[0] == doctest::Approx(mesh.vertices[4].position[0]-mesh.vertices[3].position[0]));
    CHECK(mesh.nodes[0].sourceObjectId == state.files[0].groupSettings[0].objectId);
    const auto result = woby::computeComparisonStages(mesh,{},woby::comparisonSource);
    REQUIRE(result.original.source.uvQuality);
    CHECK(result.original.source.uvQuality->missing == 1);
    CHECK(woby::uvQualityVertices(result.original.source).size() == mesh.indices.size());
    const auto report = woby::controlComparisonResults(result,.01,false);
    CHECK(report["uvQuality"]["validTriangles"] == 2);
    CHECK(report["uvQuality"]["missingUvTriangles"] == 1);
    std::vector<woby::ScenePickPart> parts;
    woby::appendComparisonPickParts(parts,state.comparisons[0],settings,result,false);
    REQUIRE(parts.size() == 2);
    CHECK(parts[0].objectId == mesh.nodes[0].sourceObjectId);
    CHECK(parts[1].objectId == mesh.nodes[1].sourceObjectId);
    CHECK(parts[1].indexOffset == 3);
    settings.uvLinkedSelection = false; parts.clear();
    woby::appendComparisonPickParts(parts,state.comparisons[0],settings,result,false);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].objectId == id);
    const auto signature = woby::comparisonGeometrySignature(state,id);
    settings.uvMetric = woby::UvQualityMetric::area; woby::setComparisonSettings(state,settings,id);
    CHECK(woby::comparisonGeometrySignature(state,id) != signature);
    const auto source = woby::createSceneDocument(state).files;
    woby::isolateUvObjects(state,{mesh.nodes[0].sourceObjectId},id);
    CHECK(woby::comparisonWorldMesh(state,woby::ComparisonSide::a,id).nodes.size() == 1);
    CHECK(woby::createSceneDocument(state).files == source);
    woby::isolateUvObjects(state,{},id);
    CHECK(woby::comparisonMemberIds(state,woby::ComparisonSide::a,id).size() == 3);
}

TEST_CASE("UV quality CLI controls validate types and isolate without changing membership")
{
    UvFixture f;
    const auto clean = woby::createSceneDocument(f.state);
    const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
    const auto& create = woby::controlMethod(woby::ControlAction::comparisonCreate);
    auto command = woby::parseControlOperation(create,{{"type","uv_quality"},{"a","file"}});
    command.aId = f.state.files[0].objectId;
    (void)woby::applyControlSceneOperation(f.state,clean,command,format,200,800);
    const auto id = f.state.comparisons.back().objectId;
    const auto& set = woby::controlMethod(woby::ControlAction::comparisonSet);
    command = woby::parseControlOperation(set,{{"target",format(id)},{"uvMetric","area"},{"uvNormalization","absolute"},{"uvSeparated",true},{"uvLinkedSelection",false}});
    command.objectId = id;
    (void)woby::applyControlSceneOperation(f.state,clean,command,format,200,800);
    CHECK(woby::comparisonSettings(f.state,id).uvMetric == woby::UvQualityMetric::area);
    CHECK(woby::controlOperationParams(command)["uvNormalization"] == "absolute");
    CHECK_THROWS(woby::parseControlOperation(set,{{"target",format(id)},{"uvMetric","distance"}}));
    CHECK_THROWS(woby::parseControlOperation(create,{{"type","uv_quality"},{"b","file"}}));
    const auto& enable = woby::controlMethod(woby::ControlAction::comparisonEnable);
    command = woby::parseControlOperation(enable,{{"target",format(id)},{"side","a"},{"object","patch"},{"enabled",true},{"isolate",true}});
    command.objectId = id; command.memberId = f.state.files[0].groupSettings[0].objectId;
    (void)woby::applyControlSceneOperation(f.state,clean,command,format,200,800);
    CHECK(woby::comparisonMemberIds(f.state,woby::ComparisonSide::a,id).size() == 1);
    CHECK(woby::comparisonMemberIds(f.state,woby::ComparisonSide::a,id,false).size() == 3);
    CHECK_THROWS(woby::parseControlOperation(enable,{{"target",format(id)},{"side","a"},{"enabled",true},{"isolate",true}}));
}

TEST_CASE("UV layouts keep world geometry and framing bounds when the scene up axis changes")
{
    UvFixture f;
    auto& state = f.state;
    woby::setFileTranslation(state.files[0].fileSettings, {3, 4, 5});
    const auto layoutId = woby::createComparison(state, woby::AnalysisType::uv);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, layoutId);
    woby::setComparisonTranslation(state, layoutId, {-7, 8, -9});
    const auto surfaceId = woby::duplicateComparison(state, layoutId);
    auto surfaceSettings = woby::comparisonSettings(state, surfaceId);
    surfaceSettings.uvView = woby::UvView::surface;
    woby::setComparisonSettings(state, surfaceSettings, surfaceId);
    const auto meshId = woby::createComparison(state);
    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, meshId);
    const auto surfaceSignature = woby::comparisonGeometrySignature(state, surfaceId);
    const auto meshSignature = woby::comparisonGeometrySignature(state, meshId);
    auto layoutSignature = woby::comparisonGeometrySignature(state, layoutId);
    REQUIRE(surfaceSignature != 0);
    REQUIRE(meshSignature != 0);
    for (const auto up : {woby::SceneUpAxis::y, woby::SceneUpAxis::z, woby::SceneUpAxis::y}) {
        woby::setSceneUpAxis(state, up);
        CHECK(woby::comparisonGeometrySignature(state, layoutId) == layoutSignature);
        CHECK(woby::comparisonGeometrySignature(state, surfaceId) == surfaceSignature);
        CHECK(woby::comparisonGeometrySignature(state, meshId) == meshSignature);
        auto refreshed = state;
        woby::recalculateSceneBounds(refreshed);
        CHECK(state.sceneBounds.min == refreshed.sceneBounds.min);
        CHECK(state.sceneBounds.max == refreshed.sceneBounds.max);
        const auto layout = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, layoutId);
        const std::array<std::array<float, 3>, 3> positions = {{{3, 4.5f, 5.5f}, {4, 4.5f, 5.5f}, {3, 4.5f, 4.5f}}};
        REQUIRE(layout.vertices.size() == 6);
        for (size_t i = 0; i < layout.vertices.size(); ++i) {
            CHECK(layout.vertices[i].position == positions[i % 3]);
            CHECK(layout.vertices[i].normal == std::array<float, 3>{0, -1, 0});
            CHECK(layout.vertices[i].texcoord == state.files[0].mesh.vertices[i % 3].texcoord);
            for (size_t k = 0; k < 3; ++k) {
                CHECK(layout.precisePositions[i][k] == layout.vertices[i].position[k]);
            }
        }
        const auto display = woby::comparisonDisplayBounds(state, layoutId);
        REQUIRE(display);
        woby::selectSceneObject(state, layoutId);
        const auto selected = woby::selectedSceneBounds(state);
        REQUIRE(selected);
        for (size_t k = 0; k < 3; ++k) {
            CHECK(display->min[k] == layout.bounds.min[k] + state.comparisons[0].translation[k]);
            CHECK(display->max[k] == layout.bounds.max[k] + state.comparisons[0].translation[k]);
            CHECK(selected->min[k] == display->min[k]);
            CHECK(selected->max[k] == display->max[k]);
            CHECK(state.sceneBounds.min[k] <= display->min[k]);
            CHECK(state.sceneBounds.max[k] >= display->max[k]);
        }
        CHECK(state.camera.target == state.sceneBounds.center);
        const auto revision = state.sceneEditRevision;
        woby::setSceneUpAxis(state, up);
        CHECK(state.sceneEditRevision == revision);
        CHECK(woby::comparisonGeometrySignature(state, layoutId) == layoutSignature);
    }
}

TEST_CASE("Every UV visualization follows the selected up axis without changing UV values")
{
    for (const auto type : {woby::AnalysisType::uv, woby::AnalysisType::uvQuality}) {
        for (const auto view : {woby::UvView::layout, woby::UvView::surface}) {
            for (const bool separated : {false, true}) {
                for (int mode = 0; mode < 3; ++mode) {
                    CAPTURE(type);
                    CAPTURE(view);
                    CAPTURE(separated);
                    CAPTURE(mode);
                    UvFixture f;
                    auto& state = f.state;
                    // Nonzero distortion/stretch and opposite patch winding make
                    // every quality color meaningful in the axis comparison.
                    auto& source = state.files[0].mesh;
                    source.vertices[1].texcoord = {2, 0};
                    source.vertices[2].texcoord = {0, .25f};
                    std::swap(source.indices[4], source.indices[5]);
                    woby::setFileTranslation(state.files[0].fileSettings, {3, 4, 5});
                    const auto id = woby::createComparison(state, type);
                    woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
                    auto settings = woby::comparisonSettings(state, id);
                    settings.uvView = view;
                    settings.uvSeparated = separated;
                    settings.uvGrid.mode = static_cast<woby::UvColorMode>(mode);
                    settings.uvMetric = static_cast<woby::UvQualityMetric>(mode);
                    settings.uvNormalization = woby::UvAreaNormalization::absolute;
                    woby::setComparisonSettings(state, settings, id);
                    woby::setComparisonTranslation(state, id, {-7, 8, -9});
                    woby::setSceneUpAxis(state, woby::SceneUpAxis::y);
                    const auto yMesh = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
                    const auto yColors = woby::uvQualityVertices(yMesh);
                    auto signature = woby::comparisonGeometrySignature(state, id);
                    for (const auto up : {woby::SceneUpAxis::z, woby::SceneUpAxis::y}) {
                        woby::setSceneUpAxis(state, up);
                        const auto nextSignature = woby::comparisonGeometrySignature(state, id);
                        CHECK(nextSignature == signature);
                        signature = nextSignature;
                        const auto mesh = woby::comparisonWorldMesh(state, woby::ComparisonSide::a, id);
                        REQUIRE(mesh.vertices.size() == yMesh.vertices.size());
                        CHECK(mesh.indices == yMesh.indices);
                        for (size_t i = 0; i < mesh.vertices.size(); ++i) {
                            CHECK(mesh.vertices[i].position == yMesh.vertices[i].position);
                            CHECK(mesh.vertices[i].normal == yMesh.vertices[i].normal);
                            CHECK(mesh.vertices[i].texcoord == yMesh.vertices[i].texcoord);
                        }
                        const auto colors = woby::uvQualityVertices(mesh);
                        REQUIRE(colors.size() == yColors.size());
                        for (size_t i = 0; i < colors.size(); ++i) {
                            CHECK(colors[i].position == mesh.vertices[mesh.indices[i]].position);
                            CHECK(colors[i].texcoord == yColors[i].texcoord);
                        }
                        const auto bounds = woby::comparisonDisplayBounds(state, id);
                        REQUIRE(bounds);
                        const auto* comparison = woby::findComparison(state, id);
                        REQUIRE(comparison);
                        for (size_t k = 0; k < 3; ++k) {
                            CHECK(bounds->min[k] == doctest::Approx(mesh.bounds.min[k] + comparison->translation[k]));
                            CHECK(bounds->max[k] == doctest::Approx(mesh.bounds.max[k] + comparison->translation[k]));
                        }
                        const auto result = woby::computeComparisonStages(mesh, {}, woby::comparisonSource);
                        std::vector<woby::ScenePickPart> parts;
                        woby::appendComparisonPickParts(parts, *comparison, settings, result, true);
                        REQUIRE(parts.size() == mesh.nodes.size());
                        const auto outlines = woby::sceneSelectionLines(parts);
                        REQUIRE_FALSE(outlines.empty());
                        for (const auto& point : outlines) {
                            for (size_t k = 0; k < 3; ++k) {
                                CHECK(point[k] >= bounds->min[k] - 1e-5f);
                                CHECK(point[k] <= bounds->max[k] + 1e-5f);
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Separated UV patch rows stay in the world XZ plane")
{
    UvFixture f;
    auto source = f.state.files[0].mesh;
    const auto patch = source.nodes[0];
    source.nodes.assign(5, patch);
    const auto layout = woby::uvLayoutMesh(source, true);
    REQUIRE(layout.nodes.size() == 5);
    const auto& first = layout.vertices[layout.indices[layout.nodes[0].indexOffset]].position;
    const auto& nextColumn = layout.vertices[layout.indices[layout.nodes[1].indexOffset]].position;
    const auto& nextRow = layout.vertices[layout.indices[layout.nodes[3].indexOffset]].position;
    CHECK(nextColumn[0] > first[0]);
    CHECK(nextColumn[2] == first[2]);
    CHECK(nextRow[0] == first[0]);
    CHECK(nextRow[2] > first[2]);
    CHECK(layout.bounds.min[1] == layout.bounds.max[1]);
}

TEST_CASE("UV rectangles turn with their source instead of staying upright when Y becomes up")
{
    woby::Mesh source;
    source.vertices = {{{5, 3, 8}, {}, {0, 1}}, {{7, 3, 8}, {}, {.5f, 1}},
        {{7, 3, 12}, {}, {.5f, 0}}, {{5, 3, 12}, {}, {0, 0}}};
    source.indices = {0, 1, 2, 0, 2, 3};
    source.nodes = {{"rectangle", 0, 6}};
    source.nodes[0].hasTexcoords = true;
    source.bounds = woby::calculateBounds(source.vertices);
    const auto layout = woby::uvLayoutMesh(source);
    REQUIRE(layout.vertices.size() == source.vertices.size());
    for (const auto up : {woby::SceneUpAxis::z, woby::SceneUpAxis::y}) {
        for (const auto direction : {woby::CameraView::front, woby::CameraView::top, woby::CameraView::isometric}) {
            const auto camera = woby::cameraWithView(woby::frameCameraBounds(source.bounds, up), direction);
            const auto view = woby::scenePickView(camera, up, source.bounds, 800, 600, false, 1);
            const auto project = [&](const std::array<float, 3>& position) {
                const float point[4] = {position[0], position[1], position[2], 1};
                float eye[4], clip[4];
                bx::vec4MulMtx(eye, point, view.view.data());
                bx::vec4MulMtx(clip, eye, view.projection.data());
                return std::array<float, 2>{clip[0] / clip[3], clip[1] / clip[3]};
            };
            for (size_t i = 0; i < source.vertices.size(); ++i) {
                const auto original = project(source.vertices[i].position);
                const auto flattened = project(layout.vertices[i].position);
                CHECK(flattened[0] == doctest::Approx(original[0]));
                CHECK(flattened[1] == doctest::Approx(original[1]));
            }
            if (direction == woby::CameraView::front) {
                const auto bottom = project(layout.vertices[0].position);
                const auto top = project(layout.vertices[3].position);
                if (up == woby::SceneUpAxis::y) {
                    CHECK(top[1] == doctest::Approx(bottom[1]));
                } else {
                    CHECK(top[1] > bottom[1] + .1f);
                }
            }
        }
    }
}

TEST_CASE("UV layout orientation survives scene reload saved views and undo redo")
{
    for (const auto up : {woby::SceneUpAxis::y, woby::SceneUpAxis::z}) {
        UvFixture f;
        auto& state = f.state;
        const auto opposite = up == woby::SceneUpAxis::y ? woby::SceneUpAxis::z : woby::SceneUpAxis::y;
        woby::setSceneUpAxis(state, opposite);
        const auto id = woby::createComparison(state, woby::AnalysisType::uv);
        woby::setComparisonObjects(state, {state.files[0].objectId}, woby::ComparisonSide::a, true, id);
        const auto clean = woby::createSceneDocument(state);
        woby::SceneHistory history;
        woby::resetSceneHistory(history, state);
        woby::setSceneUpAxis(state, up);
        REQUIRE(woby::recordSceneHistory(history, state));
        auto undo = woby::prepareSceneHistoryStep(history, state, clean, false);
        REQUIRE(undo);
        woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
        CHECK(state.upAxis == opposite);
        auto redo = woby::prepareSceneHistoryStep(history, state, clean, true);
        REQUIRE(redo);
        woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
        CHECK(state.upAxis == up);
        const auto view = woby::createView(state);
        woby::setSceneUpAxis(state, opposite);
        woby::applyView(state, view);
        CHECK(state.upAxis == up);
        const auto path = f.root / "uv-orientation.woby";
        woby::writeSceneDocument(path, woby::createSceneDocument(state));
        auto document = woby::readSceneDocument(path);
        for (auto& file : document.files) { file.path = woby::sceneAbsolutePath(path, file.path); }
        const auto restored = woby::prepareSceneReplacement(state, state.files, document);
        CHECK(restored.upAxis == up);
        const auto checkLayout = [&](const woby::UiState& current, woby::SceneObjectId currentId) {
            const auto mesh = woby::comparisonWorldMesh(current, woby::ComparisonSide::a, currentId);
            CHECK(mesh.bounds.max[2] - mesh.bounds.min[2] == doctest::Approx(1));
            CHECK(mesh.bounds.max[1] == mesh.bounds.min[1]);
            CHECK(mesh.vertices[0].position[2] > mesh.vertices[2].position[2]);
        };
        checkLayout(state, id);
        checkLayout(restored, restored.activeComparisonId);
    }
}

TEST_CASE("UV analysis controls validate types and reject incompatible edits atomically")
{
    UvFixture f;
    const auto clean = woby::createSceneDocument(f.state);
    const auto format = [](woby::SceneObjectId id) { return std::to_string(id); };
    const auto& create = woby::controlMethod(woby::ControlAction::comparisonCreate);
    CHECK_THROWS(woby::parseControlOperation(create, {{"type", "unknown"}}));
    CHECK_THROWS(woby::parseControlOperation(create, {{"type", "uv"}, {"b", "1"}}));
    auto command = woby::parseControlOperation(create, {{"type", "uv"}, {"a", format(f.state.files[0].objectId)}});
    command.aId = f.state.files[0].objectId;
    (void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800);
    const auto id = f.state.comparisons.back().objectId;
    const auto& set = woby::controlMethod(woby::ControlAction::comparisonSet);
    command = woby::parseControlOperation(set, {{"target", format(id)}, {"uvView", "surface"}, {"uvDensityU", 5}, {"uvDensityV", 9}});
    command.objectId = id;
    CHECK(woby::controlOperationParams(command)["uvView"] == "surface");
    (void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800);
    CHECK(woby::comparisonSettings(f.state, id).uvView == woby::UvView::surface);
    CHECK(woby::comparisonSettings(f.state, id).uvGrid == woby::UvGridSettings{true, 5, 9});
    const auto before = woby::createSceneDocument(f.state);
    command.mode = "distance";
    command.name = "Should not rename";
    CHECK_THROWS_AS((void)woby::applyControlSceneOperation(f.state, clean, command, format, 200, 800), std::invalid_argument);
    CHECK(woby::createSceneDocument(f.state) == before);
    CHECK_THROWS(woby::parseControlOperation(set, {{"target", format(id)}, {"uvView", "invalid"}}));
}
