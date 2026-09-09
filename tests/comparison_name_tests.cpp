#include "comparison_view.h"
#include "scene_file.h"
#include "ui_operations.h"
#include "ui_icon_controls.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

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

TEST_CASE("comparison scene F2 edits locally and commits on Enter or focus loss")
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

TEST_CASE("comparison scene Escape and unchanged names do not create edits")
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

TEST_CASE("comparison scene inline names support long text and empty input")
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
        CHECK(woby::findComparison(f.state, f.id)->name == "Comparison");
    }
}

TEST_CASE("comparison scene single clicks only select and double click renames")
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

TEST_CASE("comparison scene F2 needs one target and deleting the target cancels renaming")
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
    CHECK(woby::findComparison(f.state, other)->name == "Comparison 2");
}

TEST_CASE("comparison scene context menu Rename starts the inline editor")
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

TEST_CASE("comparison rename normalizes empty input and ignores missing objects")
{
    woby::UiState state;
    const auto id = woby::createComparison(state);
    woby::renameComparison(state, id, "");
    CHECK(woby::findComparison(state, id)->name == "Comparison");
    const auto revision = state.sceneEditRevision;
    woby::renameComparison(state, id, "");
    woby::renameComparison(state, id + 1, "Missing");
    CHECK(state.sceneEditRevision == revision);
}
