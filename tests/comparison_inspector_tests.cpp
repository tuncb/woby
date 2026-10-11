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
#include <cmath>

TEST_CASE("distance range bar selects tolerance only on its marker and keeps the drag axis stable")
{
    auto* previous = ImGui::GetCurrentContext();
    auto* context = ImGui::CreateContext();
    struct Cleanup {
        ImGuiContext* context;
        ImGuiContext* previous;
        ~Cleanup() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
    } cleanup{context, previous};
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = {900, 700}; io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr; int atlasWidth = 0, atlasHeight = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
    woby::ComparisonSettings settings;
    settings.colorRange = 1;
    settings.colorRangeMode = woby::ComparisonColorRange::allSamples;
    woby::ComparisonRangeDrag drag;
    double sampleMaximum = 1;
    ImRect bounds;
    float markerMinX = 0, markerMaxX = 0;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({360, 160});
        ImGui::Begin("Range bar");
        woby::drawComparisonRangeControl(settings, drag, sampleMaximum);
        bounds = context->LastItemData.Rect;
        markerMinX = bounds.Max.x; markerMaxX = bounds.Min.x;
        for (const auto& vertex : ImGui::GetWindowDrawList()->VtxBuffer) {
            if (vertex.col == IM_COL32(71, 198, 224, 255)
                && vertex.pos.y >= bounds.Min.y + context->FontSize * .8f
                && vertex.pos.y <= bounds.Min.y + context->FontSize * 1.8f) {
                markerMinX = std::min(markerMinX, vertex.pos.x);
                markerMaxX = std::max(markerMaxX, vertex.pos.x);
            }
        }
        ImGui::End(); ImGui::EndFrame();
    };
    const auto click = [&](ImVec2 position) {
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    };
    frame(); frame();
    const auto original = settings;
    frame(); CHECK(settings == original);
    const float padding = context->FontSize * .5f;
    const float left = bounds.Min.x + padding, width = bounds.GetWidth() - 2 * padding;
    const float y = bounds.Min.y + context->FontSize;
    REQUIRE(markerMaxX > markerMinX);
    CHECK((markerMinX + markerMaxX) * .5f == doctest::Approx(left + width * settings.tolerance / drag.axisMaximum));
    settings.tolerance = .35f;
    const auto changedTolerance = settings;
    frame();
    CHECK(settings == changedTolerance);
    CHECK((markerMinX + markerMaxX) * .5f == doctest::Approx(left + width * .35f / drag.axisMaximum));
    settings = original; frame();
    const float markerY = bounds.Min.y + context->FontSize * 1.3f;
    const float markerX = left + width * settings.tolerance / drag.axisMaximum;
    click({markerX, markerY});
    CHECK(settings == original);
    io.AddMousePosEvent(markerX, markerY); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMousePosEvent(markerX + width * .2f, markerY); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    CHECK(settings.tolerance == doctest::Approx(.26).epsilon(.015));
    CHECK(settings.colorMinimum == original.colorMinimum);
    CHECK(settings.colorRange == original.colorRange);
    CHECK(settings.colorRangeMode == original.colorRangeMode);
    // The same x on the bar, outside the diamond, must edit a bound instead.
    const float tolerance = settings.tolerance;
    click({left + width * tolerance / drag.axisMaximum, bounds.Min.y + context->FontSize * .8f});
    CHECK(settings.tolerance == tolerance);
    CHECK(settings.colorMinimum > 0);
    settings = original; frame();
    click({left + width * .2f, y});
    CHECK(settings.colorRangeMode == woby::ComparisonColorRange::custom);
    CHECK(settings.colorMinimum == doctest::Approx(.21).epsilon(.02));
    CHECK(settings.colorRange == 1);
    CHECK(settings.tolerance == original.tolerance);
    const float lower = settings.colorMinimum;
    const float upperHandle = left + width / drag.axisMaximum;
    io.AddMousePosEvent(upperHandle, y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMousePosEvent(upperHandle - width * .2f, y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    CHECK(settings.colorRange == doctest::Approx(.79).epsilon(.005));
    CHECK(settings.colorMinimum == lower);
    CHECK(settings.tolerance == original.tolerance);
    CHECK(drag.axisMaximum == doctest::Approx(1.05));
    const auto afterUpperDrag = settings;
    for (int i = 0; i < 3; ++i) { frame(); }
    CHECK(settings == afterUpperDrag);
    CHECK(drag.axisMaximum == doctest::Approx(1.05));
    // A drag beyond the upper end extends the range instead of trapping it at the preset.
    const float movedUpperHandle = left + width * settings.colorRange / drag.axisMaximum;
    io.AddMousePosEvent(movedUpperHandle, y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMousePosEvent(movedUpperHandle + width * .5f, y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    CHECK(settings.colorRange > 1);
    CHECK(settings.colorMinimum == lower);
    // The triangle remains selectable when tolerance and the upper limit coincide.
    settings.tolerance = settings.colorRange; frame();
    const float triangleX = left + width * settings.colorRange / drag.axisMaximum;
    const float triangleY = bounds.Min.y + context->FontSize * .4f;
    const auto beforeOverlap = settings;
    io.AddMousePosEvent(triangleX, triangleY); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMousePosEvent(triangleX + width * .1f, triangleY); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    CHECK(settings.colorRange > beforeOverlap.colorRange);
    CHECK(settings.tolerance == beforeOverlap.tolerance);
    // A tolerance gesture stays on that marker even when dragged past a bound.
    const float overlapX = left + width * settings.tolerance / drag.axisMaximum;
    const auto beforeTolerance = settings;
    io.AddMousePosEvent(overlapX, markerY); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMousePosEvent(left + width * 2, markerY); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    CHECK(settings.tolerance == drag.axisMaximum);
    CHECK(settings.colorRange == beforeTolerance.colorRange);
    CHECK(settings.colorMinimum == beforeTolerance.colorMinimum);
    // A narrow custom interval still uses the full measured extent shown in Results.
    sampleMaximum = 1.7959;
    settings = original;
    settings.colorRangeMode = woby::ComparisonColorRange::custom;
    settings.colorRange = .45446f;
    frame();
    CHECK(drag.axisMaximum == doctest::Approx(1.7959 * 1.05));
    const auto measuredAxis = drag.axisMaximum;
    settings.colorRange = .14636f;
    settings.colorRangeMode = woby::ComparisonColorRange::max95;
    frame();
    CHECK(drag.axisMaximum == measuredAxis);
    settings.colorRange = .3f;
    settings.colorRangeMode = woby::ComparisonColorRange::custom;
    frame();
    CHECK(drag.axisMaximum == measuredAxis);
    // New measurement results reset the axis; editing bounds does not.
    sampleMaximum = 3;
    frame();
    CHECK(drag.axisMaximum == doctest::Approx(3.15));
    sampleMaximum = .5;
    frame();
    CHECK(drag.axisMaximum == doctest::Approx(.525));
}

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
        editMeshGeometry(mesh).vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
        mesh.bounds = calculateBounds(meshGeometry(mesh).vertices);
        for (size_t i = 0; i < groups; ++i) {
            editMeshGeometry(mesh).nodes.push_back({"part" + std::to_string(i), static_cast<uint32_t>(meshGeometry(mesh).indices.size()), 3});
            editMeshGeometry(mesh).indices.insert(meshGeometry(mesh).indices.end(), {0, 1, 2});
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

TEST_CASE("distance inspector applies maximum percentages without numeric range fields")
{
    InspectorFixture f;
    setAnalysisTask(f.state, f.id, AnalysisTask::surfaceComparison);
    setComparisonObjects(f.state, {f.state.files[0].objectId}, ComparisonSide::b, true, f.id);
    auto& runtime = f.runtimes.objects[f.id];
    runtime.results.signature = comparisonGeometrySignature(f.state, f.id);
    runtime.results.cache = {runtime.results.signature, comparisonSource | comparisonDistance};
    for (auto* surface : {&runtime.results.value.original, &runtime.results.value.repaired}) {
        surface->maximum = 6.1218; surface->mean = .062883;
        surface->percentile95 = .1796;
        surface->distances = {0, .2}; surface->sampleAreas = {88, 12};
    }
    f.frame(); f.frame();
    const auto before = createSceneDocument(f.state);
    const auto contents = f.frame(true);
    INFO(contents);
    CHECK(createSceneDocument(f.state) == before);
    for (const char* label : {"Measurement direction", "A -> B", "B -> A", "Measured mesh edges", "Mean distance"}) {
        CHECK(contents.find(label) != std::string::npos);
    }
    CHECK(contents.find("Upper limit") == std::string::npos);
    CHECK(contents.find("Color maximum") == std::string::npos);
    CHECK(contents.find("SATURATED") == std::string::npos);
    CHECK(contents.find("Allowed deviation") == std::string::npos);
    CHECK(contents.find("Measured surface") == std::string::npos);
    CHECK(contents.find("Acceptance tolerance") == std::string::npos);
    // Logging expands tree nodes for that frame. Restore the normal layout before clicking.
    f.frame(); f.frame();
    ImGuiWindow* editor = nullptr;
    for (auto* window : f.context->Windows) {
        if (std::string_view(window->Name).find("comparison_properties") != std::string_view::npos) { editor = window; break; }
    }
    REQUIRE(editor);
    auto& io = ImGui::GetIO();
    const auto click = [&](ImVec2 position) {
        io.AddMousePosEvent(position.x, position.y); f.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); f.frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); f.frame(); f.frame();
    };
    const std::array expected{6.1218 * .9, 6.1218 * .95, 6.1218 * .975, 6.1218 * .99, 6.1218};
    for (size_t choice = 1; choice < comparisonColorRangeLabels.size(); ++choice) {
        const auto* table = ImGui::TableFindByID(editor->GetID("distance_scale"));
        REQUIRE(table);
        click({table->Columns[1].WorkMinX + 20, table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + ImGui::GetFrameHeight() * .5f});
        REQUIRE_FALSE(f.context->OpenPopupStack.empty());
        const auto* popup = f.context->OpenPopupStack.back().Window;
        REQUIRE(popup);
        const auto start = popup->DC.CursorStartPos;
        click({start.x + 30, start.y + static_cast<float>(choice) * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeight() * .5f});
        const auto settings = comparisonSettings(f.state, f.id);
        CHECK(settings.colorRangeMode == static_cast<ComparisonColorRange>(choice));
        CHECK(settings.colorMinimum == 0);
        CHECK(settings.colorRange >= expected[choice - 1]);
        CHECK(settings.colorRange == doctest::Approx(expected[choice - 1]));
        CHECK(settings.tolerance == before.comparisons[0].settings.tolerance);
    }
    CHECK(f.frame(true).find("All sampled distances are covered") != std::string::npos);
    f.frame(); f.frame();
    // Direction buttons use the maximum of the selected measured surface.
    runtime.results.value.original.maximum = .3;
    runtime.results.value.repaired.maximum = .4;
    auto settings = comparisonSettings(f.state, f.id);
    settings.colorRangeMode = ComparisonColorRange::max95;
    setComparisonSettings(f.state, settings, f.id);
    f.frame();
    const auto* direction = ImGui::TableFindByID(editor->GetID("distance_measurement"));
    REQUIRE(direction);
    const float radioX = direction->Columns[1].WorkMinX;
    const float radioY = direction->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + ImGui::GetFrameHeight() * .5f;
    click({radioX + ImGui::GetFrameHeight() * .5f, radioY});
    CHECK(comparisonSettings(f.state, f.id).distanceOnOriginal);
    CHECK(comparisonSettings(f.state, f.id).colorRange == doctest::Approx(.3 * .95));
    const float radioWidth = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("A -> B").x;
    click({radioX + radioWidth + ImGui::GetStyle().ItemSpacing.x + ImGui::GetFrameHeight() * .5f, radioY});
    CHECK_FALSE(comparisonSettings(f.state, f.id).distanceOnOriginal);
    CHECK(comparisonSettings(f.state, f.id).colorRange == doctest::Approx(.4 * .95));
    // The diamond changes acceptance statistics through scene operations.
    settings = comparisonSettings(f.state, f.id);
    settings.colorRangeMode = ComparisonColorRange::custom;
    settings.colorMinimum = .1f; settings.colorRange = .5f;
    setComparisonSettings(f.state, settings, f.id);
    f.frame();
    ImVec2 diamondMin{io.DisplaySize.x, io.DisplaySize.y}, diamondMax{0, 0};
    // The first diamond is on the track; the second is the numeric key below it.
    for (const auto& vertex : editor->DrawList->VtxBuffer) {
        if (vertex.col == IM_COL32(71, 198, 224, 255)) {
            diamondMin.x = std::min(diamondMin.x, vertex.pos.x);
            diamondMin.y = std::min(diamondMin.y, vertex.pos.y);
        }
    }
    REQUIRE(diamondMin.y < io.DisplaySize.y);
    const float top = diamondMin.y;
    diamondMin.x = io.DisplaySize.x;
    for (const auto& vertex : editor->DrawList->VtxBuffer) {
        if (vertex.col == IM_COL32(71, 198, 224, 255) && vertex.pos.y <= top + ImGui::GetFontSize()) {
            diamondMin.x = std::min(diamondMin.x, vertex.pos.x);
            diamondMax.x = std::max(diamondMax.x, vertex.pos.x);
        }
    }
    const ImVec2 marker{(diamondMin.x + diamondMax.x) * .5f, top + ImGui::GetFontSize() * .5f};
    io.AddMousePosEvent(marker.x, marker.y); f.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); f.frame();
    io.AddMousePosEvent(marker.x + 160, marker.y); f.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); f.frame();
    CHECK(comparisonSettings(f.state, f.id).tolerance > .2f);
    CHECK(comparisonSettings(f.state, f.id).colorMinimum == settings.colorMinimum);
    CHECK(comparisonSettings(f.state, f.id).colorRange == settings.colorRange);
    CHECK(f.frame(true).find("100.00% of surface area within tolerance") != std::string::npos);
}

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
        auto replacement = meshGeometry(f.state.files[0].mesh).vertices;
        replacement[0].position[0] = 3;
        editMeshGeometry(f.state.files[0].mesh).vertices = std::move(replacement);
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
    for (const size_t groups : {1u, 256u, 2203u}) {
        InspectorFixture f(groups);
        auto& runtime = f.runtimes.objects[f.id];
        runtime.results.value.original.source = comparisonWorldMesh(f.state, ComparisonSide::a, f.id);
        runtime.results.value.original.diagnostics.boundaryEdges = {{{0, 0, 0}, {1, 0, 0}}};
        auto gpu = createGpuMesh(runtime.results.value.original.source, meshVertexLayout(), gpuMeshEdges);
        runtime.gpu.original.vertices = gpu.vertexBuffer;
        runtime.gpu.original.triangles = gpu.triangleIndexBuffer;
        if (woby::graphics::isValid(gpu.lineIndexBuffer)) { woby::graphics::destroy(gpu.lineIndexBuffer); }
        f.runtimes.program = program;
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
                    submitComparisonScenes(0, f.state, f.runtimes, program, scratch);
                    const auto helpers = std::chrono::steady_clock::now();
                    scratch.parts.clear();
                    appendVisibleComparisonPickParts(scratch.parts, f.state, f.runtimes);
                    submitSceneSelection(0, scratch.parts, f.state, helperLineVertexLayout(), program, scratch);
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
    graphics::destroy(program);
}
