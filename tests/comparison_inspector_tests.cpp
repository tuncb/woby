#include "comparison_view.h"
#include "ui_operations.h"
#include "scene_history.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <iostream>

namespace {
using namespace woby;
struct InspectorFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-inspector-" + automationRandomHex(8));
    UiState state;
    ComparisonRuntimes runtimes;
    SceneObjectId id = 0;
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();

    explicit InspectorFixture(size_t groups = 2) {
        std::filesystem::create_directory(root);
        Mesh mesh;
        mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
        mesh.bounds = calculateBounds(mesh.vertices);
        for (size_t i = 0; i < groups; ++i) {
            mesh.nodes.push_back({"part" + std::to_string(i), static_cast<uint32_t>(mesh.indices.size()), 3});
            mesh.indices.insert(mesh.indices.end(), {0, 1, 2});
        }
        state.files.push_back(createUiFileState(root / "source.obj", std::move(mesh), 0));
        appendFolderTreeSceneNode(state, root, 0, 1);
        id = createAnalysisFromObjects(state, AnalysisTask::meshChecks, {state.files[0].objectId});
        selectSceneObject(state, id);
        auto& runtime = runtimes.objects[id];
        runtime.gpu.ready = true;
        runtime.results.signature = comparisonGeometrySignature(state, id);
        runtime.results.cache = {runtime.results.signature, comparisonSource | comparisonDetectors};
        runtime.results.cache.topologyMode = comparisonSettings(state, id).topologyMode;
        for (auto& detector : runtime.results.value.detectors) { detector.phase = IntersectionPhase::complete; }
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1524, 1664};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~InspectorFixture() {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    const ComparisonInspectorCache& queries() {
        return updateComparisonInspectorCache(runtimes.objects[id].inspector.queries, state, id);
    }
    void expandSources() {
        for (auto* window : context->Windows) {
            if (std::string_view(window->Name).find("comparison_properties") == std::string_view::npos) { continue; }
            const auto sourcesId = ImHashStr("Sources", 0, window->ID);
            window->StateStorage.SetBool(ImHashStr("root", 0, sourcesId), true);
        }
    }
    std::string frame(bool log = false) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({420, 1500});
        ImGui::Begin("Properties probe");
        if (log) { ImGui::LogToBuffer(); }
        drawComparisonPanelContents(state, runtimes);
        const std::string contents = log ? context->LogBuffer.c_str() : "";
        if (log) { ImGui::LogFinish(); }
        ImGui::End();
        ImGui::EndFrame();
        return contents;
    }
};

void checkTree(const std::vector<ComparisonTreeNode>& actual, const std::vector<ComparisonTreeNode>& expected) {
    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        CHECK(actual[i].objectId == expected[i].objectId);
        CHECK(actual[i].name == expected[i].name);
        CHECK(actual[i].kind == expected[i].kind);
        CHECK(actual[i].partCount == expected[i].partCount);
        CHECK(actual[i].enabledPartCount == expected[i].enabledPartCount);
        CHECK(actual[i].triangleCount == expected[i].triangleCount);
        checkTree(actual[i].children, expected[i].children);
    }
}
void checkQueries(InspectorFixture& f) {
    const auto& cache = f.queries();
    CHECK(cache.signature == comparisonGeometrySignature(f.state, f.id));
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        const auto& input = cache.inputs[side == ComparisonSide::a ? 0 : 1];
        const auto expected = comparisonInputSummary(f.state, side, f.id);
        CHECK(input.summary.partCount == expected.partCount);
        CHECK(input.summary.enabledPartCount == expected.enabledPartCount);
        CHECK(input.summary.sourceNames == expected.sourceNames);
        CHECK(input.summary.issue == expected.issue);
        checkTree(input.roots, comparisonTree(f.state, side, f.id));
    }
}
}

TEST_CASE("analysis inspector reuses unchanged source queries across draws and navigation") {
    InspectorFixture f(2203);
    checkQueries(f);
    const auto builds = f.queries().builds;
    const auto* roots = f.queries().inputs[0].roots.data();
    for (int i = 0; i < 3; ++i) { f.frame(); }
    f.expandSources();
    f.frame();
    orbitUiCamera(f.state, 10, 5);
    selectSceneObject(f.state, f.state.files[0].objectId);
    selectSceneObject(f.state, f.id);
    setPropertiesPaneVisible(f.state, false);
    setPropertiesPaneVisible(f.state, true);
    f.frame();
    CHECK(f.queries().builds == builds);
    CHECK(f.queries().inputs[0].roots.data() == roots);
    CHECK(f.queries().inputs[0].summary.partCount == 2203);
}

TEST_CASE("analysis inspector invalidates membership transforms names geometry and missing sources") {
    InspectorFixture f;
    const auto initial = f.queries().builds;
    const auto signature = f.queries().signature;
    SUBCASE("disabled member") {
        setComparisonObjectsEnabled(f.state, {f.state.files[0].groupSettings[0].objectId}, ComparisonSide::a, false, f.id);
        CHECK(f.queries().inputs[0].summary.enabledPartCount == 1);
        CHECK(f.queries().signature != signature);
    }
    SUBCASE("all members disabled") {
        setComparisonObjectsEnabled(f.state, {}, ComparisonSide::a, false, f.id);
        CHECK(f.queries().signature == 0);
        CHECK(f.queries().inputs[0].enabledMemberCount == 0);
    }
    SUBCASE("two inputs with mixed membership") {
        setAnalysisTask(f.state, f.id, AnalysisTask::surfaceComparison);
        setComparisonObjects(f.state, {f.state.files[0].groupSettings[1].objectId}, ComparisonSide::b, true, f.id);
        CHECK(f.queries().inputs[1].summary.partCount == 1);
        CHECK(f.queries().signature != signature);
    }
    SUBCASE("group transform") {
        selectSceneObject(f.state, f.state.files[0].groupSettings[0].objectId);
        setSelectedObjectProperty(f.state, UiObjectProperty::translationY, 3);
        CHECK(f.queries().signature != signature);
    }
    SUBCASE("file transform") {
        selectSceneObject(f.state, f.state.files[0].objectId);
        setSelectedObjectProperty(f.state, UiObjectProperty::scale, 2);
        CHECK(f.queries().signature != signature);
    }
    SUBCASE("folder transform") {
        selectSceneObject(f.state, f.state.sceneNodes[0].objectId);
        setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 7);
        CHECK(f.queries().signature != signature);
    }
    SUBCASE("source name") {
        f.state.sceneNodes[0].children[0].name = "Renamed source";
        notifySceneEdit(f.state);
        CHECK(f.queries().inputs[0].summary.sourceNames == "Renamed source");
        CHECK(f.queries().signature == signature);
    }
    SUBCASE("equal sized replacement geometry") {
        auto replacement = f.state.files[0].mesh.vertices;
        replacement[0].position[0] = 3;
        f.state.files[0].mesh.vertices = std::move(replacement);
        renewMeshContentRevision(f.state.files[0].mesh);
        notifySceneEdit(f.state);
        CHECK(f.queries().signature != signature);
    }
    SUBCASE("missing source") {
        f.state.files.clear();
        f.state.sceneNodes.clear();
        notifySceneEdit(f.state);
        CHECK(f.queries().signature == 0);
        CHECK(f.queries().inputs[0].memberCount == 2);
        CHECK(f.queries().inputs[0].enabledMemberCount == 2);
        CHECK(f.queries().inputs[0].summary.partCount == 0);
        CHECK(f.queries().inputs[0].summary.issue.find("missing") != std::string::npos);
    }
    SUBCASE("empty input") {
        clearComparisonGroup(f.state, ComparisonSide::a, f.id);
        CHECK(f.queries().inputs[0].roots.empty());
    }
    checkQueries(f);
    CHECK(f.queries().builds == initial + 1);
}

TEST_CASE("analysis inspector invalidates history scene replacement and reused identities") {
    InspectorFixture f;
    SceneHistory history;
    resetSceneHistory(history, f.state);
    const auto clean = createSceneDocument(f.state);
    const auto first = f.queries().signature;
    setComparisonObjectsEnabled(f.state, {f.state.files[0].groupSettings[0].objectId}, ComparisonSide::a, false, f.id);
    recordSceneHistory(history, f.state);
    const auto second = f.queries().signature;
    CHECK(first != second);
    for (const bool redo : {false, true}) {
        const auto builds = f.queries().builds;
        auto restored = prepareSceneHistoryStep(history, f.state, clean, redo);
        REQUIRE(restored);
        commitSceneHistoryStep(history, f.state, std::move(*restored), redo);
        // History may restore geometry with a different backing identity.
        CHECK(f.queries().inputs[0].summary.enabledPartCount == (redo ? 1 : 2));
        CHECK(f.queries().builds == builds + 1);
        checkQueries(f);
    }
    const auto builds = f.queries().builds;
    auto replacement = prepareSceneReplacement(f.state, f.state.files, createSceneDocument(f.state));
    // Exercise a reused analysis ID and matching revision across document generations.
    replacement.comparisons[0].objectId = f.id;
    replacement.sceneEditRevision = f.state.sceneEditRevision;
    f.state = std::move(replacement);
    CHECK(f.queries().builds == builds + 1);
    checkQueries(f);
    auto otherState = f.state;
    auto& cache = f.runtimes.objects[f.id].inspector.queries;
    CHECK_FALSE(comparisonInspectorCacheCurrent(cache, otherState, f.id));
    const auto other = duplicateComparison(f.state, f.id);
    updateComparisonInspectorCache(cache, f.state, other);
    CHECK(cache.objectId == other);
    checkQueries(f);
}

TEST_CASE("analysis inspector keeps result publication and settings readiness live") {
    InspectorFixture f;
    auto& runtime = f.runtimes.objects[f.id];
    const auto signature = f.queries().signature;
    const auto builds = f.queries().builds;
    auto settings = comparisonSettings(f.state, f.id);
    const auto ready = [&](uint32_t stage, bool gpu = false) {
        return comparisonStagesReady(runtime, settings, signature, stage, gpu);
    };
    CHECK(ready(comparisonSource));
    CHECK_FALSE(ready(comparisonSource, true));
    runtime.gpu.uploadedStages = comparisonSource;
    CHECK(ready(comparisonSource, true));
    runtime.results.cache.completed &= ~comparisonSource;
    CHECK_FALSE(ready(comparisonSource));
    runtime.results.cache.completed |= comparisonSource;
    CHECK(ready(comparisonSource));
    ++runtime.results.signature;
    CHECK_FALSE(ready(comparisonSource));
    runtime.results.signature = signature;
    CHECK(ready(comparisonTopology));
    settings.topologyInspection.holeSizeRatioTolerance += 1;
    CHECK_FALSE(ready(comparisonTopology));
    settings = comparisonSettings(f.state, f.id);
    settings.degenerates.needleThresholdRatio += 1;
    CHECK_FALSE(ready(comparisonDegenerates));
    settings = comparisonSettings(f.state, f.id);
    settings.topologyMode = TopologyMode::exactPosition;
    runtime.results.cache.topologyMode = TopologyMode::originalIndex;
    CHECK_FALSE(ready(comparisonTopology));
    for (const auto phase : {IntersectionPhase::queued, IntersectionPhase::running,
            IntersectionPhase::failed, IntersectionPhase::canceled, IntersectionPhase::complete}) {
        runtime.results.value.detectors[0].phase = phase;
        f.frame();
        CHECK(f.queries().builds == builds);
    }
}

TEST_CASE("analysis inspector preserves final findings page and clears stale selection") {
    InspectorFixture f;
    auto& runtime = f.runtimes.objects[f.id];
    auto& surface = runtime.results.value.original;
    auto topologySources = std::make_shared<std::vector<SourceTopology>>(1);
    surface.topology.sources = *topologySources;
    surface.topology.sourceStorage = topologySources;
    auto& source = (*topologySources)[0];
    source.source = "source.obj";
    source.edges.resize(52);
    source.vertices.resize(1);
    for (size_t i = 0; i < 52; ++i) { surface.topology.boundaries.push_back({0, i}); }
    surface.topologyBoundaries.resize(52);
    selectComparisonDiagnostic(f.state, runtime.results.value, runtime.results.signature, 51, f.id);
    REQUIRE(findComparison(f.state, f.id)->diagnosticFocus);
    const auto contents = f.frame(true);
    CHECK(contents.find("Edges 51-52 of 52") != std::string::npos);
    CHECK(findComparison(f.state, f.id)->diagnosticFocus->index == 51);
    const auto builds = f.queries().builds;
    f.frame();
    CHECK(f.queries().builds == builds);
    setComparisonObjectsEnabled(f.state, {f.state.files[0].groupSettings[0].objectId}, ComparisonSide::a, false, f.id);
    f.frame();
    CHECK_FALSE(findComparison(f.state, f.id)->diagnosticFocus);
    CHECK(surface.topology.boundaries.size() == 52);
}

TEST_CASE("analysis inspector unchanged draw benchmark" * doctest::skip()) {
    for (const size_t groups : {1u, 256u, 2203u}) {
        InspectorFixture f(groups);
        f.frame();
        for (const bool expanded : {false, true}) {
            if (expanded) { f.expandSources(); f.frame(); }
            std::vector<double> times;
            for (int i = 0; i < 9; ++i) {
                const auto start = std::chrono::steady_clock::now();
                f.frame();
                times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(times.begin(), times.end());
            std::cout << (expanded ? "properties_expanded_sources," : "properties_collapsed_sources,") << groups << ',' << times[4] << " ms\n";
            CHECK(f.queries().builds == 1);
        }
    }
}

TEST_CASE("analysis submission resolves live modes from current cached inputs") {
    for (const auto enabled : {std::array{true, true}, std::array{true, false},
                              std::array{false, true}, std::array{false, false}}) {
        for (const auto mode : {ComparisonMode::original, ComparisonMode::repaired,
                ComparisonMode::overlay, ComparisonMode::distance, ComparisonMode::surfaceQuality}) {
            InspectorFixture f;
            auto& runtime = f.runtimes.objects[f.id];
            runtime.gpu.uploadedStages = runtime.results.cache.completed | comparisonDistance | comparisonQuality;
            runtime.results.cache.completed = runtime.gpu.uploadedStages;
            setAnalysisTask(f.state, f.id, AnalysisTask::surfaceComparison);
            setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::b, true, f.id);
            setComparisonObjectsEnabled(f.state, {}, ComparisonSide::a, enabled[0], f.id);
            setComparisonObjectsEnabled(f.state, {}, ComparisonSide::b, enabled[1], f.id);
            auto settings = comparisonSettings(f.state, f.id);
            settings.mode = mode;
            // Also exercise legacy two-input quality settings, before operation
            // normalization consolidates a mesh-quality analysis into source A.
            findComparison(f.state, f.id)->settings = settings;
            notifySceneEdit(f.state);
            runtime.results.signature = runtime.results.cache.signature = f.queries().signature;
            const auto cached = readyComparisonSettings(runtime, f.state, f.id);
            const auto queries = runtime.inspector.queries;
            runtime.inspector.queries = {};
            // Render/export callers before the runtime update must still resolve
            // current state rather than trusting an absent or stale snapshot.
            const auto uncached = readyComparisonSettings(runtime, f.state, f.id);
            runtime.inspector.queries = queries;
            CHECK(cached == uncached);
            if (enabled[0] != enabled[1]) {
                if (mode == ComparisonMode::surfaceQuality) {
                    CHECK(cached.quality.onOriginal == enabled[0]);
                } else {
                    CHECK(cached.mode == (enabled[0] ? ComparisonMode::original : ComparisonMode::repaired));
                }
            }
        }
    }
}

TEST_CASE("analysis submission preserves selection translation overlays and stale result rejection") {
    InspectorFixture f;
    auto& runtime = f.runtimes.objects[f.id];
    runtime.results.value.original.source = comparisonWorldMesh(f.state, ComparisonSide::a, f.id);
    runtime.results.value.original.diagnostics.boundaryEdges = {{{0, 0, 0}, {1, 0, 0}}};
    runtime.gpu.uploadedStages = runtime.results.cache.completed;
    f.queries();
    setPropertiesPaneVisible(f.state, false);
    setComparisonTranslation(f.state, f.id, {7, 8, 9});
    const auto parts = [&] {
        std::vector<ScenePickPart> result;
        appendVisibleComparisonPickParts(result, f.state, f.runtimes);
        return result;
    };
    auto visible = parts();
    REQUIRE(visible.size() >= 2);
    CHECK(visible.front().selected);
    CHECK(visible.front().model[12] == 7);
    CHECK(visible.front().model[13] == 8);
    CHECK(visible.front().model[14] == 9);
    const auto bounds = sceneSelectionLines(visible);
    CHECK(bounds.size() == 24);
    auto settings = comparisonSettings(f.state, f.id);
    settings.showBoundaries = false;
    setComparisonSettings(f.state, settings, f.id);
    CHECK(parts().size() == 1);
    CHECK(sceneSelectionLines(parts()) == bounds);
    ++runtime.results.stageRevisions[0];
    CHECK(parts().empty());
    runtime.gpu.stageRevisions = runtime.results.stageRevisions;
    CHECK(parts().size() == 1);
    setComparisonObjectsEnabled(f.state, {f.state.files[0].groupSettings[0].objectId}, ComparisonSide::a, false, f.id);
    CHECK_FALSE(comparisonInspectorCacheCurrent(runtime.inspector.queries, f.state, f.id));
    CHECK(parts().empty());
    f.queries();
    CHECK(parts().empty());
}

TEST_CASE("analysis submission keeps finding focus live without rebuilding cached inputs") {
    InspectorFixture f;
    auto& runtime = f.runtimes.objects[f.id];
    setAnalysisTask(f.state, f.id, AnalysisTask::surfaceComparison);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::b, true, f.id);
    auto settings = comparisonSettings(f.state, f.id);
    settings.mode = ComparisonMode::overlay;
    setComparisonSettings(f.state, settings, f.id);
    runtime.results.signature = runtime.results.cache.signature = f.queries().signature;
    runtime.results.value.original.diagnostics.boundaryEdges = {{{0, 0, 0}, {1, 0, 0}}};
    const auto builds = f.queries().builds;
    selectComparisonDiagnostic(f.state, runtime.results.value, runtime.results.signature, 0, f.id);
    REQUIRE(findComparison(f.state, f.id)->diagnosticFocus);
    CHECK(readyComparisonSettings(runtime, f.state, f.id).mode == ComparisonMode::original);
    setComparisonTranslation(f.state, f.id, {1, 2, 3});
    CHECK(readyComparisonSettings(runtime, f.state, f.id).mode == ComparisonMode::original);
    CHECK(f.queries().builds == builds);
    resetComparisonDiagnosticFocus(f.state, f.id);
    CHECK(readyComparisonSettings(runtime, f.state, f.id).mode == ComparisonMode::overlay);
    selectComparisonDiagnostic(f.state, runtime.results.value, runtime.results.signature, 0, f.id);
    REQUIRE(findComparison(f.state, f.id)->diagnosticFocus);
    selectSceneObject(f.state, f.state.files[0].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 5);
    CHECK_FALSE(comparisonInspectorCacheCurrent(runtime.inspector.queries, f.state, f.id));
    CHECK(readyComparisonSettings(runtime, f.state, f.id).mode == ComparisonMode::overlay);
    f.queries();
    CHECK(readyComparisonSettings(runtime, f.state, f.id).mode == ComparisonMode::overlay);
}

TEST_CASE("analysis submission validates UV focus against a resolved geometry signature") {
    InspectorFixture f;
    auto* comparison = findComparison(f.state, f.id);
    comparison->settings.type = AnalysisType::uvQuality;
    notifySceneEdit(f.state);
    const auto signature = f.queries().signature;
    REQUIRE(signature != 0);
    comparison->uvFindingFocus = UvFindingFocus{signature, 0, std::array<std::array<float, 3>, 3>{}};
    const auto resolved = [&] {
        return focusedUvFinding(f.state, signature, f.id,
            comparisonCurrentSignature(f.runtimes.objects[f.id], f.state, f.id));
    };
    CHECK(resolved() == focusedUvFinding(f.state, signature, f.id));
    CHECK(resolved() != nullptr);
    CHECK(focusedUvFinding(f.state, signature, f.id, 0) == nullptr);
    SUBCASE("hidden") {
        auto settings = comparison->settings;
        settings.enabled = false;
        setComparisonSettings(f.state, settings, f.id);
    }
    SUBCASE("missing finding geometry") { comparison->uvFindingFocus->geometry.reset(); }
    SUBCASE("changed source") {
        selectSceneObject(f.state, f.state.files[0].objectId);
        setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 5);
    }
    CHECK(resolved() == nullptr);
    f.queries();
    CHECK(resolved() == nullptr);
}

TEST_CASE("analysis submission CPU benchmark" * doctest::skip()) {
    graphics::Init init;
    init.type = graphics::RendererType::Noop;
    init.resolution.width = init.resolution.height = 1;
    REQUIRE(graphics::init(init));
    struct Shutdown { ~Shutdown() { graphics::shutdown(); } } shutdown;
    const uint32_t code = 0;
    const auto program = graphics::createProgram(
        graphics::createShader(graphics::copy(&code, sizeof(code))),
        graphics::createShader(graphics::copy(&code, sizeof(code))), true);
    const auto uniform = graphics::createUniform("u_color", graphics::UniformType::Vec4);
    const auto parameters = graphics::createUniform("u_comparison", graphics::UniformType::Vec4);
    for (const size_t groups : {1u, 256u, 2203u}) {
        InspectorFixture f(groups);
        auto& runtime = f.runtimes.objects[f.id];
        runtime.results.value.original.source = comparisonWorldMesh(f.state, ComparisonSide::a, f.id);
        runtime.results.value.original.diagnostics.boundaryEdges = {{{0, 0, 0}, {1, 0, 0}}};
        auto gpu = createGpuMesh(runtime.results.value.original.source, meshVertexLayout(), gpuMeshEdges);
        runtime.gpu.original.vertices = gpu.vertexBuffer;
        runtime.gpu.original.triangles = gpu.triangleIndexBuffer;
        runtime.gpu.original.lines = gpu.lineIndexBuffer;
        f.runtimes.program = program;
        f.runtimes.parameters = parameters;
        runtime.gpu.uploadedStages = runtime.results.cache.completed;
        f.queries();
        SceneRenderScratch scratch;
        for (const bool overlays : {true, false}) {
            auto settings = comparisonSettings(f.state, f.id);
            settings.showEdges = settings.showBoundaries = settings.showNonManifold = settings.showWinding = overlays;
            settings.topologyInspection.showNonManifoldVertices = settings.topologyInspection.showHoles = settings.topologyInspection.showFins = overlays;
            settings.duplicates.showPoints = settings.duplicates.showTriangles = settings.degenerates.show = overlays;
            setComparisonSettings(f.state, settings, f.id);
            f.queries();
            for (int scenario = 0; scenario < 4; ++scenario) {
                const bool properties = scenario % 2 == 0;
                const bool focused = scenario >= 2;
                if (focused) { selectComparisonDiagnostic(f.state, runtime.results.value, runtime.results.signature, 0, f.id); }
                else { resetComparisonDiagnosticFocus(f.state, f.id); }
                setPropertiesPaneVisible(f.state, properties);
                if (properties) { f.frame(); }
                std::array<std::vector<double>, 3> times;
                for (int i = 0; i < 25; ++i) {
                    auto start = std::chrono::steady_clock::now();
                    (void)effectiveComparisonSettings(f.state, f.id);
                    const auto scene = std::chrono::steady_clock::now();
                    submitComparisonScenes(0, f.state, f.runtimes, program, uniform, scratch);
                    const auto helpers = std::chrono::steady_clock::now();
                    scratch.parts.clear();
                    appendVisibleComparisonPickParts(scratch.parts, f.state, f.runtimes);
                    submitSceneSelection(0, scratch.parts, f.state, helperLineVertexLayout(), program, uniform, scratch);
                    const auto end = std::chrono::steady_clock::now();
                    if (i >= 5) {
                        times[0].push_back(std::chrono::duration<double, std::micro>(scene-start).count());
                        times[1].push_back(std::chrono::duration<double, std::micro>(helpers-scene).count());
                        times[2].push_back(std::chrono::duration<double, std::micro>(end-helpers).count());
                    }
                    graphics::frame();
                }
                std::cout << "submission_us," << groups << ",overlays=" << overlays << ",properties=" << properties << ",focus=" << focused;
                for (auto& samples : times) {
                    std::sort(samples.begin(), samples.end());
                    std::cout << ',' << samples[samples.size()/2];
                }
                std::cout << '\n';
            }
        }
        destroyGpuMesh(gpu);
    }
    graphics::destroy(parameters);
    graphics::destroy(uniform);
    graphics::destroy(program);
}
