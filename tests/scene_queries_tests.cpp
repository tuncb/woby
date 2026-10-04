#include "scene_queries.h"
#include "scene_inspector_queries.h"
#include "scene_history.h"
#include "annotation_work.h"
#include "annotation_command.h"
#include "hover_pick.h"
#include "comparison_runtime.h"
#include "automation_registry.h"
#include "allocation_probe.h"
#include <doctest/doctest.h>
#include <chrono>
#include <iostream>
#include <algorithm>

namespace {
using namespace woby;
struct QueryFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-queries-" + automationRandomHex(8));
    UiState state;
    SceneQueryRuntime queries;
    explicit QueryFixture(size_t count = 3) {
        std::filesystem::create_directory(root);
        Mesh mesh;
        mesh.vertices = {{{-.8f,-.8f,.5f}, {}, {0,0}}, {{.8f,-.8f,.5f}, {}, {1,0}}, {{0,.8f,.5f}, {}, {.5f,1}}};
        mesh.bounds = calculateBounds(mesh.vertices);
        for (size_t i = 0; i < count; ++i) {
            mesh.nodes.push_back({"part" + std::to_string(i), static_cast<uint32_t>(mesh.indices.size()), 3});
            mesh.nodes.back().hasTexcoords = true;
            mesh.indices.insert(mesh.indices.end(), {0,1,2});
        }
        state.files.push_back(createUiFileState(root / "model.obj", std::move(mesh), 0));
        appendFolderTreeSceneNode(state, root, 0, 1);
        selectSceneObject(state, state.files[0].objectId);
    }
    ~QueryFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    SceneObjectId part(size_t i = 0) const { return state.files[0].groupSettings[i].objectId; }
    SceneObjectId annotation() {
        ScenePickView view;
        bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
        view.width = view.height = 200;
        const auto projection = annotationProjection(scenePickParts(state), view, part());
        return createAnnotation(state, part(), projectAnnotation(projection, AnnotationShape::line, {-.1f,-.2f}, {.1f,-.2f}));
    }
    void checkParts(bool hidden = false) {
        const auto expected = scenePickParts(state, hidden);
        std::vector<ScenePickPart> actual;
        resolveSceneParts(queries, state, actual, hidden);
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i) {
            CAPTURE(i);
            CHECK(actual[i].objectId == expected[i].objectId);
            CHECK(actual[i].mesh == expected[i].mesh);
            CHECK(actual[i].indexOffset == expected[i].indexOffset);
            CHECK(actual[i].indexCount == expected[i].indexCount);
            CHECK(actual[i].lineIndexOffset == expected[i].lineIndexOffset);
            CHECK(actual[i].lineIndexCount == expected[i].lineIndexCount);
            CHECK(actual[i].pointIndexOffset == expected[i].pointIndexOffset);
            CHECK(actual[i].pointIndexCount == expected[i].pointIndexCount);
            CHECK(actual[i].model == expected[i].model);
            CHECK(actual[i].selected == expected[i].selected);
            CHECK(actual[i].opacity == expected[i].opacity);
            CHECK(actual[i].pointSize == expected[i].pointSize);
            CHECK(actual[i].solid == expected[i].solid);
            CHECK(actual[i].edges == expected[i].edges);
            CHECK(actual[i].vertices == expected[i].vertices);
            CHECK(actual[i].lineWidth == expected[i].lineWidth);
            CHECK(actual[i].edgeXray == expected[i].edgeXray);
        }
    }
    void checkSelection() {
        updateSceneSelectionQueries(queries, state);
        const auto bounds = selectedSceneBounds(state);
        REQUIRE(queries.selection.bounds.has_value() == bounds.has_value());
        if (bounds) {
            CHECK(queries.selection.bounds->min == bounds->min);
            CHECK(queries.selection.bounds->max == bounds->max);
            CHECK(queries.selection.bounds->radius == doctest::Approx(bounds->radius));
        }
        const auto dimensions = sceneDimensions(scenePickParts(state));
        REQUIRE(queries.selection.dimensions.has_value() == dimensions.has_value());
        if (dimensions) { CHECK(queries.selection.dimensions->lengths == dimensions->lengths); }
    }
};
}

TEST_CASE("scene query hits reuse work and allocate nothing") {
    QueryFixture f(4096);
    const auto analysis = createComparison(f.state);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, true, analysis);
    selectSceneObject(f.state, f.state.files[0].objectId);
    f.checkParts(); f.checkSelection();
    updateSceneTreeQueries(f.queries.tree, f.state);
    updateSceneBoundsQuery(f.queries, f.state);
    const auto builds = f.queries.parts.builds;
    const auto dimensions = f.queries.selection.dimensionBuilds;
    const auto counts = f.queries.tree.builds;
    const auto members = f.queries.tree.membershipBuilds;
    const auto work = [&] {
        for (int i = 0; i < 100; ++i) {
            updateScenePartQueries(f.queries.parts, f.state);
            updateSceneTreeQueries(f.queries.tree, f.state);
            updateSceneSelectionQueries(f.queries, f.state);
            updateSceneAnnotationQueries(f.queries, f.state);
            updateSceneBoundsQuery(f.queries, f.state);
        }
    };
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(test::countAllocations(work) == 0);
#else
    work();
#endif
    CHECK(f.queries.parts.builds == builds);
    CHECK(f.queries.selection.dimensionBuilds == dimensions);
    CHECK(f.queries.tree.builds == counts);
    CHECK(f.queries.tree.membershipBuilds == members);
    const auto view = createView(f.state);
    renameView(f.state, view, "Saved view");
    renameComparison(f.state, analysis, "Analysis label");
    setSelectedObjectProperty(f.state, UiObjectProperty::red, .2f);
    setShowGrid(f.state, false);
    orbitUiCamera(f.state, 20, 10);
    work();
    CHECK(f.queries.parts.builds == builds);
    CHECK(f.queries.selection.dimensionBuilds == dimensions);
    CHECK(f.queries.tree.builds == counts);
    CHECK(f.queries.tree.membershipBuilds == members);
    CHECK(f.state.isDirty);
}

TEST_CASE("scene tree summaries match recursive counts and active membership") {
    QueryFixture f;
    const auto first = createComparison(f.state);
    setComparisonObjects(f.state, {f.part()}, ComparisonSide::a, true, first);
    setComparisonObjectsEnabled(f.state, {f.part()}, ComparisonSide::a, false, first);
    const auto second = createComparison(f.state);
    setComparisonObjects(f.state, {f.part(1)}, ComparisonSide::b, true, second);
    const auto check = [&] {
        updateSceneTreeQueries(f.queries.tree, f.state);
        const auto visit = [&](auto&& self, const UiSceneNode& node) -> void {
            const auto summary = sceneTreeSummary(f.queries.tree, node.objectId);
            CHECK(summary.parts == countSceneNodeGroups(f.state, node));
            CHECK(summary.visible == countVisibleSceneNodeGroups(f.state, node));
            for (const auto& child : node.children) { self(self, child); }
        };
        for (const auto& node : f.state.sceneNodes) { visit(visit, node); }
        for (size_t i = 0; i < 3; ++i) {
            for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
                CHECK(sceneTreeMember(f.queries.tree, f.part(i), side) == comparisonContains(f.state, f.part(i), side));
            }
        }
    };
    check();
    selectSceneObject(f.state, first); check();
    CHECK(sceneTreeMember(f.queries.tree, f.part(), ComparisonSide::a)); // Disabled members retain their badge.
    selectSceneObject(f.state, f.part()); check(); // Keep the last active comparison.
    setSceneNodeSubtreeVisible(f.state, f.state.sceneNodes[0], false); check();
    setSceneNodeSubtreeVisible(f.state, f.state.sceneNodes[0], true); check();
    removeComparison(f.state, first); check();
    removeFileFromState(f.state, 0);
    updateSceneTreeQueries(f.queries.tree, f.state);
    CHECK(f.queries.tree.nodes.empty());
}

TEST_CASE("scene parts preserve hidden sources and selection without recomposing transforms") {
    QueryFixture f;
    f.checkParts();
    const auto builds = f.queries.parts.builds;
    selectSceneObject(f.state, f.part(1)); f.checkParts();
    selectSceneObject(f.state, f.state.sceneNodes[0].objectId, true); f.checkParts();
    CHECK(f.queries.parts.builds == builds);
    f.state.files.reserve(f.state.files.capacity() + 10);
    f.checkParts();
    CHECK(f.queries.parts.builds == builds);
    for (const auto& record : f.queries.parts.records) {
        CHECK(record.value.mesh == nullptr);
        CHECK(record.value.pointCloud == nullptr);
        CHECK(record.value.diagnosticEdges.empty());
    }
    clearSceneSelection(f.state); selectSceneObject(f.state, f.part());
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 4); f.checkParts();
    setSelectedObjectProperty(f.state, UiObjectProperty::rotationY, 37); f.checkParts();
    setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 0); f.checkParts(); f.checkParts(true);
    setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 1);
    setSelectedObjectsVisible(f.state, false); f.checkParts(); f.checkParts(true);
    setSelectedObjectsVisible(f.state, true);
    setSelectedObjectProperty(f.state, UiObjectProperty::solidMesh, 0); f.checkParts(); f.checkParts(true);
    setSelectedObjectProperty(f.state, UiObjectProperty::vertices, 1);
    f.checkParts();
    const auto transforms = f.queries.parts.builds;
    setMasterVertexPointSize(f.state, 13);
    setSelectedObjectProperty(f.state, UiObjectProperty::vertexSize, 2);
    setTriangleEdgeXray(f.state, !f.state.triangleEdgeXray);
    f.checkParts(); CHECK(f.queries.parts.builds == transforms);
}

TEST_CASE("cached parts retain imported lines points and inherited transforms") {
    QueryFixture f;
    auto& mesh = f.state.files[0].mesh;
    mesh.lineIndices = {0, 1}; mesh.pointIndices = {2};
    mesh.nodes[1].indexCount = 0; mesh.nodes[1].lineIndexCount = 2;
    mesh.nodes[2].indexCount = 0; mesh.nodes[2].pointIndexCount = 1;
    renewMeshContentRevision(mesh); notifySceneEdit(f.state, SceneChange::geometry);
    selectSceneObject(f.state, f.part(2));
    setSelectedObjectProperty(f.state, UiObjectProperty::vertices, 1);
    selectSceneObject(f.state, f.state.sceneNodes[0].objectId);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationY, 7);
    setSelectedObjectProperty(f.state, UiObjectProperty::rotationZ, 36);
    f.checkParts(); f.checkSelection();
    const auto builds = f.queries.parts.builds;
    selectSceneObject(f.state, f.part(1));
    setSelectedObjectProperty(f.state, UiObjectProperty::lineWidth, 8);
    setSelectedObjectProperty(f.state, UiObjectProperty::lineDepthTest, 0);
    f.checkParts(); f.checkSelection();
    CHECK(f.queries.parts.builds == builds);
    setSelectedObjectsVisible(f.state, false);
    f.checkParts(); f.checkParts(true); f.checkSelection();
}

TEST_CASE("shared selection queries distinguish framing from exact measurements") {
    QueryFixture f;
    f.checkSelection();
    SceneInspectorRuntime inspector;
    inspector.sharedQueries = &f.queries;
    updateSceneInspector(inspector, f.state);
    CHECK(f.queries.selection.dimensionBuilds == 1);
    CHECK(inspector.snapshot.dimensions->lengths == f.queries.selection.dimensions->lengths);
    setSelectedObjectProperty(f.state, UiObjectProperty::rotationZ, 29); f.checkSelection();
    setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 0); f.checkSelection();
    REQUIRE(f.queries.selection.bounds);
    CHECK_FALSE(f.queries.selection.dimensions);
    setSelectedObjectsVisible(f.state, false); f.checkSelection();
    CHECK_FALSE(f.queries.selection.bounds);
    clearSceneSelection(f.state); f.checkSelection();
}

TEST_CASE("scene query ownership covers replacement documents equal size meshes and history") {
    QueryFixture f;
    f.checkSelection();
    const auto builds = f.queries.selection.dimensionBuilds;
    UiState copy = f.state;
    updateSceneSelectionQueries(f.queries, copy);
    CHECK(f.queries.selection.dimensionBuilds == builds + 1);
    f.checkSelection();
    SceneHistory history;
    const auto clean = createSceneDocument(f.state);
    resetSceneHistory(history, f.state);
    setSelectedObjectProperty(f.state, UiObjectProperty::scale, 2);
    recordSceneHistory(history, f.state); f.checkSelection();
    for (const bool redo : {false, true}) {
        auto step = prepareSceneHistoryStep(history, f.state, clean, redo);
        REQUIRE(step);
        commitSceneHistoryStep(history, f.state, std::move(*step), redo);
        f.checkSelection();
    }
    auto& mesh = f.state.files[0].mesh;
    const auto* storage = mesh.vertices.data();
    mesh.vertices[1].position[0] += 3;
    renewMeshContentRevision(mesh);
    notifySceneEdit(f.state, SceneChange::geometry);
    f.checkSelection();
    CHECK(mesh.vertices.data() == storage);
    const auto oldId = f.part();
    const auto document = createSceneDocument(f.state);
    auto files = f.state.files;
    f.state = prepareSceneReplacement(f.state, std::move(files), document);
    selectSceneObject(f.state, f.state.files[0].objectId); f.checkParts(); f.checkSelection();
    // Even reused IDs and reset domain counters must not alias the prior document.
    f.state.files[0].groupSettings[0].objectId = oldId;
    f.state.sceneNodes[0].children[0].children[0].objectId = oldId;
    ++f.state.sceneGeneration; f.state.revisions = {};
    selectSceneObject(f.state, oldId); f.checkSelection();
    f.state = prepareSceneReplacement(f.state, {}, {});
    f.checkSelection(); f.checkParts();
    CHECK_FALSE(f.queries.selection.bounds);
}

TEST_CASE("UV signatures and bounds cache hits ignore result publication and label edits") {
    QueryFixture f;
    const auto id = createComparison(f.state, AnalysisType::uvQuality);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, true, id);
    auto& query = updateSceneComparisonQuery(f.queries, f.state, id);
    const auto builds = query.inputs.signatureBuilds;
    const auto signature = query.inputs.signature;
    renameComparison(f.state, id, "UV label");
    updateSceneComparisonQuery(f.queries, f.state, id);
    CHECK(query.inputs.signatureBuilds == builds);
    CHECK(query.inputs.signature == signature);
    CHECK(setUvProbe(f.state, id, f.part(), 1, {1.0/3, 1.0/3, 1.0/3}));
    auto parts = scenePickParts(f.state);
    std::vector<std::array<float, 3>> lines;
    uvProbeLines(parts, f.state, lines, &f.queries);
    REQUIRE_FALSE(lines.empty());
    const auto work = [&] { for (int i = 0; i < 100; ++i) { uvProbeLines(parts, f.state, lines, &f.queries); } };
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(test::countAllocations(work) == 0);
#else
    work();
#endif
    CHECK(query.inputs.signatureBuilds == builds);
    updateSceneSelectionQueries(f.queries, f.state);
    const auto bounds = query.boundsBuilds;
    auto settings = comparisonSettings(f.state, id);
    settings.uvMetric = UvQualityMetric::area;
    setComparisonSettings(f.state, settings, id);
    updateSceneSelectionQueries(f.queries, f.state);
    CHECK(query.boundsBuilds == bounds);
    CHECK(query.inputs.signature != signature);
    setComparisonTranslation(f.state, id, {10,20,30});
    updateSceneSelectionQueries(f.queries, f.state);
    CHECK(query.boundsBuilds == bounds + 1);
    CHECK(query.bounds->min == comparisonDisplayBounds(f.state, id)->min);
}

TEST_CASE("dimensions refresh on live analysis result publication and GPU readiness") {
    QueryFixture f(1);
    const auto id = createComparison(f.state, AnalysisType::uv);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, true, id);
    auto settings = comparisonSettings(f.state, id);
    settings.uvLinkedSelection = false;
    setComparisonSettings(f.state, settings, id);
    ComparisonRuntimes comparisons;
    auto& runtime = comparisons.objects[id];
    updateComparisonInspectorCache(runtime.inspector.queries, f.state, id);
    runtime.results.signature = runtime.results.cache.signature = runtime.inspector.queries.signature;
    runtime.results.value.original.source = comparisonWorldMesh(f.state, ComparisonSide::a, id);
    runtime.results.cache.completed = comparisonSource;
    runtime.results.stageRevisions[0] = 2;
    updateSceneSelectionQueries(f.queries, f.state, &comparisons);
    CHECK_FALSE(f.queries.selection.dimensions);
    runtime.gpu.uploadedStages = comparisonSource;
    runtime.gpu.stageRevisions[0] = 2;
    updateSceneSelectionQueries(f.queries, f.state, &comparisons);
    REQUIRE(f.queries.selection.dimensions);
    const auto dimensions = f.queries.selection.dimensionBuilds;
    updateSceneSelectionQueries(f.queries, f.state, &comparisons);
    CHECK(f.queries.selection.dimensionBuilds == dimensions);
    runtime.results.value.original.source.vertices[1].position[0] += 3;
    renewMeshContentRevision(runtime.results.value.original.source);
    ++runtime.results.revision;
    updateSceneSelectionQueries(f.queries, f.state, &comparisons);
    CHECK(f.queries.selection.dimensionBuilds == dimensions + 1);
    runtime.gpu.stageRevisions[0] = 1;
    updateSceneSelectionQueries(f.queries, f.state, &comparisons);
    CHECK_FALSE(f.queries.selection.dimensions);
}

TEST_CASE("annotation world geometry is shared and unrelated edits keep workers current") {
    QueryFixture f(1);
    const auto id = f.annotation();
    updateSceneAnnotationQueries(f.queries, f.state);
    const auto builds = f.queries.annotations.builds;
    REQUIRE_FALSE(f.queries.annotations.objects.at(id).lines.empty());
    auto identity = annotationWorkIdentity(f.state);
    renameAnnotation(f.state, id, "Outline");
    const auto view = createView(f.state); renameView(f.state, view, "View label");
    auto settings = findAnnotation(f.state, id)->settings;
    settings.color[0] = .4f; settings.width = 5;
    setAnnotationSettings(f.state, id, settings);
    updateSceneAnnotationQueries(f.queries, f.state);
    CHECK(f.queries.annotations.builds == builds);
    CHECK(annotationWorkCurrent(identity, f.state, true));
    f.checkSelection(); // Annotation framing uses the cached world outline.
    CHECK(f.queries.annotations.builds == builds);
    std::vector<ScenePickPart> parts;
    resolveSceneParts(f.queries, f.state, parts);
    appendSceneAnnotationParts(f.queries, f.state, parts);
    CHECK(parts.back().diagnosticEdges.data() == f.queries.annotations.objects.at(id).lines.data());
    SUBCASE("camera") { orbitUiCamera(f.state, 20, 0); }
    SUBCASE("projection depth bounds") { f.state.sceneBounds.radius += 10; }
    SUBCASE("selection") { selectSceneObject(f.state, f.part()); }
    SUBCASE("target transform") { selectSceneObject(f.state, f.part()); setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 3); }
    SUBCASE("visibility") { setAllAnnotationsVisible(f.state, false); }
    SUBCASE("lock") { settings.locked = true; setAnnotationSettings(f.state, id, settings); }
    SUBCASE("in-place same-sized geometry") { f.state.files[0].mesh.vertices[0].position[0] += 1; renewMeshContentRevision(f.state.files[0].mesh); }
    SUBCASE("document") { ++f.state.sceneGeneration; }
    CHECK_FALSE(annotationWorkCurrent(identity, f.state, true));
}

TEST_CASE("annotation edits rebuild only the affected world outline") {
    QueryFixture f(1);
    const auto id = f.annotation();
    const auto duplicate = duplicateAnnotation(f.state, id);
    updateSceneAnnotationQueries(f.queries, f.state);
    const auto original = f.queries.annotations.objects.at(id).lines;
    const auto duplicateBuilds = f.queries.annotations.objects.at(duplicate).builds;
    ScenePickView view;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    view.width = view.height = 200;
    auto geometry = projectAnnotation(annotationProjection(scenePickParts(f.state), view, f.part()),
        AnnotationShape::line, {-.2f, 0}, {.2f, 0});
    reshapeAnnotation(f.state, id, std::move(geometry));
    updateSceneAnnotationQueries(f.queries, f.state);
    const auto& lines = f.queries.annotations.objects.at(id).lines;
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front().a != original.front().a);
    CHECK(f.queries.annotations.objects.at(duplicate).builds == duplicateBuilds);
    CHECK(lines.front().a == annotationWorldLines(*findAnnotation(f.state, id), scenePickParts(f.state)).front().a);
    auto settings = findAnnotation(f.state, id)->settings;
    settings.color[3] = 0;
    setAnnotationSettings(f.state, id, settings);
    updateSceneAnnotationQueries(f.queries, f.state);
    CHECK(f.queries.annotations.objects.at(id).lines.empty());
    CHECK(f.queries.annotations.objects.at(duplicate).builds == duplicateBuilds);
    deleteAnnotation(f.state, id);
    updateSceneAnnotationQueries(f.queries, f.state);
    CHECK_FALSE(f.queries.annotations.objects.contains(id));
    selectSceneObject(f.state, f.part());
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, 3);
    updateSceneAnnotationQueries(f.queries, f.state);
    CHECK(f.queries.annotations.objects.at(duplicate).builds == duplicateBuilds + 1);
    CHECK(f.queries.annotations.objects.at(duplicate).lines.front().a[0] == doctest::Approx(original.front().a[0] + 3));
}

TEST_CASE("hover scene keys ignore labels and colors but track display mesh and GPU dependencies") {
    QueryFixture f(256);
    std::vector<LoadedModelRuntime> runtimes(1);
    HoverPickCache cache;
    const auto signature = hoverPickSignature(cache, f.state, runtimes, {20,30}, true, 800, 600, false);
    const auto view = createView(f.state); renameView(f.state, view, "Hover view");
    setSelectedObjectProperty(f.state, UiObjectProperty::red, .2f);
    CHECK(hoverPickSignature(cache, f.state, runtimes, {20,30}, true, 800, 600, false) == signature);
    CHECK(cache.sceneBuilds == 1);
    CHECK(hoverPickSignature(cache, f.state, runtimes, {21,30}, true, 800, 600, false) != signature);
    CHECK(cache.sceneBuilds == 1);
    SUBCASE("point size") { setMasterVertexPointSize(f.state, 19); }
    SUBCASE("opacity") { setSelectedObjectProperty(f.state, UiObjectProperty::opacity, 0); }
    SUBCASE("transform") { setSelectedObjectProperty(f.state, UiObjectProperty::translationY, 8); }
    SUBCASE("mesh content") { renewMeshContentRevision(f.state.files[0].mesh); }
    SUBCASE("GPU publication") { ++runtimes[0].gpuMesh.resourceRevision; }
    SUBCASE("GPU readiness") { runtimes[0].gpuMesh.pointIdBuffer.idx = 42; }
    CHECK(hoverPickSignature(cache, f.state, runtimes, {20,30}, true, 800, 600, false) != signature);
    CHECK(cache.sceneBuilds == 2);
}

TEST_CASE("annotation projection caches camera inputs separately from world geometry") {
    QueryFixture f(1);
    const auto id = f.annotation();
    updateSceneAnnotationQueries(f.queries, f.state);
    auto& query = f.queries.annotations.objects.at(id);
    auto parts = scenePickParts(f.state);
    ScenePickView view;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.renderProjection.data());
    view.width = view.height = 200;
    const auto update = [&] { updateAnnotationProjection(query, *findAnnotation(f.state, id), parts, view); };
    update();
    REQUIRE_FALSE(query.projected.empty());
    const auto vertices = query.projected;
    const auto builds = query.builds;
    const auto hits = [&] { for (int i = 0; i < 100; ++i) { update(); } };
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(test::countAllocations(hits) == 0);
#else
    hits();
#endif
    CHECK(query.projectionBuilds == 1);
    auto settings = findAnnotation(f.state, id)->settings;
    settings.color[0] = .7f;
    setAnnotationSettings(f.state, id, settings); update();
    CHECK(query.projectionBuilds == 1);
    view.view[12] = .2f; update();
    CHECK(query.projectionBuilds == 2);
    CHECK(query.projected != vertices);
    view.width = 400; update();
    CHECK(query.projectionBuilds == 3);
    settings.width = 10;
    setAnnotationSettings(f.state, id, settings); update();
    CHECK(query.projectionBuilds == 4);
    CHECK(query.builds == builds);
#if defined(_MSC_VER) && defined(_DEBUG)
    CHECK(test::countAllocations([&] {
        for (int i = 0; i < 100; ++i) { view.view[12] += .0001f; update(); }
    }) == 0);
#endif
    setAllAnnotationsVisible(f.state, false);
    updateSceneAnnotationQueries(f.queries, f.state); update();
    CHECK(query.projected.empty());
    CHECK(query.builds == builds + 1);
}

TEST_CASE("cached hover parts preserve CPU picking coordinates") {
    QueryFixture f(1);
    setSelectedObjectProperty(f.state, UiObjectProperty::vertices, 1);
    setSelectedObjectProperty(f.state, UiObjectProperty::translationX, .1f);
    SUBCASE("default opacity") {}
    SUBCASE("small inherited and group opacity") {
        setSelectedObjectProperty(f.state, UiObjectProperty::opacity, .0001f);
        selectSceneObject(f.state, f.part());
        setSelectedObjectProperty(f.state, UiObjectProperty::opacity, .0001f);
    }
    SUBCASE("invisible inherited opacity") { setSelectedObjectProperty(f.state, UiObjectProperty::opacity, .0000001f); }
    std::vector<LoadedModelRuntime> runtimes(1);
    runtimes[0].gpuMesh.nodeRanges.resize(1);
    runtimes[0].gpuMesh.nodeRanges[0].pointIndexCount = 3;
    runtimes[0].gpuMesh.pointVertexIndices = {0,1,2};
    ScenePickView view;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    view.width = view.height = 200;
    std::vector<ScenePickPart> parts;
    resolveSceneParts(f.queries, f.state, parts);
    for (const auto mouse : {MousePosition{30,180}, MousePosition{190,180}, MousePosition{110,20}, MousePosition{60,60}}) {
        const auto before = findHoveredVertex(f.state.files, f.state.sceneNodes, runtimes, mouse, f.state.masterVertexPointSize,
            view.view.data(), view.projection.data(), view.width, view.height, view.homogeneousDepth);
        const auto after = findHoveredVertex(f.state, parts, runtimes, mouse, view);
        REQUIRE(before.has_value() == after.has_value());
        if (before) {
            CHECK(after->localPosition == before->localPosition);
            CHECK(after->transformedPosition == before->transformedPosition);
        }
    }
}

TEST_CASE("scene query cache benchmark" * doctest::skip()) {
    using Clock = std::chrono::steady_clock;
    std::cout << "query,parts,legacy_us,cached_us,legacy_allocations,cached_allocations,retained_payload_bytes,builds\n";
    const auto measure = [](auto work) {
        const auto begin = Clock::now(); work();
        const auto estimate = std::chrono::duration<double, std::micro>(Clock::now() - begin).count();
        const auto iterations = static_cast<size_t>(std::clamp(5000.0 / std::max(estimate, .01), 1.0, 1000.0));
        std::vector<double> times;
        for (int i = 0; i < 7; ++i) {
            const auto start = Clock::now();
            for (size_t sample = 0; sample < iterations; ++sample) { work(); }
            times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count() / static_cast<double>(iterations));
        }
        std::sort(times.begin(), times.end()); return times[3];
    };
    const auto report = [&](const char* name, size_t count, auto before, auto after, size_t bytes, uint64_t builds) {
        const auto oldTime = measure(before), newTime = measure(after);
        size_t oldAllocations = 0, newAllocations = 0;
#if defined(_MSC_VER) && defined(_DEBUG)
        oldAllocations = test::countAllocations(before); newAllocations = test::countAllocations(after);
#endif
        std::cout << name << ',' << count << ',' << oldTime << ',' << newTime << ',' << oldAllocations << ','
            << newAllocations << ',' << bytes << ',' << builds << '\n';
    };
    for (const size_t count : {256u, 4096u, 16384u}) {
        QueryFixture f(count);
        const auto id = createComparison(f.state, AnalysisType::uvQuality);
        setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, true, id);
        selectSceneObject(f.state, f.state.files[0].objectId);
        updateSceneTreeQueries(f.queries.tree, f.state);
        f.checkSelection();
        size_t checksum = 0;
        report("membership", count, [&] {
            checksum = 0;
            for (const auto& part : f.state.files[0].groupSettings) { checksum += comparisonContains(f.state, part.objectId, ComparisonSide::a); }
        }, [&] {
            updateSceneTreeQueries(f.queries.tree, f.state); checksum = 0;
            for (const auto& part : f.state.files[0].groupSettings) { checksum += sceneTreeMember(f.queries.tree, part.objectId, ComparisonSide::a); }
        }, f.queries.tree.members[0].bucket_count() * sizeof(SceneObjectId), f.queries.tree.membershipBuilds);
        CHECK(checksum == count);
        report("tree_counts", count, [&] {
            checksum = countVisibleSceneNodeGroups(f.state, f.state.sceneNodes[0]) + countSceneNodeGroups(f.state, f.state.sceneNodes[0]);
        }, [&] {
            updateSceneTreeQueries(f.queries.tree, f.state);
            const auto summary = sceneTreeSummary(f.queries.tree, f.state.sceneNodes[0].objectId);
            checksum = summary.visible + summary.parts;
        }, f.queries.tree.nodes.bucket_count() * sizeof(decltype(f.queries.tree.nodes)::value_type), f.queries.tree.builds);
        CHECK(checksum == count * 2);
        size_t metadata = f.queries.parts.records.capacity() * sizeof(ScenePartRecord);
        for (const auto& part : f.queries.parts.records) { metadata += part.ancestors.capacity() * sizeof(SceneObjectId); }
        std::vector<ScenePickPart> scratch;
        resolveSceneParts(f.queries, f.state, scratch);
        report("parts", count, [&] { scenePickParts(f.state, scratch); }, [&] { resolveSceneParts(f.queries, f.state, scratch); }, metadata, f.queries.parts.builds);
        report("selection", count, [&] {
            const auto bounds = selectedSceneBounds(f.state);
            scenePickParts(f.state, scratch); const auto dimensions = sceneDimensions(scratch);
            if (!bounds || !dimensions) { throw std::runtime_error("Missing benchmark selection."); }
        }, [&] { updateSceneSelectionQueries(f.queries, f.state); }, sizeof(SceneSelectionQueries), f.queries.selection.dimensionBuilds);
        std::vector<LoadedModelRuntime> gpu(1);
        gpu[0].gpuMesh.nodeRanges.resize(count);
        gpu[0].gpuMesh.pointVertexIndices = {0,1,2};
        HoverPickCache hover;
        (void)hoverPickSignature(hover, f.state, gpu, {20,30}, true, 800, 600, false);
        report("hover_signature", count, [&] {
            checksum = hoverPickSignature(f.state.files, f.state.sceneNodes, gpu, {20,30}, true,
                f.state.masterVertexPointSize, f.state.camera, f.state.upAxis, f.state.sceneBounds, 800, 600, false);
        }, [&] { checksum = hoverPickSignature(hover, f.state, gpu, {20,30}, true, 800, 600, false); }, sizeof(hover), hover.sceneBuilds);
        auto& query = updateSceneComparisonQuery(f.queries, f.state, id);
        const auto treeBytes = [&](auto&& self, const std::vector<ComparisonTreeNode>& nodes) -> size_t {
            size_t bytes = nodes.capacity() * sizeof(ComparisonTreeNode);
            for (const auto& node : nodes) { bytes += node.name.capacity() + self(self, node.children); }
            return bytes;
        };
        size_t queryBytes = sizeof(query) + query.inputs.sources.capacity();
        for (const auto& input : query.inputs.inputs) {
            queryBytes += treeBytes(treeBytes, input.roots) + input.summary.sourceNames.capacity() + input.summary.issue.capacity();
        }
        report("uv_signature", count, [&] { if (!comparisonGeometrySignature(f.state, id)) { throw std::runtime_error("Missing signature."); } },
            [&] { updateSceneComparisonQuery(f.queries, f.state, id); }, queryBytes, query.inputs.signatureBuilds);
    }
    QueryFixture f(1);
    const auto id = f.annotation();
    auto& item = f.state.annotations[0];
    item.geometry.segments.resize(20000, item.geometry.segments.front());
    ++item.geometryRevision; notifySceneEdit(f.state, SceneChange::annotations);
    updateSceneAnnotationQueries(f.queries, f.state);
    const auto parts = scenePickParts(f.state);
    auto& query = f.queries.annotations.objects.at(id);
    std::vector<DiagnosticEdge> worldScratch;
    std::vector<const ScenePickPart*> sourceScratch;
    annotationWorldLines(item, parts, worldScratch, sourceScratch);
    report("annotation_20000_segments", 1, [&] { annotationWorldLines(item, parts, worldScratch, sourceScratch); },
        [&] { updateSceneAnnotationQueries(f.queries, f.state); }, query.lines.capacity() * sizeof(DiagnosticEdge), query.builds);
    ScenePickView view;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.renderProjection.data());
    view.width = view.height = 1000;
    updateAnnotationProjection(query, item, parts, view);
    report("annotation_projection", 1, [&] {
        query.projectedBuild = 0; updateAnnotationProjection(query, item, parts, view);
    }, [&] { updateAnnotationProjection(query, item, parts, view); }, query.projected.capacity() * sizeof(query.projected.front())
        + query.sources.capacity() * sizeof(AnnotationProjectionSource), query.projectionBuilds);
}
