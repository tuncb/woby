#include "scene_inspector.h"
#include <imgui.h>
#include <algorithm>
#include "comparison_runtime.h"
#include "scene_history.h"
#include "automation_registry.h"
#include "allocation_probe.h"

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
    explicit PropertiesFixture(size_t groups = 4) {
        std::filesystem::create_directory(root);
        Mesh mesh;
        mesh.vertices = {{{0,0,0}, {}, {}}, {{2,0,0}, {}, {}}, {{0,3,0}, {}, {}}};
        mesh.bounds = calculateBounds(mesh.vertices);
        for (size_t i = 0; i < groups; ++i) {
            mesh.nodes.push_back({"part" + std::to_string(i), static_cast<uint32_t>(mesh.indices.size()), 3});
            mesh.nodes.back().hasTexcoords = (i % 2 == 0);
            mesh.indices.insert(mesh.indices.end(), {0,1,2});
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
    const auto* buffer = mesh.vertices.data();
    mesh.vertices[1].position[0] += 1;
    renewMeshContentRevision(mesh);
    notifySceneEdit(f.state, SceneChange::geometry);
    updateComparisonInspectorCache(cache, f.state, id);
    CHECK(mesh.vertices.data() == buffer);
    CHECK(cache.signature != transformed);
    CHECK(cache.signature == comparisonGeometrySignature(f.state, id));
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
