#include "comparison_view.h"
#include "comparison_scene.h"
#include "scene_file.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"
#include "ui_popup_controls.h"
#include "ui_views.h"
#include "uv_quality.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace {
struct ComparisonNameFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    woby::UiState state;
    woby::ComparisonNameEdit edit;
    woby::ViewNameEdit viewEdit;
    woby::ViewListLayout viewLayout;
    woby::SceneObjectId id = woby::createComparison(state);
    ImVec2 row;
    ImVec2 viewRow;
    std::string viewContents;
    bool showViews = false;

    explicit ComparisonNameFixture(bool withViews = false) : showViews(withViews)
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
        if (showViews) {
            ImGui::LogToBuffer();
            woby::drawViews(state, viewEdit, viewLayout, 80.0f);
            viewContents = context->LogBuffer.c_str();
            ImGui::LogFinish();
            for (auto* window : context->Windows) {
                if (std::string(window->Name).find("view_rows") == std::string::npos) { continue; }
                viewRow = ImVec2(window->Pos.x + 50.0f,
                    window->Pos.y + ImGui::GetStyle().WindowPadding.y + woby::renderModeButtonSize() * 0.5f);
                break;
            }
        }
        woby::drawComparisonObjects(state, edit);
        if (const auto* table = ImGui::TableFindByID(ImGui::GetID("analysis_header"))) {
            row = ImVec2(origin.x + 100, table->OuterRect.Max.y + ImGui::GetStyle().ItemSpacing.y
                + woby::renderModeButtonSize() * 0.5f);
        }
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

TEST_CASE("analysis rows show only names and reveal metadata in the hover hint")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].objectId}, woby::ComparisonSide::a, true, f.id);
    woby::renameComparison(f.state, f.id, "Inspection one");
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    runtime.ready = true;
    runtime.sidebarInputs[0].enabledPartCount = 1;
    runtime.sidebarSources = "inspection-source.obj";
    for (auto& check : runtime.diagnosticSummaries) { check[0].state = woby::AnalysisResultState::ready; }
    runtime.diagnosticSummaries[0][0].count = 3;
    runtime.diagnosticSummaries[1][0].state = woby::AnalysisResultState::notRun;
    const char* expectedTask = "Mesh checks";
    const char* expectedStatus = "Checks pending";
    bool missing = false;
    SUBCASE("checks include the finding summary") {}
    SUBCASE("quality includes the result status") {
        woby::setAnalysisTask(f.state, f.id, woby::AnalysisTask::meshQuality);
        expectedTask = "Mesh quality"; expectedStatus = "Ready";
    }
    SUBCASE("input issues appear directly in the hint") {
        woby::clearComparisonGroup(f.state, woby::ComparisonSide::a, f.id);
        runtime.sidebarInputs[0].enabledPartCount = 0;
        runtime.sidebarInputs[0].issue = "Add an enabled source to this input.";
        expectedStatus = "Needs inputs"; missing = true;
    }
    runtime.sidebarRevision = f.state.sceneEditRevision;
    std::string contents;
    float endY = 0;
    ImVec2 row;
    const auto frame = [&] {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({500, 400});
        ImGui::Begin("Compact analysis rows"); ImGui::LogToBuffer(0);
        woby::drawComparisonObjects(f.state, f.edit, runtimes);
        const auto minimum = ImGui::GetItemRectMin();
        row = {ImGui::GetWindowPos().x + 100, minimum.y + woby::renderModeButtonSize() * .5f};
        endY = ImGui::GetCursorPosY();
        contents = f.context->LogBuffer.c_str(); ImGui::LogFinish(); ImGui::End(); ImGui::EndFrame();
    };
    ImGui::GetIO().AddMousePosEvent(-1000, -1000);
    frame(); frame();
    const auto oneRowEnd = endY;
    CHECK(contents.find("Inspection one") != std::string::npos);
    CHECK(contents.find(expectedTask) == std::string::npos);
    CHECK(contents.find("Sources:") == std::string::npos);
    CHECK(contents.find("checks with findings") == std::string::npos);
    const auto before = woby::createSceneDocument(f.state);
    const auto revision = f.state.sceneEditRevision;
    ImGui::GetIO().AddMousePosEvent(row.x, row.y);
    frame(); frame();
    INFO(contents);
    CHECK(contents.find(expectedTask) != std::string::npos);
    CHECK(contents.find(expectedStatus) != std::string::npos);
    CHECK(contents.find("Sources: inspection-source.obj") != std::string::npos);
    CHECK(contents.find("Double-click to rename") != std::string::npos);
    if (missing) { CHECK(contents.find(runtime.sidebarInputs[0].issue) != std::string::npos); }
    else if (woby::comparisonSettings(f.state, f.id).mode != woby::ComparisonMode::surfaceQuality) {
        CHECK(contents.find("1 checks with findings | 1 not run") != std::string::npos);
    }
    CHECK(endY == doctest::Approx(oneRowEnd));
    CHECK(woby::createSceneDocument(f.state) == before);
    CHECK(f.state.sceneEditRevision == revision);
    ImGui::GetIO().AddMousePosEvent(-1000, -1000);
    const auto second = woby::createComparison(f.state);
    woby::renameComparison(f.state, second, "Inspection two");
    frame(); frame();
    CHECK(contents.find("Inspection two") != std::string::npos);
    CHECK(contents.find("Sources:") == std::string::npos);
    CHECK(endY - oneRowEnd == doctest::Approx(woby::renderModeButtonSize() + ImGui::GetStyle().ItemSpacing.y));
}

TEST_CASE("analysis compact add button opens an anchored menu while the section is collapsed")
{
    ComparisonNameFixture f;
    auto* window = ImGui::FindWindowByName("Scene");
    REQUIRE(window);
    const auto header = window->GetID("Analyses");
    window->StateStorage.SetInt(header, 0);
    f.frame(); f.frame();
    const auto* table = ImGui::TableFindByID(window->GetID("analysis_header"));
    REQUIRE(table);
    const float size = woby::renderModeButtonSize();
    const ImVec2 buttonMin{table->Columns[2].WorkMinX, table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y};
    CHECK(table->Columns[2].WidthGiven == doctest::Approx(size));
    const auto count = f.state.comparisons.size();
    f.click({buttonMin.x + size * .5f, buttonMin.y + size * .5f});
    f.frame(); f.frame(); // Let the auto-sized popup settle before checking or clicking it.
    REQUIRE(f.context->OpenPopupStack.Size == 1);
    auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    CHECK(popup->Pos.y >= buttonMin.y + size);
    CHECK(popup->Pos.y <= buttonMin.y + size + ImGui::GetStyle().ItemSpacing.y + 1);
    CHECK(window->StateStorage.GetInt(header, 1) == 0);
    f.click({popup->Pos.x + ImGui::GetStyle().WindowPadding.x + 30,
        popup->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetTextLineHeight() * .5f});
    REQUIRE(f.state.comparisons.size() == count + 1);
    CHECK(f.state.comparisons.back().settings.task == woby::AnalysisTask::meshChecks);
    CHECK(f.state.selectedSceneObjects == std::vector<woby::SceneObjectId>{f.state.comparisons.back().objectId});
}

TEST_CASE("analysis sidebar lists every task without a filter for more than four analyses")
{
    ComparisonNameFixture f;
    for (const auto task : {woby::AnalysisTask::meshChecks, woby::AnalysisTask::meshQuality,
            woby::AnalysisTask::surfaceComparison, woby::AnalysisTask::uvInspection, woby::AnalysisTask::meshChecks}) {
        woby::createAnalysisFromSelection(f.state, task);
    }
    REQUIRE(f.state.comparisons.size() == 6);
    const auto hidden = f.state.comparisons.back().objectId;
    auto settings = woby::comparisonSettings(f.state, hidden);
    settings.enabled = false;
    woby::setComparisonSettings(f.state, settings, hidden);
    const auto before = woby::createSceneDocument(f.state);
    std::string contents;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({500, 400});
        ImGui::Begin("Scene"); ImGui::LogToBuffer();
        woby::drawComparisonObjects(f.state, f.edit);
        contents = f.context->LogBuffer.c_str(); ImGui::LogFinish(); ImGui::End(); ImGui::EndFrame();
    }
    INFO(contents);
    for (const auto& analysis : f.state.comparisons) { CHECK(contents.find(analysis.name) != std::string::npos); }
    CHECK(contents.find("All analysis tasks") == std::string::npos);
    CHECK(woby::createSceneDocument(f.state) == before);
    f.frame(); f.click(f.row);
    CHECK(f.state.selectedSceneObjects == std::vector<woby::SceneObjectId>{f.id});
    CHECK_FALSE(woby::comparisonSettings(f.state, hidden).enabled);
}

TEST_CASE("analysis rows use the full width and delete only through the context menu")
{
    ComparisonNameFixture f;
    woby::renameComparison(f.state, f.id, std::string(300, 'x'));
    const auto other = woby::createComparison(f.state);
    SUBCASE("unselected row") {}
    SUBCASE("selected row") { woby::selectSceneObject(f.state, f.id); }
    SUBCASE("renaming row") {
        woby::selectSceneObject(f.state, f.id);
        f.key(ImGuiKey_F2);
        REQUIRE(f.edit.objectId == f.id);
    }
    SUBCASE("different task row") {
        woby::setAnalysisTask(f.state, other, woby::AnalysisTask::meshQuality);
    }
    f.frame(); f.frame();
    const auto revision = f.state.sceneEditRevision;
    const auto otherName = woby::findComparison(f.state, other)->name;
    const auto* window = ImGui::FindWindowByName("Scene");
    REQUIRE(window);
    f.click({window->WorkRect.Max.x - woby::renderModeButtonSize() * .5f, f.row.y});
    f.frame();
    REQUIRE(woby::findComparison(f.state, f.id));
    CHECK(f.state.comparisons.size() == 2);
    CHECK(f.state.sceneEditRevision == revision);
    if (f.edit.objectId != woby::invalidSceneObjectId) { f.key(ImGuiKey_Escape); }
    f.click({f.row.x, f.row.y}, ImGuiMouseButton_Right);
    f.frame();
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    const auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    const auto start = popup->DC.CursorStartPos;
    f.click({start.x + 30, start.y + 4 * ImGui::GetTextLineHeightWithSpacing()
        + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight() * .5f});
    f.frame();
    CHECK(woby::findComparison(f.state, f.id) == nullptr);
    REQUIRE(f.state.comparisons.size() == 1);
    CHECK(f.state.comparisons[0].objectId == other);
    CHECK(f.state.comparisons[0].name == otherName);
    CHECK_FALSE(woby::sceneObjectSelected(f.state, f.id));
    CHECK(f.state.sceneEditRevision == revision + 1);
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
}

TEST_CASE("file analysis menus preserve selection until a task is chosen and use the explicit file")
{
    ComparisonNameFixture f;
    for (size_t i = 0; i < 2; ++i) {
        woby::Mesh mesh;
        mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
        mesh.indices = {0, 1, 2};
        mesh.nodes.push_back({"surface", 0, 3});
        mesh.bounds = woby::calculateBounds(mesh.vertices);
        f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), i));
    }
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::createComparison(f.state);
    woby::selectSceneObject(f.state, f.id, true);
    woby::selectSceneObject(f.state, f.state.files[0].objectId, true);
    REQUIRE(f.state.selectedSceneObjects.size() == 3);
    woby::setPropertiesPaneVisible(f.state, false);
    const auto source = f.state.files[1].objectId;
    const auto selection = f.state.selectedSceneObjects;
    const auto before = woby::createSceneDocument(f.state);
    const auto revision = f.state.sceneEditRevision;
    const auto active = f.state.activeComparisonId;
    const auto dirty = f.state.isDirty;
    bool open = true;
    std::string contents;
    const auto frame = [&] {
        ImGui::NewFrame(); ImGui::SetNextWindowPos({20, 20}); ImGui::SetNextWindowSize({500, 400});
        ImGui::Begin("File analysis menu");
        if (open) { ImGui::OpenPopup("analysis_type"); open = false; }
        if (ImGui::BeginPopup("analysis_type")) {
            ImGui::LogToBuffer();
            woby::drawAnalysisCreationMenu(f.state, {source});
            contents = f.context->LogBuffer.c_str(); ImGui::LogFinish();
            ImGui::EndPopup();
        }
        ImGui::End(); ImGui::EndFrame();
    };
    frame(); frame();
    REQUIRE(f.context->OpenPopupStack.Size == 1);
    for (const char* label : {"Mesh checks", "Mesh quality", "Surface comparison", "UV inspection"}) {
        CHECK(contents.find(label) != std::string::npos);
    }
    CHECK(f.state.selectedSceneObjects == selection);
    CHECK(f.state.activeComparisonId == active);
    CHECK_FALSE(f.state.propertiesPaneVisible);
    CHECK(f.state.sceneEditRevision == revision);
    CHECK(f.state.isDirty == dirty);
    CHECK(woby::createSceneDocument(f.state) == before);
    int menuIndex = 0;
    SUBCASE("dismiss without changes") { menuIndex = -1; }
    SUBCASE("mesh checks") {}
    SUBCASE("mesh quality") { menuIndex = 1; }
    SUBCASE("comparison ignores unrelated multi-selection") { menuIndex = 2; }
    SUBCASE("UV inspection") { menuIndex = 3; }
    auto& io = ImGui::GetIO();
    if (menuIndex < 0) {
        io.AddMousePosEvent(480, 360); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
        CHECK(f.context->OpenPopupStack.empty());
        CHECK(f.state.selectedSceneObjects == selection);
        CHECK_FALSE(f.state.propertiesPaneVisible);
        CHECK(f.state.sceneEditRevision == revision);
        CHECK(woby::createSceneDocument(f.state) == before);
        return;
    }
    const auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    io.AddMousePosEvent(popup->DC.CursorStartPos.x + 30, popup->DC.CursorStartPos.y
        + static_cast<float>(menuIndex) * ImGui::GetTextLineHeightWithSpacing() + ImGui::GetTextLineHeight() * .5f);
    frame(); io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    REQUIRE(f.state.comparisons.size() == before.comparisons.size() + 1);
    const auto& created = f.state.comparisons.back();
    CHECK(created.settings.task == static_cast<woby::AnalysisTask>(menuIndex + 1));
    CHECK(woby::comparisonContains(f.state, f.state.files[1].groupSettings[0].objectId,
        woby::ComparisonSide::a, created.objectId));
    CHECK_FALSE(woby::comparisonContains(f.state, f.state.files[0].groupSettings[0].objectId,
        woby::ComparisonSide::a, created.objectId));
    CHECK(created.b.empty());
    CHECK(f.state.selectedSceneObjects == std::vector<woby::SceneObjectId>{created.objectId});
}

TEST_CASE("analysis header eye controls every analysis independently of collapse and task")
{
    ComparisonNameFixture f;
    const auto other = woby::createAnalysisFromSelection(f.state, woby::AnalysisTask::uvInspection);
    auto settings = woby::comparisonSettings(f.state, other);
    settings.enabled = false;
    woby::setComparisonSettings(f.state, settings, other);
    auto* window = ImGui::FindWindowByName("Scene");
    REQUIRE(window);
    const auto header = window->GetID("Analyses");
    SUBCASE("collapsed") { window->StateStorage.SetInt(header, 0); }
    SUBCASE("expanded") { window->StateStorage.SetInt(header, 1); }
    const auto open = window->StateStorage.GetInt(header);
    f.frame(); f.frame();
    const auto* table = ImGui::TableFindByID(window->GetID("analysis_header"));
    REQUIRE(table);
    const float size = woby::renderModeButtonSize();
    CHECK(table->Columns[0].WidthGiven == doctest::Approx(size));
    const ImVec2 eye{table->Columns[0].WorkMinX + size * .5f,
        table->OuterRect.Min.y + ImGui::GetStyle().CellPadding.y + size * .5f};
    const auto selection = f.state.selectedSceneObjects;
    const auto revision = f.state.sceneEditRevision;
    f.click(eye); // Mixed -> show all, including the other task.
    CHECK(woby::comparisonSettings(f.state, f.id).enabled);
    CHECK(woby::comparisonSettings(f.state, other).enabled);
    CHECK(f.state.sceneEditRevision == revision + 1);
    f.click(eye);
    CHECK_FALSE(woby::comparisonSettings(f.state, f.id).enabled);
    CHECK_FALSE(woby::comparisonSettings(f.state, other).enabled);
    CHECK(f.state.sceneEditRevision == revision + 2);
    f.click(eye);
    CHECK(woby::comparisonSettings(f.state, f.id).enabled);
    CHECK(woby::comparisonSettings(f.state, other).enabled);
    CHECK(f.state.selectedSceneObjects == selection);
    CHECK(window->StateStorage.GetInt(header) == open);
    woby::removeComparison(f.state, f.id);
    woby::removeComparison(f.state, other);
    const auto emptyRevision = f.state.sceneEditRevision;
    f.frame(); f.click(eye);
    CHECK(f.state.sceneEditRevision == emptyRevision);
    CHECK(window->StateStorage.GetInt(header) == open);
}

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

TEST_CASE("analysis inspectors keep measurements separate from diagnostic findings")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, mesh, 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    for (const auto side : {woby::ComparisonSide::a, woby::ComparisonSide::b}) {
        woby::setComparisonObjects(f.state, {f.state.files[0].objectId}, side, true, f.id);
    }
    auto task = woby::AnalysisTask::meshChecks;
    SUBCASE("checks") {}
    SUBCASE("quality") { task = woby::AnalysisTask::meshQuality; }
    SUBCASE("comparison") { task = woby::AnalysisTask::surfaceComparison; }
    woby::setAnalysisTask(f.state, f.id, task);
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
    runtime.cache = {runtime.resultSignature, woby::comparisonSource | woby::comparisonQuality | woby::comparisonDistance};
    runtime.result = woby::computeComparisonStages(mesh, mesh, runtime.cache.completed);
    std::string contents;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({600, 1800});
        ImGui::Begin("Task inspector");
        ImGui::LogToBuffer(0);
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::End();
        ImGui::EndFrame();
    }
    INFO(contents);
    CHECK((contents.find("Diagnostics") != std::string::npos) == (task == woby::AnalysisTask::meshChecks));
    CHECK((contents.find("Size limits") != std::string::npos) == (task == woby::AnalysisTask::meshQuality));
    CHECK((contents.find("Area-weighted mean") != std::string::npos) == (task == woby::AnalysisTask::surfaceComparison));
    CHECK(contents.find("Placement") != std::string::npos);
    CHECK(contents.find("Result position") == std::string::npos);
}

TEST_CASE("analysis UV inspector shows all findings and only relevant normalization controls")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    for (int i = 0; i < 125; ++i) { mesh.indices.insert(mesh.indices.end(), {0, 1, 2}); }
    mesh.nodes.push_back({"Missing UV patch", 0, 375});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].objectId}, woby::ComparisonSide::a, true, f.id);
    auto settings = woby::comparisonSettings(f.state, f.id);
    settings.type = woby::AnalysisType::uvQuality;
    settings.uvView = woby::UvView::surface;
    SUBCASE("angle") { settings.uvMetric = woby::UvQualityMetric::angle; }
    SUBCASE("area") { settings.uvMetric = woby::UvQualityMetric::area; }
    SUBCASE("orientation") { settings.uvMetric = woby::UvQualityMetric::orientation; }
    woby::setComparisonSettings(f.state, settings, f.id);
    woby::selectSceneObject(f.state, f.id);
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
    runtime.cache = {runtime.resultSignature, woby::comparisonSource};
    runtime.result.original.source = woby::comparisonWorldMesh(f.state, woby::ComparisonSide::a, f.id);
    std::string contents;
    const auto before = woby::createSceneDocument(f.state);
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::NewFrame(); ImGui::SetNextWindowSize({360, 1800});
        ImGui::Begin("UV task inspector"); ImGui::LogToBuffer(0);
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str(); ImGui::LogFinish(); ImGui::End(); ImGui::EndFrame();
    }
    INFO(contents);
    CHECK(contents.find("Layout") != std::string::npos);
    CHECK(contents.find("Distortion") != std::string::npos);
    CHECK(contents.find("Missing UV triangles") != std::string::npos);
    CHECK(contents.find("125 | Missing UVs") != std::string::npos);
    CHECK(contents.find("Full result") != std::string::npos);
    CHECK(contents.find("Diagnostics") == std::string::npos);
    CHECK((contents.find("Area normalization") != std::string::npos) == (settings.uvMetric == woby::UvQualityMetric::area));
    CHECK(woby::createSceneDocument(f.state) == before);
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
    CHECK(contents.find("Placement") != std::string::npos);
    CHECK(contents.find("Frame result") == std::string::npos);
    CHECK(contents.find("Result ready") == std::string::npos);
    for (const auto* label : {"Queued...", "Calculating analysis...", "Analysis failed"}) {
        CHECK((contents.find(label) != std::string::npos) == (std::string(status) == label));
    }
    CHECK((contents.find("Diagnostics") != std::string::npos)
        == (woby::comparisonSettings(f.state, f.id).mode != woby::ComparisonMode::surfaceQuality));
    CHECK(contents.find("Updating...") == std::string::npos);
    CHECK((contents.find("Retry") != std::string::npos) == (std::string(status) == "Analysis failed"));
}

TEST_CASE("analysis name shares the first row with its label without an idle status gap")
{
    ComparisonNameFixture f;
    auto task = woby::AnalysisTask::meshChecks;
    float scale = 1.0f;
    SUBCASE("mesh checks") {}
    SUBCASE("UV inspection") { task = woby::AnalysisTask::uvInspection; }
    SUBCASE("UV inspection at double scale") { task = woby::AnalysisTask::uvInspection; scale = 2.0f; }
    auto& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
    f.id = woby::createAnalysisFromSelection(f.state, task);
    woby::ComparisonRuntimes runtimes;
    ImGuiWindow* editor = nullptr;
    ImVec2 contentStart;
    std::string contents;
    const auto frame = [&] {
        ImGui::GetIO().DisplaySize = {1000 * scale, 1800 * scale};
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({300 * scale, 1600 * scale});
        ImGui::Begin("Compact analysis properties");
        ImGui::TextUnformatted("Properties");
        ImGui::Separator();
        contentStart = ImGui::GetCursorScreenPos();
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        for (auto* window : f.context->Windows) {
            if (window->ParentWindow == ImGui::GetCurrentWindow()
                && std::string(window->Name).find("comparison_properties") != std::string::npos) { editor = window; }
        }
        ImGui::End();
        ImGui::EndFrame();
    };
    frame(); frame();
    REQUIRE(editor);
    CHECK(editor->Pos.y == doctest::Approx(contentStart.y));
    const auto label = contents.find("Name");
    REQUIRE(label != std::string::npos);
    const auto lineEnd = contents.find('\n', label);
    CHECK(contents.find(woby::findComparison(f.state, f.id)->name, label) < lineEnd);
    auto& io = ImGui::GetIO();
    const float inputX = editor->DC.CursorStartPos.x + ImGui::CalcTextSize("Name").x + style.ItemSpacing.x;
    io.AddMousePosEvent(inputX + 20 * scale, editor->DC.CursorStartPos.y + ImGui::GetFrameHeight() * .5f); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    REQUIRE(f.context->InputTextState.ID == editor->GetID("##comparison_name"));
    io.AddKeyEvent(ImGuiMod_Ctrl, true); frame();
    io.AddKeyEvent(ImGuiKey_A, true); frame();
    io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false); frame();
    io.AddInputCharactersUTF8("Inspection review"); frame();
    io.AddKeyEvent(ImGuiKey_Enter, true); frame();
    io.AddKeyEvent(ImGuiKey_Enter, false); frame();
    CHECK(woby::findComparison(f.state, f.id)->name == "Inspection review");
}

TEST_CASE("analysis status stays fixed while active and collapses without losing the editor scroll position")
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
            if (window->ParentWindow != ImGui::GetCurrentWindow()) { continue; }
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
    CHECK_FALSE(activity->Active);
    CHECK(editor->Pos.y == statusY);
    CHECK(editor->Pos.y < editorY);
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

TEST_CASE("F2 renames the focused view even when an analysis is selected")
{
    ComparisonNameFixture f(true);
    const auto first = woby::createView(f.state);
    woby::createView(f.state);
    f.frame();
    REQUIRE(f.viewRow.x > 0.0f);
    f.click(f.viewRow);
    REQUIRE(f.state.activeViewId == first);
    REQUIRE(woby::sceneObjectSelected(f.state, f.id));
    f.key(ImGuiKey_F2);
    CHECK(f.viewEdit.id == first);
    CHECK(f.edit.objectId == woby::invalidSceneObjectId);
}

TEST_CASE("view rows retain save and delete through the context menu")
{
    ComparisonNameFixture f(true);
    const auto id = woby::createView(f.state);
    f.frame();
    REQUIRE(f.viewRow.x > 0.0f);
    // The trailing action is Save; clicking where Delete used to be keeps the view.
    const ImGuiWindow* rows = nullptr;
    for (const auto* window : f.context->Windows) {
        if (std::string(window->Name).find("view_rows") != std::string::npos) { rows = window; break; }
    }
    REQUIRE(rows);
    f.click({rows->WorkRect.Max.x - woby::renderModeButtonSize() * .5f, f.viewRow.y});
    REQUIRE(woby::findView(f.state, id));
    f.click(f.viewRow, ImGuiMouseButton_Right);
    f.frame();
    CHECK(f.viewContents.find("Rename") != std::string::npos);
    CHECK(f.viewContents.find("Duplicate") != std::string::npos);
    CHECK(f.viewContents.find("Delete view") != std::string::npos);
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    const auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    const auto start = popup->DC.CursorStartPos;
    f.click({start.x + 30, start.y + 4 * ImGui::GetTextLineHeightWithSpacing()
        + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight() * .5f});
    CHECK(woby::findView(f.state, id) == nullptr);
    CHECK(f.state.views.empty());
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
    f.click(ImVec2(start.x + 30, start.y + 2 * ImGui::GetTextLineHeightWithSpacing()
        + ImGui::GetStyle().ItemSpacing.y + ImGui::GetTextLineHeight() * 0.5f));
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

TEST_CASE("diagnostics show all checks including zero counts and pending results without filters")
{
    ComparisonNameFixture f;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes.push_back({"surface", 0, 3});
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    auto source = std::make_shared<woby::SourceMeshData>();
    source->provenance = woby::SourceProvenance::objPositions;
    for (const auto& vertex : mesh.vertices) {
        source->points.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});
    }
    source->indices = mesh.indices;
    mesh.sourceData = std::move(source);
    f.state.files.push_back(woby::createUiFileState({}, std::move(mesh), 0));
    woby::appendDefaultSceneNodesForFiles(f.state, 0);
    woby::setComparisonObjects(f.state, {f.state.files[0].objectId}, woby::ComparisonSide::a, true, f.id);
    woby::setAnalysisTask(f.state, f.id, woby::AnalysisTask::meshChecks);
    woby::selectSceneObject(f.state, f.id);
    woby::ComparisonRuntimes runtimes;
    auto& runtime = runtimes.objects[f.id];
    runtime.ready = true;
    runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
    runtime.cache = {runtime.resultSignature, woby::comparisonSource | woby::comparisonDetectors};
    runtime.result = woby::computeComparisonStages(
        woby::comparisonWorldMesh(f.state, woby::ComparisonSide::a, f.id), {}, runtime.cache.completed);
    REQUIRE(woby::comparisonDetectorReady(runtime, f.state, f.id, woby::DiagnosticCategory::nonManifold));
    REQUIRE(runtime.result.original.topology.availableSources == 1);
    REQUIRE(runtime.result.original.topology.nonManifoldEdges.empty());
    std::string contents;
    ImGuiTable* diagnostics = nullptr;
    for (int frame = 0; frame < 2; ++frame) {
        ImGui::GetIO().DisplaySize = ImVec2(1000, 1800);
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({900, 1700});
        ImGui::Begin("All diagnostic checks");
        ImGui::LogToBuffer();
        woby::drawComparisonPanelContents(f.state, runtimes);
        contents = f.context->LogBuffer.c_str();
        ImGui::LogFinish();
        for (auto* window : f.context->Windows) {
            if (std::string(window->Name).find("comparison_properties") == std::string::npos) { continue; }
            diagnostics = ImGui::TableFindByID(window->GetID("Analysis diagnostics"));
        }
        ImGui::End();
        ImGui::EndFrame();
    }
    INFO(contents);
    REQUIRE(diagnostics);
    CHECK(diagnostics->CurrentRow == 10); // Header plus every diagnostic category.
    CHECK(contents.find("| 0 |") != std::string::npos);
    CHECK(contents.find("Not run") != std::string::npos);
    CHECK(contents.find("All check groups") == std::string::npos);
    CHECK(contents.find("With findings") == std::string::npos);
    CHECK(contents.find("No findings match this filter") == std::string::npos);
}

TEST_CASE("diagnostics keep run visibility and settings independent with nearby popups")
{
    ComparisonNameFixture f;
    bool hasA = true, hasB = false, finSettings = false, legacy = false;
    SUBCASE("fin settings") { finSettings = true; }
    SUBCASE("one input A") {}
    SUBCASE("one input B") { hasA = false; hasB = true; }
    SUBCASE("legacy input B") { hasA = false; hasB = true; legacy = true; }
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
    settings.task = legacy ? woby::AnalysisTask::automatic : woby::AnalysisTask::meshChecks;
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
                eye = ImVec2(table->Columns[table->ColumnsCount - 2].WorkMinX
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
                intersectionEye = ImVec2(table->Columns[table->ColumnsCount - 2].WorkMinX + woby::renderModeButtonSize()*0.5f, intersectionY);

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
    const auto diagnosticsHeading = contents.find("Diagnostics");
    REQUIRE(diagnosticsHeading != std::string::npos);
    const auto headingRow = contents.substr(diagnosticsHeading,
        contents.find('\n', diagnosticsHeading) - diagnosticsHeading);
    const auto targetLabel = headingRow.find("Target");
    CHECK(targetLabel == std::string::npos);
    CHECK(contents.find("Count") != std::string::npos);
    CHECK(contents.find("| A |") == std::string::npos);
    CHECK(contents.find("| B |") == std::string::npos);
    CHECK(contents.find("Sources") != std::string::npos);
    CHECK(contents.find("Group A") == std::string::npos);
    CHECK(contents.find("Group B") == std::string::npos);
    CHECK(contents.find("Input A") == std::string::npos);
    CHECK(contents.find("Input B") == std::string::npos);
    CHECK(contents.find("Swap inputs") == std::string::npos);
    CHECK(contents.find("\xef\x80\x93") != std::string::npos); // Settings glyph.
    REQUIRE((diagnostics->Flags & ImGuiTableFlags_ScrollX) == 0);
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
    CHECK(diagnostics->Columns[diagnostics->ColumnsCount - 1].WorkMinX + woby::renderModeButtonSize()
        <= diagnostics->OuterRect.Max.x);

}

TEST_CASE("diagnostics keep readable labels and reachable controls as properties narrow")
{
    ComparisonNameFixture f;
    bool hasA = true, hasB = true;
    float scale = 1.0f;
    SUBCASE("two inputs") {}
    SUBCASE("only input A") { hasB = false; }
    SUBCASE("only input B") { hasA = false; }
    SUBCASE("no inputs") { hasA = hasB = false; }
    SUBCASE("two inputs at double UI scale") { scale = 2.0f; }
    auto& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleMain = scale;
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
    settings.task = woby::AnalysisTask::meshChecks;
    woby::setComparisonSettings(f.state, settings, f.id);
    woby::selectSceneObject(f.state, f.id);
    woby::ComparisonRuntimes runtimes;
    ImGuiTable* diagnostics = nullptr;
    float paneWidth = 900.0f;
    const auto frame = [&] {
        ImGui::GetIO().DisplaySize = {1200 * scale, 1600 * scale};
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({paneWidth * scale, 1500 * scale});
        ImGui::Begin("Responsive diagnostic properties");
        woby::drawComparisonPanelContents(f.state, runtimes);
        for (auto* window : f.context->Windows) {
            if (auto* table = ImGui::TableFindByID(window->GetID("Analysis diagnostics"))) {
                diagnostics = table;
            }
        }
        ImGui::End();
        ImGui::EndFrame();
    };
    const auto settle = [&] { frame(); frame(); frame(); };
    const auto click = [&](ImVec2 position) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); settle();
    };
    for (const float width : {900.0f, 440.0f, 340.0f, 300.0f, 900.0f}) {
        paneWidth = width;
        settle();
        CAPTURE(width);
        CAPTURE(scale);
        CAPTURE(hasA);
        CAPTURE(hasB);
        REQUIRE(diagnostics);
        CHECK(diagnostics->ColumnsCount == 5);
        const float findingWidth = diagnostics->Columns[1].WorkMaxX - diagnostics->Columns[1].WorkMinX;
        CHECK(findingWidth > 0);
        CHECK((diagnostics->Flags & ImGuiTableFlags_ScrollX) == 0);
        CHECK(diagnostics->InnerWindow == diagnostics->OuterWindow);
        const auto& gearColumn = diagnostics->Columns[diagnostics->ColumnsCount - 1];
        CHECK(gearColumn.WorkMinX >= diagnostics->OuterRect.Min.x);
        CHECK(gearColumn.WorkMinX + woby::renderModeButtonSize() <= diagnostics->OuterRect.Max.x);
        CHECK(diagnostics->Columns[0].WorkMinX >= diagnostics->OuterRect.Min.x);
        const float buttonSize = woby::renderModeButtonSize();
        CHECK(diagnostics->InstanceDataFirst.LastTopHeadersRowHeight >= buttonSize + style.CellPadding.y * 2);
        const auto& visibilityColumn = diagnostics->Columns[3];
        CHECK(visibilityColumn.WorkMinX >= diagnostics->Columns[2].WorkMaxX);
        CHECK(visibilityColumn.WorkMinX + buttonSize <= diagnostics->OuterRect.Max.x);
        const ImVec2 masterEye(visibilityColumn.WorkMinX + buttonSize * .5f,
            diagnostics->OuterRect.Min.y + style.CellPadding.y + buttonSize * .5f);
        // Exercise the header control at every width and scale: mixed -> all -> none -> all.
        const auto original = woby::comparisonSettings(f.state, f.id);
        woby::setComparisonDiagnosticsVisible(f.state, false, f.id);
        auto mixed = woby::comparisonSettings(f.state, f.id);
        mixed.showBoundaries = true;
        woby::setComparisonSettings(f.state, mixed, f.id);
        settle();
        for (const auto expected : {woby::diagnosticCategoryCount, size_t{0}, woby::diagnosticCategoryCount}) {
            click(masterEye);
            CHECK(woby::countVisibleComparisonDiagnostics(woby::comparisonSettings(f.state, f.id)) == expected);
        }
        woby::setComparisonSettings(f.state, original, f.id);
    }

    // The settings action remains reachable after repeatedly changing the pane width.
    paneWidth = 440.0f;
    settle();
    const auto& gearColumn = diagnostics->Columns[diagnostics->ColumnsCount - 1];
    // Use a wide pane for deterministic row coordinates; narrow wrapping is verified above.
    paneWidth = 900.0f;
    settle();
    const float rowHeight = diagnostics->RowPosY2 - diagnostics->RowPosY1;
    const ImVec2 gear(gearColumn.WorkMinX + woby::renderModeButtonSize() * .5f,
        diagnostics->RowPosY1 - (diagnostics->CurrentRow - 3) * rowHeight
        + style.CellPadding.y + woby::renderModeButtonSize() * .5f);
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(gear.x, gear.y); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); settle();
    REQUIRE_FALSE(f.context->OpenPopupStack.empty());
    const auto* popup = f.context->OpenPopupStack.back().Window;
    REQUIRE(popup);
    CHECK(popup->Pos.x <= gear.x + woby::renderModeButtonSize() * .5f);
    CHECK(popup->Pos.x + popup->Size.x >= gear.x - woby::renderModeButtonSize() * .5f);
    CHECK(std::abs(popup->Pos.y - (gear.y + woby::renderModeButtonSize() * .5f + style.ItemSpacing.y)) < 2 * scale);
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
