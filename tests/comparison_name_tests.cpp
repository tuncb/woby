#include "comparison_view.h"
#include "comparison_scene.h"
#include "scene_file.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

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

TEST_CASE("analysis properties show computing only while the result is pending")
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
    bool computing = true;
    SUBCASE("pending") {}
    SUBCASE("ready") {
        runtime.ready = true;
        runtime.resultSignature = woby::comparisonGeometrySignature(f.state, f.id);
        computing = false;
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
    CHECK((contents.find("Computing analysis...") != std::string::npos) == computing);
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
