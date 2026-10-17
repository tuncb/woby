#include "ui_bulk_controls.h"
#include "scene_name_controls.h"

#include <doctest/doctest.h>
#include <imgui.h>

#include <array>

namespace {
struct SelectionShortcutFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    woby::UiState state;
    std::array<woby::UiSelectionRows, 2> rows;
    std::array<ImVec2, 2> positions;
    std::array<int, 2> deletes{};
    woby::SceneNameEdit nameEdit;
    bool disabled = false, popup = false, showFirst = true;

    SelectionShortcutFixture()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {800, 600};
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        frame(); frame();
    }
    ~SelectionShortcutFixture()
    {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({400, 400});
        ImGui::Begin("Lists");
        ImGui::BeginDisabled(disabled);
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i == 0 && !showFirst) { continue; }
            ImGui::PushID(static_cast<int>(i));
            woby::beginSelectionRows(rows[i], state);
            woby::registerSelectionRow(rows[i], i + 1);
            if (i == 0 && nameEdit.objectId) { woby::drawSceneNameInput("Name", nameEdit, 300); }
            else { ImGui::Button("Row", {300, 30}); }
            const auto low = ImGui::GetItemRectMin();
            positions[i] = {low.x + 60, low.y + 15};
            if (woby::endSelectionRows(rows[i]).remove) { ++deletes[i]; }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if (popup) { ImGui::OpenPopup("Modal"); popup = false; }
        if (ImGui::BeginPopupModal("Modal")) { ImGui::TextUnformatted("A dialog is open."); ImGui::EndPopup(); }
        ImGui::End();
        ImGui::EndFrame();
    }
    void click(ImVec2 position)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
    }
    void pressDelete()
    {
        auto& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Delete, true); frame();
        io.AddKeyEvent(ImGuiKey_Delete, false); frame();
    }
};
}

TEST_CASE("Delete belongs to one clicked list and does not follow the pointer")
{
    SelectionShortcutFixture f;
    f.click(f.positions[0]);
    ImGui::GetIO().AddMousePosEvent(f.positions[1].x, f.positions[1].y); f.frame();
    f.pressDelete();
    CHECK(f.deletes == std::array<int, 2>{1, 0});
    f.click(f.positions[1]);
    f.pressDelete();
    CHECK(f.deletes == std::array<int, 2>{1, 1});
}

TEST_CASE("Delete is blocked by disabled controls dialogs modifiers and lost list focus")
{
    SelectionShortcutFixture f;
    f.click(f.positions[0]);
    SUBCASE("disabled controls") { f.disabled = true; f.frame(); }
    SUBCASE("modal dialog") { f.popup = true; f.frame(); f.frame(); }
    SUBCASE("click outside the list") { f.click({350, 350}); }
    SUBCASE("another window has focus") { ImGui::SetWindowFocus(nullptr); f.frame(); }
    SUBCASE("Ctrl Delete") { ImGui::GetIO().AddKeyEvent(ImGuiMod_Ctrl, true); f.frame(); }
    SUBCASE("Shift Delete") { ImGui::GetIO().AddKeyEvent(ImGuiMod_Shift, true); f.frame(); }
    SUBCASE("replaced scene") { ++f.state.sceneGeneration; f.frame(); }
    SUBCASE("collapsed then reopened list") { f.showFirst = false; f.frame(); f.showFirst = true; f.frame(); }
    f.pressDelete();
    CHECK(f.deletes == std::array<int, 2>{0, 0});
}

TEST_CASE("Delete does not repeat while held")
{
    SelectionShortcutFixture f;
    f.click(f.positions[0]);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Delete, true);
    for (int frame = 0; frame < 90; ++frame) { f.frame(); }
    CHECK(f.deletes == std::array<int, 2>{1, 0});
}

TEST_CASE("Delete edits a model name without removing selected rows")
{
    SelectionShortcutFixture f;
    f.click(f.positions[0]);
    f.nameEdit.objectId = 1;
    f.nameEdit.text = f.nameEdit.initialName = f.nameEdit.originalName = "Model";
    f.nameEdit.focus = true;
    f.frame(); f.frame();
    REQUIRE(f.nameEdit.active);
    f.pressDelete();
    CHECK(f.nameEdit.text.empty());
    CHECK(f.deletes == std::array<int, 2>{0, 0});
}
