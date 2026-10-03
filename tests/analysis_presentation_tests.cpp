#include "analysis_presentation.h"
#include "ui_operations.h"
#include "obj_mesh.h"
#include "scene_history.h"
#include "comparison_scene.h"

#include <doctest/doctest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

namespace {
using namespace woby;
struct AnalysisFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-analysis-ui-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
    UiState state;
    AnalysisFixture() {
        std::filesystem::create_directory(root);
        for (const auto* name : {"first.obj", "second.obj", "third.obj"}) {
            const auto path = root / name;
            { std::ofstream stream(path); stream << "v 0 0 0\nv 1 0 0\nv 0 1 0\ng surface\nf 1 2 3\n"; }
            state.files.push_back(createUiFileState(path, loadObjMesh(path), 0));
        }
        appendDefaultSceneNodesForFiles(state, 0);
    }
    ~AnalysisFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};
}

TEST_CASE("analysis tasks infer legacy intent without changing settings")
{
    ComparisonSettings settings;
    CHECK(analysisTask(settings, false) == AnalysisTask::meshChecks);
    CHECK(analysisTask(settings, true) == AnalysisTask::surfaceComparison);
    settings.mode = ComparisonMode::original;
    CHECK(analysisTask(settings, true) == AnalysisTask::surfaceComparison);
    settings.mode = ComparisonMode::surfaceQuality;
    CHECK(analysisTask(settings, false) == AnalysisTask::meshQuality);
    settings.type = AnalysisType::uvQuality;
    CHECK(analysisTask(settings, false) == AnalysisTask::uvInspection);
    CHECK(settings.task == AnalysisTask::automatic);
}

TEST_CASE("analysis creation assigns inputs according to the requested task")
{
    AnalysisFixture f;
    const auto a = f.state.files[0].objectId, b = f.state.files[1].objectId;
    selectSceneObject(f.state, a); selectSceneObject(f.state, b, true);
    const auto comparison = createAnalysisFromSelection(f.state, AnalysisTask::surfaceComparison);
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, comparison) == 1);
    CHECK(comparisonPartCount(f.state, ComparisonSide::b, comparison) == 1);
    CHECK(findComparison(f.state, comparison)->settings.task == AnalysisTask::surfaceComparison);
    selectSceneObject(f.state, a); selectSceneObject(f.state, b, true);
    const auto checks = createAnalysisFromSelection(f.state, AnalysisTask::meshChecks);
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, checks) == 2);
    CHECK(comparisonPartCount(f.state, ComparisonSide::b, checks) == 0);
    CHECK(findComparison(f.state, checks)->settings.mode == ComparisonMode::original);
    selectSceneObject(f.state, a); selectSceneObject(f.state, b, true);
    selectSceneObject(f.state, f.state.files[2].objectId, true);
    const auto count = f.state.comparisons.size();
    CHECK(createAnalysisFromSelection(f.state, AnalysisTask::surfaceComparison) == invalidSceneObjectId);
    CHECK(f.state.comparisons.size() == count);
    const auto quality = createAnalysisFromSelection(f.state, AnalysisTask::meshQuality);
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, quality) == 3);
    CHECK(findComparison(f.state, quality)->settings.mode == ComparisonMode::surfaceQuality);
}

TEST_CASE("analysis task creation names are unique and empty tasks remain repairable")
{
    UiState state;
    const auto first = createAnalysisFromSelection(state, AnalysisTask::surfaceComparison);
    const auto second = createAnalysisFromSelection(state, AnalysisTask::surfaceComparison);
    CHECK(findComparison(state, first)->name != findComparison(state, second)->name);
    CHECK(analysisTask(findComparison(state, first)->settings, false) == AnalysisTask::surfaceComparison);
    CHECK_FALSE(canInspectComparison(state, first));
    const auto uv = createAnalysisFromSelection(state, AnalysisTask::uvInspection);
    CHECK(findComparison(state, uv)->settings.type == AnalysisType::uv);
    setAnalysisTask(state, uv, AnalysisTask::meshChecks);
    CHECK(findComparison(state, uv)->settings.type == AnalysisType::uv);
}

TEST_CASE("analysis task and display survive scene and saved view round trips")
{
    AnalysisFixture f;
    selectSceneObject(f.state, f.state.files[0].objectId);
    const auto id = createAnalysisFromSelection(f.state, AnalysisTask::surfaceComparison);
    auto settings = comparisonSettings(f.state, id);
    settings.mode = ComparisonMode::original;
    setComparisonSettings(f.state, settings, id);
    CHECK(analysisTask(comparisonSettings(f.state, id), false) == AnalysisTask::surfaceComparison);
    createView(f.state);
    const auto document = createSceneDocument(f.state);
    const auto path = f.root / "review.woby";
    writeSceneDocument(path, document);
    const auto loaded = readSceneDocument(path);
    REQUIRE(loaded.comparisons.size() == 1);
    CHECK(loaded.comparisons[0].settings.task == AnalysisTask::surfaceComparison);
    CHECK(loaded.comparisons[0].settings.mode == ComparisonMode::original);
    REQUIRE(loaded.views.size() == 1);
    bool found = false;
    for (const auto& object : loaded.views[0].objects) {
        if (object.settings.comparison.task == AnalysisTask::surfaceComparison) { found = true; }
    }
    CHECK(found);
    { std::ofstream stream(path); stream << "version = 20\n[[analyses]]\nanalysis_mode = \"surface_quality\"\n"; }
    CHECK(analysisTask(readSceneDocument(path).comparisons[0].settings, true) == AnalysisTask::meshQuality);
    { std::ofstream stream(path); stream << "version = 21\n[[analyses]]\nanalysis_task = \"unknown\"\n"; }
    CHECK_THROWS((void)readSceneDocument(path));
}

TEST_CASE("analysis task switches preserve inputs thresholds and user names")
{
    AnalysisFixture f;
    selectSceneObject(f.state, f.state.files[1].objectId);
    const auto id = createAnalysisFromSelection(f.state, AnalysisTask::meshChecks);
    auto settings = comparisonSettings(f.state, id);
    settings.tolerance = .123f;
    settings.quality.maximumSize = 7;
    setComparisonSettings(f.state, settings, id);
    renameComparison(f.state, id, "My inspection");
    const auto members = findComparison(f.state, id)->a;
    setAnalysisTask(f.state, id, AnalysisTask::meshQuality);
    CHECK(comparisonSettings(f.state, id).mode == ComparisonMode::surfaceQuality);
    CHECK(comparisonSettings(f.state, id).quality.onOriginal);
    setAnalysisTask(f.state, id, AnalysisTask::surfaceComparison);
    CHECK(comparisonSettings(f.state, id).mode == ComparisonMode::distance);
    CHECK(analysisTask(comparisonSettings(f.state, id), false) == AnalysisTask::surfaceComparison);
    setAnalysisTask(f.state, id, AnalysisTask::meshChecks);
    CHECK(comparisonSettings(f.state, id).mode == ComparisonMode::original);
    CHECK(comparisonSettings(f.state, id).tolerance == doctest::Approx(.123));
    CHECK(comparisonSettings(f.state, id).quality.maximumSize == 7);
    CHECK(findComparison(f.state, id)->a == members);
    CHECK(findComparison(f.state, id)->name == "My inspection");
    clearComparisonGroup(f.state, ComparisonSide::a, id);
    setAnalysisTask(f.state, id, AnalysisTask::surfaceComparison);
    setComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::b, true, id);
    setAnalysisTask(f.state, id, AnalysisTask::meshQuality);
    CHECK_FALSE(comparisonSettings(f.state, id).quality.onOriginal);
    setAnalysisTask(f.state, id, AnalysisTask::meshChecks);
    CHECK(comparisonSettings(f.state, id).mode == ComparisonMode::original);
    CHECK(comparisonSettings(f.state, id).diagnosticSide == ComparisonSide::a);
    CHECK(findComparison(f.state, id)->b.empty());
}

TEST_CASE("mesh checks consolidate sources preserve disabled and missing parts and support undo")
{
    AnalysisFixture f;
    const auto id = createComparison(f.state);
    const auto first = f.state.files[0].objectId, second = f.state.files[1].objectId, third = f.state.files[2].objectId;
    setComparisonObjects(f.state, {first, third}, ComparisonSide::a, true, id);
    setComparisonObjects(f.state, {first, second}, ComparisonSide::b, true, id);
    setComparisonObjectsEnabled(f.state, {first, third}, ComparisonSide::a, false, id);
    auto* comparison = findComparison(f.state, id);
    comparison->a.push_back({invalidSceneObjectId, "Missing A", false});
    comparison->b.push_back({invalidSceneObjectId, "Missing B", false});
    comparison->diagnosticFocus = DiagnosticFocus{1, 0, ComparisonSide::b, DiagnosticCategory::boundary};
    const auto before = createSceneDocument(f.state);
    SceneHistory history; resetSceneHistory(history, f.state);
    const auto view = createView(f.state);
    setAnalysisTask(f.state, id, AnalysisTask::meshChecks);
    comparison = findComparison(f.state, id);
    CHECK(comparison->b.empty());
    CHECK(comparison->a.size() == 5);
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, id) == 3);
    CHECK(enabledComparisonPartCount(f.state, ComparisonSide::a, id) == 2);
    CHECK_FALSE(comparison->diagnosticFocus);
    CHECK(comparison->a[comparison->a.size() - 1].name == "Missing B");
    CHECK_FALSE(comparisonPartEnabled(f.state, f.state.files[2].groupSettings[0].objectId, ComparisonSide::a, id));
    applyView(f.state, view);
    CHECK(enabledComparisonPartCount(f.state, ComparisonSide::a, id) == 2);
    CHECK(std::all_of(findView(f.state, view)->parts.begin(), findView(f.state, view)->parts.end(),
        [](const UiViewPart& part) { return part.side == ComparisonSide::a; }));
    // Applying an earlier view can restore its task, but cannot resurrect removed membership sides.
    setAnalysisTask(f.state, id, AnalysisTask::meshChecks);
    const auto mesh = comparisonWorldMesh(f.state, ComparisonSide::a, id);
    CHECK(mesh.indices.size() == 6);
    REQUIRE(recordSceneHistory(history, f.state));
    auto undo = prepareSceneHistoryStep(history, f.state, before, false);
    REQUIRE(undo);
    commitSceneHistoryStep(history, f.state, std::move(*undo), false);
    CHECK(createSceneDocument(f.state).comparisons == before.comparisons);
}

TEST_CASE("mesh checks load both legacy member lists and retain merged sources through save and views")
{
    AnalysisFixture f;
    const auto id = createComparison(f.state);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::a, true, id);
    setComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::b, true, id);
    setComparisonObjectsEnabled(f.state, {}, ComparisonSide::b, false, id);
    // This represents an older Mesh checks scene with two input lists.
    findComparison(f.state, id)->settings.task = AnalysisTask::meshChecks;
    findComparison(f.state, id)->settings.mode = ComparisonMode::original;
    const auto view = createView(f.state);
    const auto path = f.root / "mesh-checks.woby";
    writeSceneDocument(path, createSceneDocument(f.state));
    auto document = readSceneDocument(path);
    for (auto& file : document.files) { file.path = sceneAbsolutePath(path, file.path); }
    auto loaded = prepareSceneReplacement(f.state, f.state.files, document);
    const auto loadedId = loaded.comparisons.front().objectId;
    CHECK(loaded.comparisons.front().b.empty());
    CHECK(comparisonPartCount(loaded, ComparisonSide::a, loadedId) == 2);
    CHECK(enabledComparisonPartCount(loaded, ComparisonSide::a, loadedId) == 1);
    setComparisonObjectsEnabled(loaded, {}, ComparisonSide::a, true, loadedId);
    applyView(loaded, loaded.views.front().id);
    CHECK(loaded.comparisons.front().b.empty());
    CHECK(enabledComparisonPartCount(loaded, ComparisonSide::a, loadedId) == 1);
    writeSceneDocument(path, createSceneDocument(loaded));
    const auto saved = readSceneDocument(path);
    CHECK(saved.comparisons.front().a.size() == 2);
    CHECK(saved.comparisons.front().b.empty());
    CHECK(findView(f.state, view));
}

TEST_CASE("legacy single B sources remain navigable and comparison categories do not change when inputs are disabled")
{
    AnalysisFixture f;
    const auto id = createComparison(f.state);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::b, true, id);
    CHECK(comparisonTask(f.state, id) == AnalysisTask::meshChecks);
    CHECK(comparisonSettings(f.state, id).diagnosticSide == ComparisonSide::b);
    const auto result = compareMeshes({}, comparisonWorldMesh(f.state, ComparisonSide::b, id));
    navigateComparisonDiagnostic(f.state, result, comparisonGeometrySignature(f.state, id), 1, id);
    REQUIRE(findComparison(f.state, id)->diagnosticFocus);
    CHECK(findComparison(f.state, id)->diagnosticFocus->side == ComparisonSide::b);
    setComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::a, true, id);
    auto settings = comparisonSettings(f.state, id); settings.mode = ComparisonMode::original;
    settings.diagnosticSide = ComparisonSide::a;
    setComparisonSettings(f.state, settings, id);
    CHECK(comparisonTask(f.state, id) == AnalysisTask::surfaceComparison);
    setComparisonObjectsEnabled(f.state, {}, ComparisonSide::b, false, id);
    CHECK(comparisonTask(f.state, id) == AnalysisTask::surfaceComparison);
    CHECK_FALSE(findComparison(f.state, id)->b.empty());
    clearComparisonGroup(f.state, ComparisonSide::a, id);
    CHECK(comparisonSettings(f.state, id).diagnosticSide == ComparisonSide::b);
    setComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::a, true, id);
    setAnalysisTask(f.state, id, AnalysisTask::meshChecks);
    setComparisonObjects(f.state, {f.state.files[2].objectId}, ComparisonSide::b, true, id);
    CHECK(findComparison(f.state, id)->b.empty());
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, id) == 3);
}

TEST_CASE("mesh checks source controls edit the single list and describe it without side labels")
{
    AnalysisFixture f;
    selectSceneObject(f.state, f.state.files[0].objectId);
    const auto id = createAnalysisFromSelection(f.state, AnalysisTask::meshChecks);
    setComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::b, true, id);
    setComparisonObjectsEnabled(f.state, {}, ComparisonSide::b, false, id);
    CHECK(enabledComparisonPartCount(f.state, ComparisonSide::a, id) == 0);
    CHECK(comparisonInputSummary(f.state, ComparisonSide::a, id).issue.find("All sources are turned off") == 0);
    isolateComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::b, id);
    CHECK(enabledComparisonPartCount(f.state, ComparisonSide::a, id) == 1);
    CHECK(comparisonPartEnabled(f.state, f.state.files[1].groupSettings[0].objectId, ComparisonSide::a, id));
    const auto before = createSceneDocument(f.state);
    const auto revision = f.state.sceneEditRevision;
    swapComparisonGroups(f.state, id);
    CHECK(createSceneDocument(f.state) == before);
    CHECK(f.state.sceneEditRevision == revision);
    findComparison(f.state, id)->a.push_back({invalidSceneObjectId, "Missing source", true});
    CHECK(comparisonInputSummary(f.state, ComparisonSide::a, id).issue.find("Sources have missing") == 0);
    removeMissingComparisonParts(f.state, ComparisonSide::b, id);
    CHECK(findComparison(f.state, id)->a.size() == 2);
    setComparisonObjects(f.state, {f.state.files[1].objectId}, ComparisonSide::b, false, id);
    CHECK(comparisonPartCount(f.state, ComparisonSide::a, id) == 1);
    clearComparisonGroup(f.state, ComparisonSide::b, id);
    CHECK(findComparison(f.state, id)->a.empty());
    CHECK(findComparison(f.state, id)->b.empty());
    CHECK(comparisonInputSummary(f.state, ComparisonSide::a, id).issue.find("No sources") == 0);
}

TEST_CASE("diagnostic summaries distinguish incomplete checks from zero findings")
{
    SurfaceComparison surface;
    surface.duplicates.points.availableSources = 1;
    const auto category = DiagnosticCategory::duplicatePoints;
    const auto ready = diagnosticSummary(surface, category, IntersectionPhase::complete, true);
    CHECK(ready.state == AnalysisResultState::ready);
    CHECK(diagnosticSummaryText(ready) == "0");
    CHECK(diagnosticSummary(surface, category, IntersectionPhase::complete, false).state == AnalysisResultState::outdated);
    const std::array phases = {IntersectionPhase::notChecked, IntersectionPhase::queued, IntersectionPhase::running,
        IntersectionPhase::outdated, IntersectionPhase::canceled, IntersectionPhase::failed};
    const std::array states = {AnalysisResultState::notRun, AnalysisResultState::queued, AnalysisResultState::running,
        AnalysisResultState::outdated, AnalysisResultState::canceled, AnalysisResultState::failed};
    for (size_t i = 0; i < phases.size(); ++i) {
        const auto pending = diagnosticSummary(surface, category, phases[i], false);
        CHECK(pending.state == states[i]);
        CHECK(diagnosticMatchesFilter(DiagnosticGroup::all, true, category, ready, pending, true, true));
        CHECK(diagnosticSummaryText(pending) != "0");
    }
    CHECK_FALSE(diagnosticMatchesFilter(DiagnosticGroup::all, true, category, ready, ready, true, true));
    surface.duplicates.points.duplicateCount = 12;
    surface.duplicates.points.findings.resize(3);
    auto findings = diagnosticSummary(surface, category, IntersectionPhase::complete, true);
    CHECK(findings.count == 12); // Counts are extra records, not duplicate groups.
    CHECK(diagnosticMatchesFilter(DiagnosticGroup::duplicates, true, category, findings, ready, true, false));
    CHECK_FALSE(diagnosticMatchesFilter(DiagnosticGroup::topology, false, category, findings, ready, true, true));
    surface.duplicates.points.unavailableSources = 1;
    CHECK(diagnosticSummary(surface, category, IntersectionPhase::complete, true).state == AnalysisResultState::partial);
    surface.duplicates.points.availableSources = 0;
    CHECK(diagnosticSummary(surface, category, IntersectionPhase::complete, true).state == AnalysisResultState::unavailable);
}

TEST_CASE("limited intersections and unavailable fin measurements cannot look clean")
{
    SurfaceComparison surface;
    surface.intersections.availableSources = 1;
    surface.intersections.truncated = true;
    const auto partial = diagnosticSummary(surface, DiagnosticCategory::selfIntersections, IntersectionPhase::complete, true);
    CHECK(partial.state == AnalysisResultState::partial);
    CHECK(diagnosticSummaryText(partial) == "0+ Partial");
    CHECK(diagnosticMatchesFilter(DiagnosticGroup::intersections, true, DiagnosticCategory::selfIntersections,
        partial, {}, true, false));
    surface.topology.availableSources = 2;
    surface.topology.unavailableFinAreaSources = 2;
    CHECK(diagnosticSummary(surface, DiagnosticCategory::fins, IntersectionPhase::complete, true).state == AnalysisResultState::unavailable);
    surface.topology.unavailableFinAreaSources = 1;
    CHECK(diagnosticSummary(surface, DiagnosticCategory::fins, IntersectionPhase::complete, true).state == AnalysisResultState::partial);
}

TEST_CASE("analysis diagnostic filters are validated session preferences")
{
    AnalysisFixture f;
    const auto document = createSceneDocument(f.state);
    const auto revision = f.state.sceneEditRevision;
    setDiagnosticFilter(f.state, DiagnosticGroup::duplicates, true);
    CHECK(createSceneDocument(f.state) == document);
    CHECK(f.state.sceneEditRevision == revision);
    const auto restored = prepareSceneReplacement(f.state, {}, createSceneDocument(UiState{}));
    CHECK(restored.diagnosticGroupFilter == DiagnosticGroup::duplicates);
    CHECK(restored.diagnosticFindingsOnly);
    setDiagnosticFilter(f.state, static_cast<DiagnosticGroup>(999), false);
    CHECK(f.state.diagnosticGroupFilter == DiagnosticGroup::all);
}

