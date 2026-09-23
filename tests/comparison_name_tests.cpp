#include "comparison_view.h"
#include "comparison_scene.h"
#include "scene_file.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "ui_popup_controls.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <chrono>

namespace {
struct ComparisonNameFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    woby::UiState state;
    woby::ComparisonNameEdit edit;
    woby::SceneObjectId id = woby::createComparison(state);
    ImVec2 row;

    ComparisonNameFixture()
    {
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        frame();
        frame();
    }

    ~ComparisonNameFixture()
    {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }

    void frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(500, 400));
        ImGui::Begin("Scene");
        const auto origin = ImGui::GetCursorScreenPos();
        row = ImVec2(origin.x + 100, origin.y + ImGui::GetStyle().ItemSpacing.y
            + ImGui::GetTextLineHeightWithSpacing() + woby::renderModeButtonSize() * 0.5f);
        woby::drawComparisonObjects(state, edit);
        ImGui::End();
        ImGui::EndFrame();
    }

    void key(ImGuiKey key)
    {
        ImGui::GetIO().AddKeyEvent(key, true);
        frame();
        ImGui::GetIO().AddKeyEvent(key, false);
        frame();
        frame(); // Let ImGui finish focus routing and queued input transitions.
    }

    void click(ImVec2 position, ImGuiMouseButton button = ImGuiMouseButton_Left)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        frame();
        io.AddMouseButtonEvent(button, true);
        frame();
        io.AddMouseButtonEvent(button, false);
        frame();
    }

    void pause()
    {
        for (int i = 0; i < 30; ++i) { frame(); }
    }

    void type(const char* text)
    {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
    }
};
} // namespace

TEST_CASE("analysis input groups start collapsed")
{
    ComparisonNameFixture f;
    const auto addPart = [&](const char* filename, const char* partName, size_t index) {
        woby::Mesh mesh;
        mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
        mesh.indices = {0, 1, 2};
        mesh.nodes.push_back({partName, 0, 3});
        mesh.bounds = woby::calculateBounds(mesh.vertices);
        f.state.files.push_back(woby::createUiFileState(filename, std::move(mesh), index));
    };
    addPart("first.obj", "first-input-part", 0);
    addPart("second.obj", "second-input-part", 1);
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].groupSettings[0].objectId},
        woby::ComparisonSide::a, true, f.id);
    woby::setComparisonObjects(f.state, {f.state.files[1].groupSettings[0].objectId},
        woby::ComparisonSide::b, true, f.id);
    woby::selectSceneObject(f.state, f.id);

    woby::ComparisonRuntimes runtimes;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(700, 1000));
    ImGui::Begin("Analysis input groups");
    ImGui::LogToBuffer(0);
    woby::drawComparisonPanelContents(f.state, runtimes);
    const std::string contents = f.context->LogBuffer.c_str();
    ImGui::LogFinish();
    ImGui::End();
    ImGui::EndFrame();

    CHECK(contents.find("Group A") != std::string::npos);
    CHECK(contents.find("Group B") != std::string::npos);
    CHECK(contents.find("first-input-part") == std::string::npos);
    CHECK(contents.find("second-input-part") == std::string::npos);
}

TEST_CASE("analysis properties distinguish queued calculating failed and inactive results")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].groupSettings[0].objectId},
        woby::ComparisonSide::a, true, f.id);
    woby::selectSceneObject(f.state, f.id);
    REQUIRE(woby::canInspectComparison(f.state, f.id));
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    std::promise<woby::MeshComparison> pending;
    const char* status = "Queued...";
    SUBCASE("queued") {}
    SUBCASE("calculating") {
        runtime.worker = pending.get_future();
        runtime.workerSignature = woby::comparisonGeometrySignature(f.state, f.id);
        status = "Calculating analysis...";
    }
    SUBCASE("obsolete worker leaves new work queued") {
        runtime.worker = pending.get_future();
        runtime.workerSignature = woby::comparisonGeometrySignature(f.state, f.id) ^ 1;
    }
    SUBCASE("canceled worker leaves new work queued") {
        runtime.worker = pending.get_future();
        runtime.workerSignature = woby::comparisonGeometrySignature(f.state, f.id);
        runtime.stop.request_stop();
    }
    SUBCASE("failed") {
        runtime.error = "Test analysis failure";
        runtime.attemptedSignature = woby::comparisonGeometrySignature(f.state, f.id);
        status = "Analysis failed";
    }
    SUBCASE("obsolete error does not report failure") {
        runtime.error = "Old failure";
        runtime.attemptedSignature = woby::comparisonGeometrySignature(f.state, f.id) ^ 1;
    }
    SUBCASE("disabled") {
        auto settings = woby::comparisonSettings(f.state, f.id);
        settings.enabled = false;
        woby::setComparisonSettings(f.state, settings, f.id);
        status = "";
    }
    SUBCASE("invalid inputs") {
        woby::setComparisonObjects(f.state, {f.state.files[0].groupSettings[0].objectId},
            woby::ComparisonSide::a, false, f.id);
        status = "";
    }
    SUBCASE("ready") {
        runtime.ready = true;
        runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
        runtime.cache = {runtime.resultSignature, woby::requestedComparisonStages(woby::comparisonSettings(f.state, f.id), false)};
        for (auto& detector : runtime.result.detectors) { detector.phase = woby::IntersectionPhase::complete; }
        status = "";
    }
    SUBCASE("new quality view waits for its missing stage") {
        runtime.ready = true;
        runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
        runtime.cache = {runtime.resultSignature, woby::requestedComparisonStages(woby::comparisonSettings(f.state, f.id), false)};
        for (auto& detector : runtime.result.detectors) { detector.phase = woby::IntersectionPhase::complete; }
        auto settings = woby::comparisonSettings(f.state, f.id);
        settings.mode = woby::ComparisonMode::surfaceQuality;
        woby::setComparisonSettings(f.state, settings, f.id);
        CHECK_FALSE(woby::comparisonResultsReady(runtime, f.state, f.id));
        CHECK_FALSE(woby::comparisonsReadyForScreenshot(f.state, runtimes));
    }
    SUBCASE("outdated result") {
        runtime.ready = true;
        runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id) ^ 1;
    }
    std::string contents;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(700, 1200));
        ImGui::Begin("Analysis properties");
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::EndFrame();
    }
    CHECK(contents.find("Result position") != std::string::npos);
    CHECK(contents.find("Frame result") == std::string::npos);
    CHECK(contents.find("Result ready") == std::string::npos);
    for (const auto* label : {"Queued...", "Calculating analysis...", "Analysis failed"}) {
        CHECK((contents.find(label) != std::string::npos) == (std::string(status) == label));
    }
    CHECK(contents.find("Diagnostics") != std::string::npos);
    CHECK(contents.find("Updating...") == std::string::npos);
    CHECK((contents.find("Retry") != std::string::npos) == (std::string(status) == "Analysis failed"));
}

TEST_CASE("analysis status stays fixed while scrolling and retry preserves the editor position")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].groupSettings[0].objectId},
        woby::ComparisonSide::a, true, f.id);
    woby::selectSceneObject(f.state, f.id);
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    ImGuiWindow* activity = nullptr;
    ImGuiWindow* editor = nullptr;
    std::string contents;
    const auto frame = [&] {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({500, 350});
        ImGui::Begin("Scrolling analysis properties");
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        for (auto* window : f.context->Windows) {
            if (std::string(window->Name).find("comparison_activity") != std::string::npos) { activity = window; }
            if (std::string(window->Name).find("comparison_properties") != std::string::npos) { editor = window; }
        }
        ImGui::End();
        ImGui::EndFrame();
    };
    frame(); frame();
    REQUIRE(activity);
    REQUIRE(editor);
    REQUIRE(editor->ScrollMax.y > 100);
    const auto statusY = activity->Pos.y;
    const auto editorY = editor->Pos.y;
    ImGui::SetScrollY(editor, 100);
    frame(); frame();
    CHECK(editor->Scroll.y == doctest::Approx(100));
    CHECK(activity->Pos.y == statusY);
    CHECK(activity->Scroll.y == 0);
    CHECK(contents.find("Queued...") != std::string::npos);

    runtime.error = "Test failure";
    runtime.attemptedSignature = woby::comparisonGeometrySignature(f.state, f.id);
    frame();
    CHECK(contents.find("Analysis failed") != std::string::npos);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(activity->Pos.x + 15, activity->Pos.y + activity->Size.y * 0.5f); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame(); frame();
    CHECK(runtime.error.empty());
    CHECK(runtime.attemptedSignature == 0);
    CHECK(contents.find("Queued...") != std::string::npos);
    CHECK(editor->Scroll.y == doctest::Approx(100));

    runtime.ready = true;
    runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
    runtime.cache = {runtime.resultSignature, woby::requestedComparisonStages(woby::comparisonSettings(f.state, f.id), false)};
        for (auto& detector : runtime.result.detectors) { detector.phase = woby::IntersectionPhase::complete; }
    frame(); frame();
    CHECK(contents.find("Queued...") == std::string::npos);
    CHECK(contents.find("Updating...") == std::string::npos);
    CHECK(activity->Pos.y == statusY);
    CHECK(editor->Pos.y == editorY);
    CHECK(editor->Scroll.y == doctest::Approx(100));
}

TEST_CASE("new analyses use unique numbered names and preserve existing names")
{
    woby::UiState state;
    const auto named = woby::createComparison(state);
    CHECK(woby::findComparison(state, named)->name == "Analysis 1");
    woby::renameComparison(state, named, "Surface inspection");
    const auto first = woby::createComparison(state);
    const auto second = woby::createComparison(state);
    CHECK(woby::findComparison(state, first)->name == "Analysis 1");
    CHECK(woby::findComparison(state, second)->name == "Analysis 2");
    const auto copy = woby::duplicateComparison(state, first);
    CHECK(woby::findComparison(state, copy)->name == "Analysis 1 copy");
    CHECK(woby::findComparison(state, named)->name == "Surface inspection");
}

TEST_CASE("analysis names round trip and empty saved names use the analysis fallback")
{
    struct Fixture {
        std::filesystem::path root = std::filesystem::temp_directory_path()
            / ("woby-analysis-names-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Fixture() { std::filesystem::create_directories(root); }
        ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    } fixture;
    woby::UiState state;
    const auto id = woby::createComparison(state);
    std::string expected = "Analysis 1";
    SUBCASE("generated name") {}
    SUBCASE("custom name") {
        expected = "Surface quality";
        woby::renameComparison(state, id, expected);
    }
    SUBCASE("empty saved name") { expected = "Analysis"; }
    auto document = woby::createSceneDocument(state);
    if (expected == "Analysis") { document.comparisons[0].name.clear(); }
    const auto restored = woby::prepareSceneReplacement(state, {}, document);
    CHECK(restored.comparisons[0].name == expected);
    const auto path = fixture.root / "names.woby";
    woby::writeSceneDocument(path, document);
    const auto loaded = woby::readSceneDocument(path);
    REQUIRE(loaded.comparisons.size() == 1);
    CHECK(loaded.comparisons[0].name == expected);
}

TEST_CASE("analysis scene F2 edits locally and commits on Enter or focus loss")
{
    ComparisonNameFixture f;
    const auto original = woby::findComparison(f.state, f.id)->name;
    const auto revision = f.state.sceneEditRevision;
    f.key(ImGuiKey_F2);
    REQUIRE(f.edit.objectId == f.id);
    f.type("Inspection result");
    CHECK(woby::findComparison(f.state, f.id)->name == original);
    CHECK(f.state.sceneEditRevision == revision);
    SUBCASE("Enter") { f.key(ImGuiKey_Enter); }
    SUBCASE("click away") { f.click(ImVec2(450, 350)); }
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
    CHECK(woby::findComparison(f.state, f.id)->name == "Inspection result");
    CHECK(f.state.sceneEditRevision == revision + 1);
    CHECK(woby::createSceneDocument(f.state).comparisons[0].name == "Inspection result");
}

TEST_CASE("analysis scene Escape and unchanged names do not create edits")
{
    ComparisonNameFixture f;
    const auto original = woby::findComparison(f.state, f.id)->name;
    const auto revision = f.state.sceneEditRevision;
    f.key(ImGuiKey_F2);
    SUBCASE("Escape discards typed name") {
        f.type("Discard me");
        f.key(ImGuiKey_Escape);
    }
    SUBCASE("Enter without changes") { f.key(ImGuiKey_Enter); }
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
    CHECK(woby::findComparison(f.state, f.id)->name == original);
    CHECK(f.state.sceneEditRevision == revision);
}

TEST_CASE("analysis scene inline names support long text and empty input")
{
    ComparisonNameFixture f;
    const std::string longName(600, 'x');
    woby::renameComparison(f.state, f.id, longName);
    f.key(ImGuiKey_F2);
    REQUIRE(f.edit.text == longName);
    SUBCASE("long replacement") {
        const auto replacement = longName + " inspection";
        f.type(replacement.c_str());
        f.key(ImGuiKey_Enter);
        CHECK(woby::findComparison(f.state, f.id)->name == replacement);
    }
    SUBCASE("cleared name") {
        f.key(ImGuiKey_Backspace);
        f.key(ImGuiKey_Enter);
        CHECK(woby::findComparison(f.state, f.id)->name == "Analysis");
    }
}

TEST_CASE("analysis scene single clicks only select and double click renames")
{
    ComparisonNameFixture f;
    f.state.selectedSceneObjects.clear();
    f.click(f.row);
    f.pause();
    REQUIRE(woby::sceneObjectSelected(f.state, f.id));
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
    f.click(f.row);
    f.pause();
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
    f.click(f.row);
    f.click(f.row);
    f.frame();
    REQUIRE(f.edit.objectId == f.id);
    f.type("From the label");
    f.key(ImGuiKey_Enter);
    CHECK(woby::findComparison(f.state, f.id)->name == "From the label");
}

TEST_CASE("analysis scene F2 needs one target and deleting the target cancels renaming")
{
    ComparisonNameFixture f;
    const auto other = woby::createComparison(f.state);
    woby::selectSceneObject(f.state, f.id, true);
    f.key(ImGuiKey_F2);
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
    woby::selectSceneObject(f.state, f.id);
    f.key(ImGuiKey_F2);
    REQUIRE(f.edit.objectId == f.id);
    woby::removeComparison(f.state, f.id);
    f.frame();
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
    CHECK(woby::findComparison(f.state, other)->name == "Analysis 2");
}

TEST_CASE("analysis scene context menu Rename starts the inline editor")
{
    ComparisonNameFixture f;
    f.click(f.row, ImGuiMouseButton_Right);
    f.frame();
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    const auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    const auto start = popup->DC.CursorStartPos;
    f.click(ImVec2(start.x + 30, start.y + ImGui::GetTextLineHeightWithSpacing()
        + ImGui::GetTextLineHeight() * 0.5f));
    f.frame();
    REQUIRE(f.edit.objectId == f.id);
    f.frame();
    f.type("Menu rename");
    CHECK(f.edit.text == "Menu rename");
    f.key(ImGuiKey_Enter);
    CHECK(woby::findComparison(f.state, f.id)->name == "Menu rename");
}

TEST_CASE("analysis rename normalizes empty input and ignores missing objects")
{
    woby::UiState state;
    const auto id = woby::createComparison(state);
    woby::renameComparison(state, id, "");
    CHECK(woby::findComparison(state, id)->name == "Analysis");
    const auto revision = state.sceneEditRevision;
    woby::renameComparison(state, id, "");
    woby::renameComparison(state, id + 1, "Missing");
    CHECK(state.sceneEditRevision == revision);
}

TEST_CASE("diagnostics use count columns resizable dividers eyes and nearby detector settings popups")
{
    ComparisonNameFixture f;
    bool hasA = true, hasB = false, finSettings = false;
    SUBCASE("fin settings") { finSettings = true; }
    SUBCASE("one input A") {}
    SUBCASE("one input B") { hasA = false; hasB = true; }
    SUBCASE("two inputs") { hasB = true; }
    SUBCASE("no inputs") { hasA = hasB = false; }
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    const auto part = f.state.files[0].groupSettings[0].objectId;
    if (hasA) { woby::setComparisonObjects(f.state, {part}, woby::ComparisonSide::a, true, f.id); }
    if (hasB) { woby::setComparisonObjects(f.state, {part}, woby::ComparisonSide::b, true, f.id); }
    auto settings = woby::comparisonSettings(f.state, f.id);
    settings.mode = woby::ComparisonMode::original;
    woby::setComparisonSettings(f.state, settings, f.id);
    woby::selectSceneObject(f.state, f.id);
    woby::ComparisonRuntimes runtimes;
    ImGuiTable* diagnostics = nullptr;
    ImVec2 gear, eye, divider, intersectionRun, intersectionEye;
    std::array<ImVec2, woby::backgroundDetectorCount> updateButtons;
    std::array<ImVec2, woby::diagnosticCategoryCount> settingsButtons;
    std::array<ImVec2, woby::diagnosticCategoryCount> findingLabels;
    std::string contents;
    const auto frame = [&] {
        ImGui::GetIO().DisplaySize = ImVec2(1200, 1500);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(900, 1400));
        ImGui::Begin("Diagnostic controls");
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        for (auto* window : f.context->Windows) {
            if (std::string(window->Name).find("comparison_properties") == std::string::npos) { continue; }
            if (auto* table = ImGui::TableFindByID(window->GetID("Analysis diagnostics"))) {
                diagnostics = table;
                const float rowHeight = table->RowPosY2 - table->RowPosY1;
                const float holeY = table->RowPosY1 - (table->CurrentRow - (finSettings ? 4 : 3)) * rowHeight
                    + ImGui::GetStyle().CellPadding.y + woby::renderModeButtonSize() * 0.5f;
                gear = ImVec2(table->Columns[table->ColumnsCount - 1].WorkMinX
                    + woby::renderModeButtonSize() * 0.5f, holeY);
                eye = ImVec2(table->Columns[table->ColumnsCount - 3].WorkMinX
                    + woby::renderModeButtonSize() * 0.5f, holeY);
                const float intersectionY = table->RowPosY1 + ImGui::GetStyle().CellPadding.y
                    + woby::renderModeButtonSize()*0.5f;
                intersectionRun = ImVec2(table->Columns[0].WorkMinX + woby::renderModeButtonSize()*0.5f, intersectionY);
                for (size_t row = 0; row < updateButtons.size(); ++row) {
                    updateButtons[row] = ImVec2(intersectionRun.x,
                        intersectionY - static_cast<float>(updateButtons.size() - row) * rowHeight);
                }
                for (size_t row = 0; row < settingsButtons.size(); ++row) {
                    settingsButtons[row] = ImVec2(gear.x, intersectionY - static_cast<float>(settingsButtons.size() - 1 - row) * rowHeight);
                    findingLabels[row] = ImVec2(table->Columns[1].WorkMinX + 5, settingsButtons[row].y);
                }
                intersectionEye = ImVec2(table->Columns[table->ColumnsCount - 3].WorkMinX + woby::renderModeButtonSize()*0.5f, intersectionY);
                divider = ImVec2(table->Columns[2].MaxX,
                    table->OuterRect.Min.y + ImGui::GetTextLineHeight() * 0.5f);
            }
        }
        ImGui::End();
        woby::dismissPopupOnEscape();
        ImGui::EndFrame();
    };
    const auto click = [&](ImVec2 position) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame(); frame();
    };
    for (int warmup = 0; warmup < 5; ++warmup) { frame(); }
    REQUIRE(diagnostics);
    CHECK((contents.find(hasA && hasB ? "Count A" : "Count") != std::string::npos) == (hasA || hasB));
    CHECK((contents.find("Count B") != std::string::npos) == (hasA && hasB));
    CHECK(contents.find("\xef\x80\x93") != std::string::npos); // Settings glyph.
    REQUIRE((diagnostics->Flags & ImGuiTableFlags_Resizable) != 0);
    auto& io = ImGui::GetIO();
    const char* hintFragments[] = {
        "edges used by only one triangle", "one connected fan or ring", "simple closed boundary loops",
        "candidate patches", "edges shared by more than two triangles", "conflicting winding",
        "exactly equal source coordinates", "same three source point indices", "collapsed or collinear triangles",
        "triangle pairs that intersect",
    };
    const auto settingsBeforeHints = woby::comparisonSettings(f.state, f.id);
    const auto revisionBeforeHints = f.state.sceneEditRevision;
    for (size_t row = 0; row < findingLabels.size(); ++row) {
        CAPTURE(row);
        io.AddMousePosEvent(findingLabels[row].x, findingLabels[row].y);
        frame(); frame();
        CHECK(contents.find(hintFragments[row]) != std::string::npos);
        CHECK(woby::comparisonSettings(f.state, f.id) == settingsBeforeHints);
        CHECK(f.state.sceneEditRevision == revisionBeforeHints);
    }
    if (!hasA && !hasB) {
        for (const auto phase : {woby::IntersectionPhase::notChecked, woby::IntersectionPhase::outdated, woby::IntersectionPhase::failed}) {
            auto& result = runtimes.objects[f.id].result;
            result.original.intersections.phase = phase;
            for (auto& status : result.detectors) { status.phase = phase; }
            frame();
            click(intersectionRun);
            CHECK(woby::findComparison(f.state, f.id)->intersectionRequestRevision == 0);
            CHECK(contents.find("No input. Add an enabled input") != std::string::npos);
            for (const auto button : updateButtons) { click(button); }
            for (const auto& request : woby::findComparison(f.state, f.id)->detectorRequests) { CHECK(request.revision == 0); }
        }
        return;
    }
    const woby::DiagnosticCategory rowCategories[] = {
        woby::DiagnosticCategory::boundary, woby::DiagnosticCategory::nonManifoldVertices,
        woby::DiagnosticCategory::holes, woby::DiagnosticCategory::fins, woby::DiagnosticCategory::nonManifold,
        woby::DiagnosticCategory::winding, woby::DiagnosticCategory::duplicatePoints,
        woby::DiagnosticCategory::duplicateTriangles, woby::DiagnosticCategory::degenerateTriangles,
        woby::DiagnosticCategory::selfIntersections,
    };
    for (size_t row = 0; row < updateButtons.size(); ++row) {
        const auto before = woby::comparisonSettings(f.state, f.id);
        const auto revision = f.state.sceneEditRevision;
        click(updateButtons[row]);
        CHECK(woby::comparisonSettings(f.state, f.id) == before);
        CHECK(f.state.sceneEditRevision == revision);
        const auto& request = woby::findComparison(f.state, f.id)->detectorRequests[static_cast<size_t>(rowCategories[row])];
        CHECK(request.revision == 1);
        CHECK_FALSE(request.cancel);
        CHECK(contents.find("Run detection") != std::string::npos);
    }
    for (size_t row = 0; row < settingsButtons.size(); ++row) {
        const auto before = woby::comparisonSettings(f.state, f.id);
        click(settingsButtons[row]);
        REQUIRE_FALSE(f.context->OpenPopupStack.empty());
        const auto* settingsPopup = f.context->OpenPopupStack.back().Window;
        REQUIRE(settingsPopup);
        CHECK(contents.find("Automatic updates") != std::string::npos);
        const auto start = settingsPopup->DC.CursorStartPos;
        const ImVec2 automatic(start.x + ImGui::GetFrameHeight() * .5f,
            start.y + ImGui::GetTextLineHeightWithSpacing() + 1 + ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight() * .5f);
        click(automatic);
        auto expected = before;
        woby::setDiagnosticAutoUpdate(expected, rowCategories[row], !woby::diagnosticAutoUpdate(before, rowCategories[row]));
        CHECK(woby::comparisonSettings(f.state, f.id) == expected);
        click(automatic);
        CHECK(woby::comparisonSettings(f.state, f.id) == before);
        io.AddKeyEvent(ImGuiKey_Escape, true); frame();
        io.AddKeyEvent(ImGuiKey_Escape, false); frame(); frame();
        CHECK(f.context->OpenPopupStack.empty());
    }
    CHECK_FALSE(woby::comparisonSettings(f.state, f.id).intersections.autoUpdate);
    const auto runRevision = f.state.sceneEditRevision;
    click(intersectionRun);
    CHECK(woby::findComparison(f.state, f.id)->intersectionRequestRevision == 1);
    CHECK(f.state.sceneEditRevision == runRevision);
    CHECK_FALSE(woby::comparisonSettings(f.state, f.id).intersections.autoUpdate);
    click(intersectionEye);
    CHECK_FALSE(woby::comparisonSettings(f.state, f.id).intersections.show);
    CHECK_FALSE(woby::comparisonSettings(f.state, f.id).intersections.autoUpdate);
    click(intersectionEye);
    auto& inspection = runtimes.objects[f.id].result.original.intersections;
    struct ActionCase {
        woby::IntersectionPhase phase;
        const char* icon;
        const char* hint;
        bool cancel;
    };
    const ActionCase actions[] = {
        {woby::IntersectionPhase::notChecked, "\xef\x81\x8b", "Run detection", false},
        {woby::IntersectionPhase::queued, "\xef\x81\x8d", "Queued. Cancel", true},
        {woby::IntersectionPhase::running, "\xef\x81\x8d", "Running. Cancel", true},
        {woby::IntersectionPhase::complete, "\xef\x80\x9e", "Complete. Rerun", false},
        {woby::IntersectionPhase::outdated, "\xef\x80\xa1", "Out of date. Update", false},
        {woby::IntersectionPhase::canceled, "\xef\x80\x9e", "Canceled. Retry", false},
        {woby::IntersectionPhase::failed, "\xef\x80\x9e", "Failed. Retry", false},
    };
    for (const auto& action : actions) {
        CAPTURE(action.hint);
        inspection.phase = action.phase;
        inspection.error = action.phase == woby::IntersectionPhase::failed ? "Test check failure" : "";
        runtimes.objects[f.id].result.repaired.intersections.phase = action.phase;
        frame();
        const auto request = woby::findComparison(f.state, f.id)->intersectionRequestRevision;
        const auto revision = f.state.sceneEditRevision;
        click(intersectionRun);
        CHECK(contents.find(action.icon) != std::string::npos);
        CHECK(contents.find(action.hint) != std::string::npos);
        if (action.phase == woby::IntersectionPhase::failed) {
            CHECK(contents.find("Test check failure") != std::string::npos);
        }
        CHECK(woby::findComparison(f.state, f.id)->cancelIntersections == action.cancel);
        CHECK(woby::findComparison(f.state, f.id)->intersectionRequestRevision == request + 1);
        CHECK(f.state.sceneEditRevision == revision);
        CHECK_FALSE(woby::comparisonSettings(f.state, f.id).intersections.autoUpdate);
    }
    inspection.phase = woby::IntersectionPhase::notChecked; frame();
    const auto original = woby::comparisonSettings(f.state, f.id);
    click(eye);
    auto hidden = original;
    if (finSettings) { hidden.topologyInspection.showFins = !hidden.topologyInspection.showFins; }
    else { hidden.topologyInspection.showHoles = !hidden.topologyInspection.showHoles; }
    CHECK(woby::comparisonSettings(f.state, f.id) == hidden);
    click(eye);
    CHECK(woby::comparisonSettings(f.state, f.id) == original);
    const auto revision = f.state.sceneEditRevision;
    const auto openingGear = gear;
    click(gear);
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    const auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    CHECK(popup->Pos.y > openingGear.y);
    CHECK(popup->Pos.y < openingGear.y + woby::renderModeButtonSize());
    CHECK(popup->Pos.x + popup->Size.x == doctest::Approx(openingGear.x + woby::renderModeButtonSize() * 0.5f).epsilon(0.002));
    CHECK(popup->Size.x == doctest::Approx(woby::uiSize(360)));
    CHECK(popup->Pos.y + popup->Size.y < io.DisplaySize.y);
    CHECK(contents.find(finSettings ? "Maximum area ratio" : "Maximum size ratio") != std::string::npos);
    CHECK(f.state.sceneEditRevision == revision);
    REQUIRE(f.context->InputTextState.ID != 0);
    io.AddInputCharactersUTF8("0.125"); frame();
    io.AddKeyEvent(ImGuiKey_Enter, true); frame();
    io.AddKeyEvent(ImGuiKey_Enter, false); frame(); frame();
    const auto& updated = woby::comparisonSettings(f.state, f.id).topologyInspection;
    CHECK((finSettings ? updated.finMaxAreaRatio : updated.holeSizeRatioTolerance) == doctest::Approx(0.125));
    io.AddKeyEvent(ImGuiKey_Escape, true); frame();
    io.AddKeyEvent(ImGuiKey_Escape, false); frame(); frame();
    CHECK(f.context->OpenPopupStack.empty());
    const float countWidth = diagnostics->Columns[2].WidthGiven;
    io.AddMousePosEvent(divider.x, divider.y); frame(); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    CHECK(diagnostics->ResizedColumn == 2);
    io.AddMousePosEvent(divider.x + 30, divider.y); frame(); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame(); frame();
    CHECK(diagnostics->Columns[2].WidthGiven > countWidth + 20);
}

TEST_CASE("diagnostic settings popup edits only its analysis and persists without inline editors")
{
    ComparisonNameFixture f;
    const auto other = woby::createComparison(f.state);
    woby::selectSceneObject(f.state, f.id);
    auto settings = woby::comparisonSettings(f.state, f.id);
    settings.diagnosticCategory = woby::DiagnosticCategory::degenerateTriangles;
    settings.degenerates.enabled = false;
    woby::setComparisonSettings(f.state, settings, f.id);
    const auto originalOther = woby::comparisonSettings(f.state, other);
    const auto revision = f.state.sceneEditRevision;
    woby::ComparisonRuntimes runtimes;
    ImVec2 gear;
    std::string contents;
    const auto frame = [&] {
        ImGui::GetIO().DisplaySize = ImVec2(900, 1000);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(20, 20));
        ImGui::SetNextWindowSize(ImVec2(650, 900));
        ImGui::Begin("Diagnostic properties");
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        for (auto* window : f.context->Windows) {
            if (std::string(window->Name).find("comparison_properties") == std::string::npos) { continue; }
            if (const auto* table = ImGui::TableFindByID(window->GetID("Analysis diagnostics"))) {
                const auto& column = table->Columns[table->ColumnsCount-1];
                gear = ImVec2(column.WorkMinX + woby::renderModeButtonSize()*0.5f,
                    table->RowPosY1 - (table->CurrentRow - 9)*(table->RowPosY2-table->RowPosY1)
                    + ImGui::GetStyle().CellPadding.y + woby::renderModeButtonSize()*0.5f);
            }
        }
        ImGui::End();
        woby::dismissPopupOnEscape();
        ImGui::EndFrame();
    };
    const auto click = [&](ImVec2 position) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame(); frame();
    };
    const auto key = [&](ImGuiKey value) {
        ImGui::GetIO().AddKeyEvent(value, true); frame();
        ImGui::GetIO().AddKeyEvent(value, false); frame(); frame();
    };
    frame(); frame();
    CHECK(contents.find("Needle edge ratio") == std::string::npos);
    CHECK(contents.find("Cap angle (degrees)") == std::string::npos);
    REQUIRE(gear.x > 0);
    click(gear);
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    CHECK(contents.find("Needle edge ratio") != std::string::npos);
    CHECK(contents.find("Cap angle (degrees)") != std::string::npos);
    CHECK(contents.find("Analysis: Analysis 1") != std::string::npos);
    CHECK(f.state.sceneEditRevision == revision); // Opening settings is not an edit.
    REQUIRE(f.context->InputTextState.ID != 0);
    ImGui::GetIO().AddInputCharactersUTF8("2500"); frame();
    key(ImGuiKey_Enter);
    CHECK(woby::comparisonSettings(f.state, f.id).degenerates.needleThresholdRatio == 2500);
    CHECK_FALSE(woby::comparisonSettings(f.state, f.id).degenerates.enabled);
    CHECK(woby::comparisonSettings(f.state, other) == originalOther);
    CHECK(f.state.sceneEditRevision > revision);
    key(ImGuiKey_Escape);
    CHECK(f.context->OpenPopupStack.empty());
    CHECK(contents.find("Needle edge ratio") == std::string::npos);
    click(gear);
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    CHECK(woby::comparisonSettings(f.state, f.id).degenerates.needleThresholdRatio == 2500);
    // Switching analyses must never reuse the first analysis's open parameter frame.
    woby::selectSceneObject(f.state, other);
    frame(); frame();
    CHECK(contents.find("Needle edge ratio") == std::string::npos);
    CHECK(woby::comparisonSettings(f.state, other) == originalOther);
    struct SavedFixture {
        std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
            / ("woby-diagnostic-popup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        SavedFixture() { std::filesystem::create_directory(root); }
        ~SavedFixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    } saved;
    const auto path = saved.root / "analyses.woby";
    const auto document = woby::createSceneDocument(f.state);
    woby::writeSceneDocument(path, document);
    const auto loaded = woby::readSceneDocument(path);
    REQUIRE(loaded.comparisons.size() == 2);
    CHECK(loaded.comparisons[0].settings == woby::comparisonSettings(f.state, f.id));
    CHECK(loaded.comparisons[1].settings == originalOther);
}

TEST_CASE("fin findings use table columns and continuous one based indices on pages of 25")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].groupSettings[0].objectId},
        woby::ComparisonSide::a, true, f.id);
    auto settings = woby::comparisonSettings(f.state, f.id);
    settings.mode = woby::ComparisonMode::original;
    settings.diagnosticCategory = woby::DiagnosticCategory::fins;
    woby::setComparisonSettings(f.state, settings, f.id);
    woby::selectSceneObject(f.state, f.id);
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    runtime.ready = true;
    runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
    runtime.cache = {runtime.resultSignature, woby::requestedComparisonStages(settings, false) | woby::comparisonDiagnosticStage(settings.diagnosticCategory)};
    for (auto& detector : runtime.result.detectors) { detector.phase = woby::IntersectionPhase::complete; }
    auto& surface = runtime.result.original;
    surface.topology.sources.resize(1);
    surface.topology.sources[0].source = "defect-source.obj";
    surface.topology.sources[0].faces.resize(101);
    surface.finBounds.resize(27);
    for (size_t i = 0; i < 27; ++i) {
        woby::TopologyFinPatch patch;
        patch.patch = i + 100;
        patch.boundary = woby::FinBoundaryKind::branched;
        for (size_t face = 0; face < 101; ++face) { patch.faces.push_back(face); }
        surface.topology.finPatches.push_back(patch);
        surface.topology.fins.push_back(i);
    }
    const auto frame = [&] {
        ImGui::GetIO().DisplaySize = ImVec2(1200, 2200);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1100, 2100));
        ImGui::Begin("Defect table");
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        const std::string contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::EndFrame();
        return contents;
    };
    frame();
    auto contents = frame();
    INFO(contents);
    CHECK(contents.find("Index") != std::string::npos);
    CHECK(contents.find("Source") != std::string::npos);
    CHECK(contents.find("Patch") != std::string::npos);
    CHECK(contents.find("Boundary") != std::string::npos);
    CHECK(contents.find("| 1 | defect-source.obj | 101 | branched |") != std::string::npos);
    CHECK(contents.find("| 25 | defect-source.obj | 125 | branched |") != std::string::npos);
    CHECK(contents.find("| 26 | defect-source.obj |") == std::string::npos);
    woby::selectComparisonDiagnostic(f.state, runtime.result, runtime.resultSignature, 25, f.id);
    contents = frame();
    CHECK(contents.find("| 26 | defect-source.obj | 126 | branched |") != std::string::npos);
    CHECK(contents.find("| 27 | defect-source.obj | 127 | branched |") != std::string::npos);
    CHECK(contents.find("| 1 | defect-source.obj |") == std::string::npos);
    CHECK(contents.find("| 25 | defect-source.obj |") == std::string::npos);
    CHECK(contents.find("101 faces; area") != std::string::npos);
    CHECK(contents.find("Triangle 1, part") == std::string::npos);
    CHECK(contents.find("Showing first 100 faces.") == std::string::npos);
}
