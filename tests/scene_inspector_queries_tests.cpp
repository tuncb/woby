#include "scene_inspector.h"
#include <imgui.h>
#include <algorithm>
#include "comparison_runtime.h"
#include "scene_history.h"
#include "automation_registry.h"
#include "allocation_probe.h"
#include "scene_draw_plan.h"

#include <doctest/doctest.h>
#include <chrono>
#include <iostream>

namespace {
using namespace woby;
struct PropertiesFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-properties-" + automationRandomHex(8));
    UiState state;
    SceneInspectorRuntime runtime;
    explicit PropertiesFixture(size_t groups = 4, bool colored = false) {
        std::filesystem::create_directory(root);
        Mesh mesh;
        editMeshGeometry(mesh).vertices = {{{0,0,0}, {}, {}}, {{2,0,0}, {}, {}}, {{0,3,0}, {}, {}}};
        if (colored) {
            editMeshGeometry(mesh).vertices.resize(6);
            editMeshGeometry(mesh).vertexColors = {0xff000000u, 0xff0000ffu, 0, 0, 0, 0};
        }
        mesh.bounds = calculateBounds(meshGeometry(mesh).vertices);
        for (size_t i = 0; i < groups; ++i) {
            editMeshGeometry(mesh).nodes.push_back({"part" + std::to_string(i), static_cast<uint32_t>(meshGeometry(mesh).indices.size()), 3});
            editMeshGeometry(mesh).nodes.back().hasTexcoords = (i % 2 == 0);
            if (colored && i >= 2) { editMeshGeometry(mesh).indices.insert(meshGeometry(mesh).indices.end(), {3,4,5}); }
            else { editMeshGeometry(mesh).indices.insert(meshGeometry(mesh).indices.end(), {0,1,2}); }
        }
        state.files.push_back(createUiFileState(root / "source.obj", std::move(mesh), 0));
        appendFolderTreeSceneNode(state, root, 0, 1);
        selectSceneObject(state, state.files.front().objectId);
    }
    ~PropertiesFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    void check() {
        updateSceneInspector(runtime, state);
        for (size_t p = 0; p < uiObjectPropertyCount; ++p) {
            CAPTURE(p);
            const auto expected = selectedObjectProperty(state, static_cast<UiObjectProperty>(p));
            const auto actual = runtime.snapshot.properties[p];
            CHECK(actual.available == expected.available);
            CHECK(actual.mixed == expected.mixed);
            CHECK(actual.value == expected.value);
        }
        const auto visibility = selectedObjectVisibility(state);
        CHECK(runtime.snapshot.visibility.available == visibility.available);
        CHECK(runtime.snapshot.visibility.value == visibility.value);
        CHECK(runtime.snapshot.visibility.mixed == visibility.mixed);
        std::vector<ScenePickPart> parts;
        scenePickParts(state, parts);
        const auto dimensions = sceneDimensions(parts);
        REQUIRE(runtime.snapshot.dimensions.has_value() == dimensions.has_value());
        if (dimensions) { CHECK(runtime.snapshot.dimensions->lengths == dimensions->lengths); }
    }
};
}

TEST_CASE("vertex color controls distinguish source custom mixed and uncolored parts") {
    PropertiesFixture f(3, true);
    const auto fileId = f.state.files[0].objectId;
    const auto first = f.state.files[0].groupSettings[0].objectId;
    const auto second = f.state.files[0].groupSettings[1].objectId;
    const auto plain = f.state.files[0].groupSettings[2].objectId;
    const auto source = [&] { f.check(); return inspectorProperty(f.runtime.snapshot, UiObjectProperty::vertexColors); };
    CHECK(source().available);
    CHECK(source().value == 1);
    CHECK_FALSE(source().mixed);
    selectSceneObject(f.state, plain);
    CHECK_FALSE(source().available);
    REQUIRE(setObjectColor(f.state, {plain}, std::array<float, 3>{.2f, .3f, .4f}));
    const auto plainColor = f.state.files[0].groupSettings[2].color;
    selectSceneObject(f.state, first);
    SceneDrawCache draw;
    REQUIRE(updateSceneDrawPlan(draw, f.state));
    CHECK(draw.plan.items[0].useVertexColors);
    const auto* colors = meshGeometry(f.state.files[0].mesh).vertexColors.data();
    const auto geometry = f.state.revisions.geometry;
    // Choosing the existing fallback still changes the displayed source.
    const auto red = f.state.files[0].groupSettings[0].color[0];
    clearSceneDirty(f.state);
    setSelectedObjectProperty(f.state, UiObjectProperty::red, red);
    CHECK(f.state.isDirty);
    CHECK(source().value == 0);
    REQUIRE(updateSceneDrawPlan(draw, f.state));
    CHECK_FALSE(draw.plan.items[0].useVertexColors);
    CHECK(f.state.revisions.geometry == geometry);
    CHECK(meshGeometry(f.state.files[0].mesh).vertexColors.data() == colors);
    clearSceneDirty(f.state);
    setSelectedObjectProperty(f.state, UiObjectProperty::red, red);
    CHECK_FALSE(f.state.isDirty);
    CHECK_FALSE(updateSceneDrawPlan(draw, f.state));
    selectSceneObject(f.state, fileId);
    CHECK(source().mixed);
    const std::array restore{InspectorEdit{InspectorEditKind::property, UiObjectProperty::vertexColors, 1, {}}};
    applyInspectorEdits(f.state, restore);
    CHECK_FALSE(source().mixed);
    CHECK(source().value == 1);
    CHECK(f.state.files[0].groupSettings[2].color == plainColor);
    CHECK(f.state.files[0].groupSettings[2].colorOverride);
    REQUIRE(setObjectColor(f.state, {fileId, first, second}, std::array<float, 3>{.2f, .3f, .4f}));
    CHECK(source().value == 0);
    CHECK_FALSE(source().mixed);
    resetSelectedObjectProperties(f.state, UiPropertyGroup::appearance);
    CHECK(source().value == 1);
    for (const auto& group : f.state.files[0].groupSettings) { CHECK_FALSE(group.colorOverride); }
}

TEST_CASE("color source changes survive undo redo and views without copying geometry") {
    PropertiesFixture f(1, true);
    const auto id = f.state.files[0].groupSettings[0].objectId;
    selectSceneObject(f.state, id);
    const auto vertexView = createView(f.state);
    SceneHistory history;
    resetSceneHistory(history, f.state);
    const auto clean = createSceneDocument(f.state);
    const auto* colors = meshGeometry(f.state.files[0].mesh).vertexColors.data();
    const auto color = f.state.files[0].groupSettings[0].color;
    REQUIRE(setObjectColor(f.state, {id}, std::array<float, 3>{color[0], color[1], color[2]}));
    recordSceneHistory(history, f.state);
    for (const bool redo : {false, true}) {
        auto prepared = prepareSceneHistoryStep(history, f.state, clean, redo);
        REQUIRE(prepared);
        commitSceneHistoryStep(history, f.state, std::move(*prepared), redo);
        CHECK(f.state.files[0].groupSettings[0].colorOverride == redo);
        CHECK(f.state.files[0].groupSettings[0].hasVertexColors);
        CHECK(meshGeometry(f.state.files[0].mesh).vertexColors.data() == colors);
        f.check();
    }
    const auto customView = createView(f.state);
    const auto path = f.root / "colors.woby";
    const auto document = createSceneDocument(f.state);
    writeSceneDocument(path, document);
    auto saved = readSceneDocument(path);
    for (auto& file : saved.files) { file.path = sceneAbsolutePath(path, file.path); }
    CHECK(saved == document);
    auto loaded = f.state.files;
    for (size_t i = 0; i < loaded.size(); ++i) {
        for (auto& group : loaded[i].groupSettings) { group.colorOverride = false; group.color = {}; }
        applySceneFileRecord(loaded[i], saved.files[i]);
    }
    auto restored = prepareSceneReplacement(f.state, std::move(loaded), saved);
    REQUIRE(restored.views.size() == 2);
    CHECK(restored.files[0].groupSettings[0].colorOverride);
    CHECK(restored.files[0].groupSettings[0].hasVertexColors);
    applyView(restored, restored.views[0].id);
    CHECK_FALSE(restored.files[0].groupSettings[0].colorOverride);
    applyView(restored, restored.views[1].id);
    CHECK(restored.files[0].groupSettings[0].colorOverride);
    applyView(f.state, vertexView);
    CHECK_FALSE(f.state.files[0].groupSettings[0].colorOverride);
    applyView(f.state, customView);
    CHECK(f.state.files[0].groupSettings[0].colorOverride);
    REQUIRE(setObjectColor(f.state, {id}, std::nullopt));
    CHECK_FALSE(f.state.files[0].groupSettings[0].colorOverride);
}

TEST_CASE("properties snapshot matches queries for overlapping mixed and unsupported selections") {
    PropertiesFixture f;
    SUBCASE("file") {}
    SUBCASE("folder") { selectSceneObject(f.state, f.state.sceneNodes.front().objectId); }
    SUBCASE("overlapping file and part") {
        selectSceneObject(f.state, f.state.files[0].groupSettings[0].objectId, true);
    }
    SUBCASE("parts in selection order") {
        clearSceneSelection(f.state);
        selectSceneObject(f.state, f.state.files[0].groupSettings[2].objectId);
        selectSceneObject(f.state, f.state.files[0].groupSettings[0].objectId, true);
    }
    SUBCASE("unsupported analysis") { selectSceneObject(f.state, createComparison(f.state)); }
    SUBCASE("missing object") { f.state.selectedSceneObjects = {999999}; }
    SUBCASE("empty selection") { clearSceneSelection(f.state); }
    f.state.files[0].groupSettings[0].opacity = .25f;
    f.state.files[0].groupSettings[0].uvGrid.densityU = 7;
    notifySceneEdit(f.state);
    f.check();
}

TEST_CASE("point cloud opacity is disabled for parts files and folders without disabling other properties") {
    PropertiesFixture f(1);
    auto& file = f.state.files[0];
    editMeshGeometry(file.mesh).indices.clear(); editMeshGeometry(file.mesh).pointIndices = {0,1,2};
    editMeshGeometry(file.mesh).nodes[0].indexCount = 0; editMeshGeometry(file.mesh).nodes[0].pointIndexCount = 3;
    file.groupSettings[0].showVertices = true;
    file.fileSettings.opacity = 0; file.groupSettings[0].opacity = .2f;
    notifySceneEdit(f.state, SceneChange::geometry);
    for (const auto id : {file.groupSettings[0].objectId, file.objectId, f.state.sceneNodes[0].objectId}) {
        selectSceneObject(f.state, id); f.check();
        CHECK_FALSE(inspectorProperty(f.runtime.snapshot, UiObjectProperty::opacity).available);
        CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::scale).available);
        CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::vertexSize).available);
        CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::red).available);
        CHECK(f.runtime.snapshot.dimensions.has_value());
        const auto before = createSceneDocument(f.state);
        clearSceneDirty(f.state);
        setSelectedObjectProperty(f.state, UiObjectProperty::opacity, .5f);
        CHECK(createSceneDocument(f.state) == before); CHECK_FALSE(f.state.isDirty);
    }
    selectSceneObject(f.state, file.groupSettings[0].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::vertexSize, 3);
    setSelectedObjectProperty(f.state, UiObjectProperty::red, .1f);
    resetSelectedObjectProperties(f.state, UiPropertyGroup::appearance);
    CHECK(file.groupSettings[0].vertexSizeScale == 1);
    CHECK(file.groupSettings[0].color == defaultGroupColor(0));
    CHECK(file.groupSettings[0].showVertices);
}

TEST_CASE("mixed point cloud and surface opacity edits affect only eligible selected objects") {
    PropertiesFixture f(1);
    auto surface = f.state.files[0];
    auto& cloud = f.state.files[0];
    editMeshGeometry(cloud.mesh).indices.clear(); editMeshGeometry(cloud.mesh).pointIndices = {0,1,2};
    editMeshGeometry(cloud.mesh).nodes[0].indexCount = 0; editMeshGeometry(cloud.mesh).nodes[0].pointIndexCount = 3;
    cloud.groupSettings[0].showVertices = true;
    cloud.fileSettings.opacity = .2f;
    surface.path = f.root / "surface.obj"; surface.objectId = 0;
    surface.groupSettings[0].objectId = 0;
    f.state.files.push_back(std::move(surface));
    appendDefaultSceneNodesForFiles(f.state, 1); assignSceneObjectIds(f.state);
    notifySceneEdit(f.state, SceneChange::geometry);
    for (const bool parts : {false, true}) {
        f.state.selectedSceneObjects.clear();
        for (const auto& file : f.state.files) {
            f.state.selectedSceneObjects.push_back(parts ? file.groupSettings[0].objectId : file.objectId);
        }
        f.check();
        const auto opacity = inspectorProperty(f.runtime.snapshot, UiObjectProperty::opacity);
        REQUIRE(opacity.available); CHECK_FALSE(opacity.mixed); CHECK(opacity.value == 1);
        setSelectedObjectProperty(f.state, UiObjectProperty::opacity, .4f); f.check();
        CHECK(f.state.files[0].fileSettings.opacity == .2f);
        CHECK(f.state.files[0].groupSettings[0].opacity == 1);
        CHECK((parts ? f.state.files[1].groupSettings[0].opacity : f.state.files[1].fileSettings.opacity) == .4f);
        setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 1);
    }
    f.state.sceneNodes[0].children.push_back(std::move(f.state.sceneNodes[1]));
    f.state.sceneNodes.resize(1); notifySceneEdit(f.state, SceneChange::geometry);
    selectSceneObject(f.state, f.state.sceneNodes[0].objectId); f.check();
    CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::opacity).available);
    setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 0);
    CHECK(scenePickParts(f.state).size() == 1);
}

TEST_CASE("properties snapshot has allocation-free hits and refreshes after operation boundaries") {
    PropertiesFixture f(2203);
    f.check();
    const auto builds = f.runtime.builds;
    const auto dimensions = f.runtime.dimensionBuilds;
    const auto* values = f.runtime.snapshot.targets.data();
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(woby::test::countAllocations([&] { for (int i = 0; i < 100; ++i) { updateSceneInspector(f.runtime, f.state); } }) == 0);
#endif
    orbitUiCamera(f.state, 10, 5);
    setPropertiesPaneVisible(f.state, false);
    f.check();
    CHECK(f.runtime.builds == builds);
    CHECK(f.runtime.snapshot.targets.data() == values);
    setSelectedObjectProperty(f.state, UiObjectProperty::red, .2f);
    f.check();
    CHECK(f.runtime.builds == builds + 1);
    CHECK(f.runtime.dimensionBuilds == dimensions);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 5);
    f.check();
    CHECK(f.runtime.dimensionBuilds == dimensions + 1);
    setSelectedObjectsVisible(f.state, false);
    f.check();
    CHECK_FALSE(f.runtime.snapshot.dimensions);
    setSelectedObjectsVisible(f.state, true);
    selectSceneObject(f.state, f.state.files[0].groupSettings.back().objectId);
    f.check();
    CHECK(f.runtime.snapshot.targets[0].name == "part2202");
}

TEST_CASE("properties dimensions follow zero opacity and disabled geometry modes") {
    PropertiesFixture f;
    f.check();
    REQUIRE(f.runtime.snapshot.dimensions);
    SUBCASE("zero opacity") { setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 0); }
    SUBCASE("no rendered primitives") {
        setSelectedObjectProperty(f.state, UiObjectProperty::solidMesh, 0);
        setSelectedObjectProperty(f.state, UiObjectProperty::triangles, 0);
        setSelectedObjectProperty(f.state, UiObjectProperty::vertices, 0);
    }
    f.check();
    CHECK_FALSE(f.runtime.snapshot.dimensions);
}

TEST_CASE("properties edits apply after the snapshot is consumed and survive history changes") {
    PropertiesFixture f;
    SceneHistory history;
    resetSceneHistory(history, f.state);
    const auto clean = createSceneDocument(f.state);
    f.check();
    const auto before = inspectorProperty(f.runtime.snapshot, UiObjectProperty::translationX);
    const std::array edits{InspectorEdit{InspectorEditKind::property, UiObjectProperty::translationX, 9, {}}};
    applyInspectorEdits(f.state, edits);
    CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::translationX).value == before.value);
    f.check();
    CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::translationX).value == 9);
    recordSceneHistory(history, f.state);
    auto undo = prepareSceneHistoryStep(history, f.state, clean, false);
    REQUIRE(undo);
    commitSceneHistoryStep(history, f.state, std::move(*undo), false);
    f.check();
    CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::translationX).value == 0);
    auto redo = prepareSceneHistoryStep(history, f.state, clean, true);
    REQUIRE(redo);
    commitSceneHistoryStep(history, f.state, std::move(*redo), true);
    f.check();
    CHECK(inspectorProperty(f.runtime.snapshot, UiObjectProperty::translationX).value == 9);
}

TEST_CASE("analysis input queries ignore appearance and refresh their exact dependencies") {
    PropertiesFixture f;
    const auto id = createAnalysisFromObjects(f.state, AnalysisTask::meshChecks, {f.state.files[0].objectId});
    selectSceneObject(f.state, f.state.files[0].objectId);
    ComparisonInspectorCache cache;
    updateComparisonInspectorCache(cache, f.state, id);
    const auto builds = cache.builds;
    const auto signatureBuilds = cache.signatureBuilds;
    const auto signature = cache.signature;
    setSelectedObjectProperty(f.state, UiObjectProperty::red, .1f);
    setSelectedObjectProperty(f.state, UiObjectProperty::opacity, .5f);
    setSelectedObjectsVisible(f.state, false);
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(cache.builds == builds);
    CHECK(cache.signature == comparisonGeometrySignature(f.state, id));
    renameComparison(f.state, id, "Renamed");
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(cache.builds == builds + 1);
    CHECK(cache.signatureBuilds == signatureBuilds);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationY, 6);
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(cache.signature != signature);
    CHECK(cache.signature == comparisonGeometrySignature(f.state, id));
    const auto transformed = cache.signature;
    auto& mesh = f.state.files[0].mesh;
    const auto* buffer = meshGeometry(mesh).vertices.data();
    editMeshGeometry(mesh).vertices[1].position[0] += 1;
    renewMeshContentRevision(mesh);
    notifySceneEdit(f.state, SceneChange::geometry);
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(meshGeometry(mesh).vertices.data() != buffer);
    CHECK(cache.signature != transformed);
    CHECK(cache.signature == comparisonGeometrySignature(f.state, id));
    publishMeshGeometry(mesh);
    const auto* published = meshGeometry(mesh).vertices.data();
    const auto previous = cache.signature;
    renewMeshContentRevision(mesh);
    notifySceneEdit(f.state, SceneChange::geometry);
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(meshGeometry(mesh).vertices.data() == published);
    CHECK(cache.signature != previous);
    clearComparisonGroup(f.state, ComparisonSide::a, id);
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(cache.inputs[0].summary.enabledPartCount == 0);
}

TEST_CASE("properties snapshot rebuilds for replacement documents and state owners") {
    PropertiesFixture f;
    f.check();
    const auto builds = f.runtime.builds;
    UiState copy = f.state;
    updateSceneInspector(f.runtime, copy);
    CHECK(f.runtime.builds == builds + 1);
    ++copy.sceneGeneration;
    copy.files[0].groupSettings[0].color[0] = .9f;
    updateSceneInspector(f.runtime, copy);
    CHECK(f.runtime.builds == builds + 2);
}

TEST_CASE("analysis readiness requires the uploaded revision of each requested stage") {
    ComparisonRuntime runtime;
    ComparisonSettings settings;
    runtime.results.signature = runtime.results.cache.signature = 42;
    runtime.results.cache.completed = runtime.gpu.uploadedStages = comparisonSource | comparisonQuality;
    runtime.results.stageRevisions[0] = runtime.gpu.stageRevisions[0] = 10;
    runtime.results.stageRevisions[4] = 12;
    runtime.gpu.stageRevisions[4] = 11;
    CHECK(comparisonStagesReady(runtime, settings, 42, comparisonSource, true));
    CHECK(comparisonStagesReady(runtime, settings, 42, comparisonQuality));
    CHECK_FALSE(comparisonStagesReady(runtime, settings, 42, comparisonQuality, true));
    runtime.gpu.stageRevisions[4] = 12;
    CHECK(comparisonStagesReady(runtime, settings, 42, comparisonQuality, true));
}

TEST_CASE("analysis fallback preserves the requested result side before derived buffers are ready") {
    PropertiesFixture f;
    const auto id = createComparison(f.state, AnalysisType::surfaceComparison);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, true, id);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::b, true, id);
    ComparisonRuntime runtime;
    for (const auto mode : {ComparisonMode::distance, ComparisonMode::surfaceQuality}) {
        for (const bool original : {false, true}) {
            auto settings = comparisonSettings(f.state, id);
            settings.mode = mode;
            settings.distanceOnOriginal = settings.quality.onOriginal = original;
            setComparisonSettings(f.state, settings, id);
            const auto effective = effectiveComparisonSettings(f.state, id);
            const bool displayOriginal = mode == ComparisonMode::distance ? effective.distanceOnOriginal : effective.quality.onOriginal;
            const auto ready = readyComparisonSettings(runtime, f.state, id);
            CHECK(ready.mode == (displayOriginal ? ComparisonMode::original : ComparisonMode::repaired));
        }
    }
}

TEST_CASE("properties unchanged query benchmark" * doctest::skip()) {
    const auto previous = ImGui::GetCurrentContext();
    auto* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = {1524, 1664};
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    for (const size_t groups : {1u, 256u, 2203u}) {
        for (const bool multi : {false, true}) {
            PropertiesFixture f(groups);
            if (multi) {
                f.state.selectedSceneObjects.clear();
                for (const auto& part : f.state.files[0].groupSettings) { f.state.selectedSceneObjects.push_back(part.objectId); }
            }
            updateSceneInspector(f.runtime, f.state);
            const auto start = std::chrono::steady_clock::now();
            for (size_t i = 0; i < 10000; ++i) { updateSceneInspector(f.runtime, f.state); }
            const auto queryUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 10000;
            std::vector<double> samples;
            for (int i = 0; i < 12; ++i) {
                ImGui::NewFrame();
                ImGui::SetNextWindowSize({420, 1500});
                ImGui::Begin("Properties benchmark");
                const auto before = std::chrono::steady_clock::now();
                drawSceneInspector(f.state, f.runtime);
                const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - before).count();
                if (i >= 3) { samples.push_back(ms); }
                ImGui::End();
                ImGui::EndFrame();
            }
            std::sort(samples.begin(), samples.end());
            std::cout << "properties groups=" << groups << " selection=" << (multi ? "all_parts" : "file")
                << " query_us=" << queryUs << " draw_ms=" << samples[samples.size() / 2]
                << " builds=" << f.runtime.builds << '\n';
        }
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
