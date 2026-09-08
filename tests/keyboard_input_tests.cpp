#include "camera.h"
#include "ui_state.h"
#include "ui_layout.h"
#include "ui_icon_controls.h"
#include "ui_operations.h"
#include "ui_popup_controls.h"
#include "settings_dialog.h"
#include "scene_history.h"
#include "ui_history_controls.h"

#include <doctest/doctest.h>
#include <imgui.h>

namespace {

struct KeyboardFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    woby::UiState state;
    woby::SceneCamera& camera = state.camera;

    KeyboardFixture()
    {
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(800.0f, 600.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    ~KeyboardFixture()
    {
        ImGui::EndFrame();
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }

    void startFrame(ImGuiKey key, ImGuiKey modifier = ImGuiKey_None)
    {
        auto& io = ImGui::GetIO();
        if (modifier != ImGuiKey_None) {
            io.AddKeyEvent(modifier, true);
        }
        io.AddKeyEvent(key, true);
        ImGui::NewFrame();
    }

    void update() { woby::updateCameraFromKeyboard(state, 0.1f); }
};

} // namespace

TEST_CASE("scene history shortcuts allow property popups but respect active text and busy state")
{
    for (const bool redo : {false, true}) {
        for (const bool shift : {false, true}) {
            KeyboardFixture fixture;
            auto& io = ImGui::GetIO();
            woby::SceneHistoryCommand result = woby::SceneHistoryCommand::none;
            bool blocked = false;
            bool textInput = false;
            const auto frame = [&]() {
                ImGui::NewFrame();
                ImGui::Begin("History shortcut test");
                ImGui::OpenPopup("Properties");
                if (ImGui::BeginPopup("Properties")) {
                    ImGui::TextUnformatted("Transform geometry");
                    ImGui::EndPopup();
                }
                io.WantTextInput = textInput;
                result = woby::sceneHistoryShortcut(blocked);
                ImGui::End();
                ImGui::EndFrame();
            };
            frame();
            frame(); // Register global shortcut routing before key presses.
            const auto key = redo && !shift ? ImGuiKey_Y : ImGuiKey_Z;
            const auto press = [&]() {
                io.AddKeyEvent(ImGuiMod_Ctrl, true);
                io.AddKeyEvent(ImGuiMod_Shift, redo && shift);
                io.AddKeyEvent(key, true);
                frame();
            };
            const auto release = [&]() { io.AddKeyEvent(key, false); frame(); frame(); };
            press();
            CHECK(result == (redo ? woby::SceneHistoryCommand::redo : woby::SceneHistoryCommand::undo));
            release();
            blocked = true;
            press();
            CHECK(result == woby::SceneHistoryCommand::none);
            release();
            blocked = false;
            textInput = true;
            press();
            CHECK(result == woby::SceneHistoryCommand::none);
        }
    }
}

TEST_CASE("scene history records an actual ImGui drag as one action")
{
    KeyboardFixture fixture;
    auto& state = fixture.state;
    woby::SceneHistory history;
    const auto clean = woby::createSceneDocument(state);
    woby::resetSceneHistory(history, state);
    auto& io = ImGui::GetIO();
    ImVec2 position;
    const auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 300));
        ImGui::Begin("History drag test");
        float value = state.masterVertexPointSize;
        if (ImGui::DragFloat("Vertex size", &value, 0.1f, 1.0f, 40.0f)) {
            woby::setMasterVertexPointSize(state, value);
        }
        position = ImGui::GetItemRectMin();
        position.x += 30;
        position.y += 10;
        ImGui::End();
        woby::recordSceneHistory(history, state, woby::sceneHistoryInteraction());
        ImGui::EndFrame();
    };
    frame();
    io.AddMousePosEvent(position.x, position.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    for (int movement = 1; movement <= 10; ++movement) {
        io.AddMousePosEvent(position.x + static_cast<float>(movement) * 5, position.y);
        frame();
    }
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
    frame();
    CHECK(state.masterVertexPointSize > clean.masterVertexPointSize);
    CHECK(history.cursor == 1);
    auto prepared = woby::prepareSceneHistoryStep(history, state, clean, false);
    REQUIRE(prepared);
    CHECK(prepared->masterVertexPointSize == clean.masterVertexPointSize);
    CHECK_FALSE(prepared->isDirty);
}

TEST_CASE("Visibility fields toggle independently and respect disabled and mixed states")
{
    for (const bool initiallyVisible : {false, true}) {
        KeyboardFixture fixture;
        bool visible = initiallyVisible;
        bool otherVisible = true;
        bool disabled = false;
        bool mixed = false;
        int changes = 0;
        ImVec2 buttonPosition;
        const auto frame = [&]() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(500, 500));
            ImGui::Begin("Visibility");
            buttonPosition = ImGui::GetCursorScreenPos();
            buttonPosition.x += woby::renderModeButtonSize() * 0.5f;
            buttonPosition.y += woby::renderModeButtonSize() * 0.5f;
            ImGui::BeginDisabled(disabled);
            if (woby::drawVisibilityField("Triangle edges", visible, mixed)) {
                ++changes;
                mixed = false;
            }
            ImGui::EndDisabled();
            CHECK_FALSE(woby::drawVisibilityField("Boundary edges", otherVisible));
            ImGui::End();
            ImGui::EndFrame();
        };
        const auto click = [&]() {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(buttonPosition.x, buttonPosition.y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            frame();
        };
        frame();
        frame();
        CHECK(visible == initiallyVisible);
        CHECK(changes == 0);
        click();
        CHECK(visible == !initiallyVisible);
        CHECK(changes == 1);
        click();
        CHECK(visible == initiallyVisible);
        CHECK(changes == 2);
        disabled = true;
        click();
        CHECK(visible == initiallyVisible);
        CHECK(changes == 2);
        disabled = false;
        mixed = true;
        frame();
        CHECK(visible == initiallyVisible);
        CHECK(mixed);
        click();
        CHECK(visible);
        CHECK_FALSE(mixed);
        CHECK(changes == 3);
        CHECK(otherVisible);
    }
}

TEST_CASE("properties visibility icon sits before solid mesh and keeps hidden targets selected")
{
    for (const float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
        KeyboardFixture fixture;
        ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
        auto& state = fixture.state;
        woby::Mesh mesh;
        mesh.nodes = {{"first", 0u, 0u}, {"second", 0u, 0u}};
        state.files.push_back(woby::createUiFileState("parts.obj", mesh, 0u));
        woby::appendDefaultSceneNodesForFiles(state, 0u);
        woby::selectSceneObject(state, state.files[0].groupSettings[0].objectId);
        woby::setSelectedObjectsVisible(state, false);
        woby::selectSceneObject(state, state.files[0].groupSettings[1].objectId, true);
        const auto selection = state.selectedSceneObjects;
        REQUIRE(woby::selectedObjectVisibility(state).mixed);
        ImVec2 clickPosition;
        const auto frame = [&]() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(700, 400));
            ImGui::Begin("Properties visibility");
            const auto current = woby::selectedObjectVisibility(state);
            bool visible = current.value != 0.0f;
            if (woby::drawVisibilityIconField("selected objects", visible, current.mixed)) {
                woby::setSelectedObjectsVisible(state, visible);
            }
            const auto low = ImGui::GetItemRectMin();
            const auto high = ImGui::GetItemRectMax();
            clickPosition = ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f);
            ImGui::SameLine();
            bool solid = true;
            CHECK_FALSE(woby::drawRenderModeField("Solid mesh", woby::solidMeshIcon, solid));
            CHECK(ImGui::GetItemRectMin().x > high.x);
            CHECK(ImGui::GetItemRectMin().y == low.y);
            CHECK(ImGui::GetItemRectMax().y == high.y);
            CHECK(high.x - low.x == doctest::Approx(woby::renderModeButtonSize()));
            ImGui::End();
            ImGui::EndFrame();
        };
        frame();
        frame();
        for (const bool expected : {true, false, true}) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(clickPosition.x, clickPosition.y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            frame();
            CHECK_FALSE(woby::selectedObjectVisibility(state).mixed);
            CHECK(woby::selectedObjectVisibility(state).value == (expected ? 1.0f : 0.0f));
            CHECK(state.selectedSceneObjects == selection);
            CHECK(state.propertiesPaneVisible);
            for (const auto& part : state.files[0].groupSettings) { CHECK(part.visible == expected); }
        }
    }
}

TEST_CASE("compact render mode fields align and apply mixed toggles only to selected parts")
{
    using P = woby::UiObjectProperty;
    const std::array properties{P::solidMesh, P::triangles, P::vertices};
    const std::array icons{woby::solidMeshIcon, woby::trianglesIcon, woby::verticesIcon};
    const std::array labels{"Solid mesh", "Triangle edges", "Vertices"};
    for (const float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
        for (size_t target = 0; target < properties.size(); ++target) {
            KeyboardFixture fixture;
            ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
            auto& state = fixture.state;
            woby::Mesh mesh;
            for (int i = 0; i < 3; ++i) { mesh.nodes.push_back({"part", 0u, 0u}); }
            state.files.push_back(woby::createUiFileState("parts.obj", mesh, 0u));
            woby::appendDefaultSceneNodesForFiles(state, 0u);
            auto& parts = state.files[0].groupSettings;
            woby::selectSceneObject(state, parts[0].objectId);
            woby::setSelectedObjectProperty(state, properties[target], 0.0f);
            woby::selectSceneObject(state, parts[1].objectId);
            woby::setSelectedObjectProperty(state, properties[target], 1.0f);
            woby::selectSceneObject(state, parts[0].objectId, true);
            const auto original = woby::createSceneDocument(state);
            woby::clearSceneDirty(state);
            REQUIRE(woby::selectedObjectProperty(state, properties[target]).mixed);
            bool disabled = false;
            ImVec2 clickPosition;
            const auto frame = [&]() {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(700, 400));
                ImGui::Begin("Compact render modes");
                ImGui::BeginDisabled(disabled);
                ImVec2 firstMin;
                ImVec2 firstMax;
                for (size_t i = 0; i < properties.size(); ++i) {
                    if (i != 0u) { ImGui::SameLine(); }
                    const auto current = woby::selectedObjectProperty(state, properties[i]);
                    bool value = current.value != 0.0f;
                    if (woby::drawRenderModeField(labels[i], icons[i], value, current.mixed)) {
                        woby::setSelectedObjectProperty(state, properties[i], value ? 1.0f : 0.0f);
                    }
                    const auto low = ImGui::GetItemRectMin();
                    const auto high = ImGui::GetItemRectMax();
                    if (i == 0u) { firstMin = low; firstMax = high; }
                    CHECK(low.y == firstMin.y);
                    CHECK(high.y == firstMax.y);
                    CHECK(high.x - low.x == doctest::Approx(woby::renderModeButtonSize()));
                    if (i == target) { clickPosition = ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f); }
                }
                ImGui::EndDisabled();
                ImGui::End();
                ImGui::EndFrame();
            };
            const auto click = [&]() {
                auto& io = ImGui::GetIO();
                io.AddMousePosEvent(clickPosition.x, clickPosition.y);
                frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                frame();
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                frame();
                frame();
            };
            frame();
            frame();
            CHECK(woby::createSceneDocument(state) == original);
            CHECK_FALSE(state.isDirty);
            disabled = true;
            click();
            CHECK(woby::selectedObjectProperty(state, properties[target]).mixed);
            CHECK_FALSE(state.isDirty);
            disabled = false;
            for (const float expected : {1.0f, 0.0f, 1.0f}) {
                click();
                const auto current = woby::selectedObjectProperty(state, properties[target]);
                CHECK_FALSE(current.mixed);
                CHECK(current.value == expected);
                CHECK(state.isDirty);
            }
            // The third part and the other render modes retain their defaults.
            for (size_t part = 0; part < parts.size(); ++part) {
                woby::selectSceneObject(state, parts[part].objectId);
                for (size_t mode = 0; mode < properties.size(); ++mode) {
                    if (part < 2u && mode == target) { continue; }
                    CHECK(woby::selectedObjectProperty(state, properties[mode]).value
                        == (properties[mode] == P::solidMesh ? 1.0f : 0.0f));
                }
            }
        }
    }
}

TEST_CASE("Settings toolbar opens a dialog that closes and reopens without changing scale")
{
    KeyboardFixture fixture;
    fixture.state.uiScale = 1.5f;
    ImVec2 buttonPosition;
    ImVec2 closePosition;
    bool disabled = false;
    woby::SettingsDialogResult result;
    const auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::Begin("Toolbar");
        const bool requested = woby::drawSettingsButton(disabled);
        const auto low = ImGui::GetItemRectMin();
        buttonPosition = ImVec2(low.x + 10, low.y + 10);
        ImGui::End();
        result = woby::drawSettingsDialog(fixture.state, requested);
        if (result.open) {
            const auto* dialog = ImGui::FindWindowByName("Settings");
            closePosition = ImVec2(dialog->DC.CursorStartPos.x + 10, dialog->DC.CursorPosPrevLine.y + 10);
        }
        CHECK_FALSE(result.scaleChanged);
        woby::dismissPopupOnEscape();
        ImGui::EndFrame();
    };
    const auto click = [&](ImVec2 position) {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
    };
    frame();
    frame();
    CHECK_FALSE(result.open);
    disabled = true;
    click(buttonPosition);
    CHECK_FALSE(result.open);
    disabled = false;
    click(buttonPosition);
    REQUIRE(result.open);
    click(closePosition);
    CHECK_FALSE(result.open);
    click(buttonPosition);
    REQUIRE(result.open);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frame();
    frame();
    CHECK_FALSE(result.open);
    CHECK(fixture.state.uiScale == 1.5f);
}

TEST_CASE("Settings scale dropdown applies every option and Escape dismisses the dropdown first")
{
    for (int option = 0; option < 5; ++option) {
        KeyboardFixture fixture;
        bool requestOpen = true;
        bool changed = false;
        const auto frame = [&]() {
            ImGui::NewFrame();
            const auto result = woby::drawSettingsDialog(fixture.state, requestOpen);
            requestOpen = false;
            changed = changed || result.scaleChanged;
            woby::dismissPopupOnEscape();
            ImGui::EndFrame();
        };
        const auto click = [&](ImVec2 position) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(position.x, position.y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            frame();
        };
        frame();
        frame();
        const auto* dialog = ImGui::FindWindowByName("Settings");
        REQUIRE(dialog != nullptr);
        const auto& style = ImGui::GetStyle();
        const ImVec2 comboPosition(dialog->Pos.x + dialog->Size.x - style.WindowPadding.x
                - woby::informationIconSize() - style.ItemSpacing.x - 10,
            dialog->DC.CursorStartPos.y + ImGui::GetFontSize() + style.SeparatorTextPadding.y * 2
                + style.ItemSpacing.y + ImGui::GetFrameHeight() * 0.5f);
        click(comboPosition);
        REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 2);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        frame();
        CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
        CHECK_FALSE(changed);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        frame();
        click(comboPosition);
        REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 2);
        const auto* dropdown = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
        REQUIRE(dropdown != nullptr);
        click(ImVec2(dropdown->DC.CursorStartPos.x + 15,
            dropdown->DC.CursorStartPos.y + ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(option)
                + ImGui::GetFontSize() * 0.5f));
        CHECK(changed == (option != 0));
        CHECK(fixture.state.uiScale == 1.0f + static_cast<float>(option) * 0.25f);
        CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    }
}

TEST_CASE("Escape dismisses color pickers and dropdowns without changing their values")
{
    for (const bool colorPicker : {true, false}) {
        KeyboardFixture fixture;
        float color[3]{0.2f, 0.4f, 0.6f};
        int choice = 0;
        ImVec2 clickPosition;
        const auto frame = [&]() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(500, 500));
            ImGui::Begin("Properties");
            clickPosition = ImGui::GetCursorScreenPos();
            clickPosition.x += 8.0f;
            clickPosition.y += 8.0f;
            if (colorPicker) {
                ImGui::ColorEdit3("Color picker", color, ImGuiColorEditFlags_NoInputs);
            } else {
                ImGui::Combo("Mode", &choice, "First\0Second\0");
            }
            ImGui::End();
            woby::dismissPopupOnEscape();
            ImGui::EndFrame();
        };
        frame();
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(clickPosition.x, clickPosition.y);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        frame();
        REQUIRE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame();
        CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
        CHECK(color[0] == 0.2f);
        CHECK(color[1] == 0.4f);
        CHECK(color[2] == 0.6f);
        CHECK(choice == 0);
        CHECK((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) == 0);
    }
}

TEST_CASE("information icons show full hints on hover without click behavior")
{
    KeyboardFixture fixture;
    ImVec2 icons[2]{};
    const auto tooltip = []() -> ImGuiWindow* {
        for (auto* window : ImGui::GetCurrentContext()->Windows) {
            if (window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_Tooltip) != 0) { return window; }
        }
        return nullptr;
    };
    const auto frame = [&]() {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 500));
        ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoMove);
        for (int index = 0; index < 2; ++index) {
            ImGui::PushID(index);
            icons[index] = ImGui::GetCursorScreenPos();
            icons[index].x += std::max(0.0f, ImGui::GetContentRegionAvail().x - woby::informationIconSize()) + 8.0f;
            icons[index].y += 8.0f;
            woby::drawInformationIcon("info", index == 0 ? "Settings" : "Appearance",
                "Parent and part opacity multiply. Values range from 0-100%.\n\nEnter applies numeric entry.");
            ImGui::PopID();
        }
        ImGui::End();
        ImGui::EndFrame();
    };
    auto& io = ImGui::GetIO();
    frame();
    CHECK(tooltip() == nullptr);
    for (int index = 0; index < 2; ++index) {
        io.AddMousePosEvent(icons[index].x, icons[index].y);
        frame();
        frame();
        REQUIRE(tooltip() != nullptr);
        CHECK(tooltip()->ContentSize.y > ImGui::GetTextLineHeight() * 2.0f);
        CHECK(tooltip()->ScrollMax.y == 0.0f);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        frame();
        CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
        CHECK(ImGui::GetCurrentContext()->ActiveId == 0);
        io.AddMousePosEvent(10, 490);
        frame();
        CHECK(tooltip() == nullptr);
        CHECK_FALSE(fixture.state.isDirty);
    }
}

TEST_CASE("object identity rows stay single-line and reveal complete wrapped names on hover")
{
    const std::string longName = std::string(95, 'W') + "\xc3\xa7\xe6\xb5\x8b##part-end";
    const std::string fileName = std::string(85, 'F') + "##file-end.obj";
    const std::string path = "C:/models/" + fileName;
    for (const float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
        for (const float width : {150.0f, 500.0f}) {
            KeyboardFixture fixture;
            ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
            ImVec2 hoverPosition;
            std::string logged;
            const auto frame = [&]() {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(width, 500));
                ImGui::Begin("Object identity", nullptr, ImGuiWindowFlags_NoMove);
                ImGui::LogToBuffer();
                const auto low = ImGui::GetCursorScreenPos();
                const float available = ImGui::GetContentRegionAvail().x;
                woby::drawObjectIdentityRow("Part", longName.c_str(), fileName.c_str(), path.c_str());
                CHECK(ImGui::GetItemRectSize().x == doctest::Approx(available));
                CHECK(ImGui::GetItemRectSize().y == doctest::Approx(ImGui::GetTextLineHeight()));
                CHECK(ImGui::GetCursorScreenPos().y - low.y
                    == doctest::Approx(ImGui::GetTextLineHeightWithSpacing()));
                hoverPosition = ImVec2(low.x + available * 0.5f, low.y + ImGui::GetTextLineHeight() * 0.5f);
                logged = ImGui::GetCurrentContext()->LogBuffer.c_str();
                ImGui::LogFinish();
                // Other selected objects also occupy exactly one row, even if
                // importer names contain line breaks or tabs.
                const float nextY = ImGui::GetCursorScreenPos().y;
                woby::drawObjectIdentityRow("Part", "surface\nsecond\tline", "repaired.obj");
                CHECK(ImGui::GetCursorScreenPos().y - nextY
                    == doctest::Approx(ImGui::GetTextLineHeightWithSpacing()));
                woby::drawObjectIdentityRow("Folder", "Models");
                CHECK(ImGui::GetItemRectSize().y == doctest::Approx(ImGui::GetTextLineHeight()));
                ImGui::End();
                ImGui::EndFrame();
            };
            frame();
            frame();
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(hoverPosition.x, hoverPosition.y);
            frame();
            frame();
            ImGuiWindow* hint = nullptr;
            for (auto* window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_Tooltip) != 0) { hint = window; }
            }
            REQUIRE(hint != nullptr);
            CHECK(logged.find(longName) != std::string::npos);
            CHECK(logged.find(fileName) != std::string::npos);
            CHECK(logged.find(path) != std::string::npos);
            CHECK(hint->Size.x <= io.DisplaySize.x);
            CHECK(hint->Size.y <= io.DisplaySize.y);
            CHECK(hint->ScrollMax.x == 0.0f);
            CHECK(hint->ScrollMax.y == 0.0f);
            CHECK(hint->ContentSize.y > ImGui::GetTextLineHeight() * 3.0f);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            CHECK_FALSE(ImGui::IsAnyItemActive());
            CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
        }
    }
}

TEST_CASE("history toolbar buttons share the Settings row and respect history and busy states")
{
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        KeyboardFixture fixture;
        ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
        woby::SceneHistory history;
        const auto clean = woby::createSceneDocument(fixture.state);
        woby::resetSceneHistory(history, fixture.state);
        ImVec2 undoPosition, redoPosition;
        bool blocked = false;
        int actions = 0;
        const auto frame = [&]() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(woby::uiSize(380), 300));
            ImGui::Begin("History toolbar");
            ImGui::TextUnformatted("Scene controls");
            const auto titleRight = ImGui::GetItemRectMax().x;
            ImGui::SameLine();
            const auto& style = ImGui::GetStyle();
            const float size = woby::renderModeButtonSize();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - style.WindowPadding.x
                - 3 * size - 2 * style.ItemSpacing.x);
            const auto undoLow = ImGui::GetCursorScreenPos();
            CHECK(undoLow.x > titleRight);
            undoPosition = ImVec2(undoLow.x + size * 0.5f, undoLow.y + size * 0.5f);
            const auto command = woby::drawSceneHistoryToolbar(
                woby::canUndoScene(history), woby::canRedoScene(history), blocked);
            const auto redoLow = ImGui::GetItemRectMin();
            const auto redoHigh = ImGui::GetItemRectMax();
            redoPosition = ImVec2(redoLow.x + size * 0.5f, redoLow.y + size * 0.5f);
            CHECK(redoLow.y == undoLow.y);
            CHECK(redoLow.x >= undoLow.x + size);
            ImGui::SameLine();
            CHECK_FALSE(woby::drawSettingsButton(blocked));
            CHECK(ImGui::GetItemRectMin().y == undoLow.y);
            CHECK(ImGui::GetItemRectMin().x >= redoHigh.x);
            CHECK(ImGui::GetItemRectMax().x <= ImGui::GetWindowPos().x + ImGui::GetWindowWidth());
            ImGui::End();
            if (command != woby::SceneHistoryCommand::none) {
                const bool redo = command == woby::SceneHistoryCommand::redo;
                auto prepared = woby::prepareSceneHistoryStep(history, fixture.state, clean, redo);
                REQUIRE(prepared);
                woby::commitSceneHistoryStep(history, fixture.state, std::move(*prepared), redo);
                ++actions;
            }
            ImGui::EndFrame();
        };
        const auto click = [&](ImVec2 position) {
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(position.x, position.y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            frame();
        };
        frame();
        frame();
        click(undoPosition);
        click(redoPosition);
        CHECK(actions == 0);
        woby::setMasterVertexPointSize(fixture.state, 12);
        woby::recordSceneHistory(history, fixture.state);
        blocked = true;
        click(undoPosition);
        CHECK(actions == 0);
        blocked = false;
        click(undoPosition);
        CHECK(actions == 1);
        CHECK(fixture.state.masterVertexPointSize == clean.masterVertexPointSize);
        blocked = true;
        click(redoPosition);
        CHECK(actions == 1);
        blocked = false;
        click(redoPosition);
        CHECK(actions == 2);
        CHECK(fixture.state.masterVertexPointSize == 12);
    }
}

TEST_CASE("information icons stay compact and right aligned beside stretch controls at every UI scale")
{
    for (const float scale : {1.0f, 1.25f, 1.5f, 1.75f, 2.0f}) {
        for (const float width : {240.0f, 500.0f}) {
            KeyboardFixture fixture;
            ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(width, 500));
            ImGui::Begin("Properties");
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            const float size = woby::informationIconSize();
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            ImGui::SetNextItemWidth(-size - spacing);
            char name[32] = "Comparison";
            ImGui::InputText("##name", name, sizeof(name));
            const float fieldRight = ImGui::GetItemRectMax().x;
            ImGui::SameLine();
            woby::drawInformationIcon("info", "Comparison", "Combined surfaces at scene positions.");
            CHECK(ImGui::GetItemRectMax().x == doctest::Approx(right));
            CHECK(ImGui::GetItemRectMin().x >= fieldRight + spacing - 1.0f);
            CHECK(ImGui::GetItemRectSize().x == doctest::Approx(20.0f * scale));
            CHECK(ImGui::GetItemRectSize().y == doctest::Approx(20.0f * scale));
            CHECK(size < woby::renderModeButtonSize());
            ImGui::End();
        }
    }
}

TEST_CASE("section header hints share one row and remain available when collapsed without toggling the section")
{
    for (const float scale : {1.0f, 2.0f}) {
        KeyboardFixture fixture;
        ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
        ImVec2 header;
        ImVec2 icon;
        bool open = false;
        const auto tooltipVisible = []() {
            for (const auto* window : ImGui::GetCurrentContext()->Windows) {
                if (window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_Tooltip) != 0) { return true; }
            }
            return false;
        };
        const auto frame = [&]() {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(300, 500));
            ImGui::Begin("Properties", nullptr, ImGuiWindowFlags_NoMove);
            const auto start = ImGui::GetCursorScreenPos();
            const float rowHeight = std::max(ImGui::GetFrameHeight(), woby::informationIconSize());
            header = ImVec2(start.x + 30.0f, start.y + rowHeight * 0.5f);
            icon = ImVec2(start.x + ImGui::GetContentRegionAvail().x - woby::informationIconSize() * 0.5f,
                start.y + rowHeight * 0.5f);
            open = woby::drawInformationHeader("Geometry", "Geometry", "Select one file or part to inspect its mesh statistics.");
            CHECK(ImGui::GetCursorScreenPos().y - start.y <= rowHeight + ImGui::GetStyle().ItemSpacing.y * 2.0f);
            ImGui::End();
            ImGui::EndFrame();
        };
        auto& io = ImGui::GetIO();
        frame();
        frame();
        REQUIRE(open);
        for (const bool expanded : {true, false}) {
            io.AddMousePosEvent(icon.x, icon.y);
            frame();
            frame();
            CHECK(tooltipVisible());
            CHECK(open == expanded);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            CHECK(open == expanded);
            CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
            io.AddMousePosEvent(header.x, header.y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            CHECK(open != expanded);
        }
    }
}

TEST_CASE("hover hints inside export popups show complete wrapped text without opening another popup")
{
    KeyboardFixture fixture;
    auto& io = ImGui::GetIO();
    ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), 2.0f);
    const char* explanation =
        "Choose a width from 960 to 7680 pixels and a height from 720 to 4320 pixels.\n\n"
        "Visible results only exports comparison results. Otherwise the image includes the scene, helpers and visible results. "
        "Uses the current camera.\n\nExport waits for complete visible results.";
    ImVec2 icon;
    const auto frame = [&](bool open) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 500));
        ImGui::Begin("Scene");
        if (open) { ImGui::OpenPopup("Export PNG"); }
        if (ImGui::BeginPopup("Export PNG")) {
            ImGui::TextUnformatted("Export PNG");
            ImGui::SameLine();
            icon = ImGui::GetCursorScreenPos();
            icon.x += std::max(0.0f, ImGui::GetContentRegionAvail().x - woby::informationIconSize()) + 8.0f;
            icon.y += 8.0f;
            woby::drawInformationIcon("export_info", "Export PNG", explanation);
            ImGui::EndPopup();
        }
        ImGui::End();
        woby::dismissPopupOnEscape();
        ImGui::EndFrame();
    };
    frame(true);
    frame(false);
    io.AddMousePosEvent(icon.x, icon.y);
    frame(false);
    frame(false);
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    ImGuiWindow* hint = nullptr;
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
        if (window->Active && !window->Hidden && (window->Flags & ImGuiWindowFlags_Tooltip) != 0) { hint = window; }
    }
    REQUIRE(hint != nullptr);
    CHECK(hint->Size.x <= io.DisplaySize.x);
    CHECK(hint->Size.y <= io.DisplaySize.y);
    CHECK(hint->ScrollMax.y == 0.0f);
    CHECK(hint->ScrollMax.x == 0.0f);
    CHECK(hint->ContentSize.y > ImGui::GetTextLineHeight() * 3.0f);
    io.AddKeyEvent(ImGuiKey_Escape, true);
    frame(false);
    CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
}

TEST_CASE("Escape closes only the innermost popup and leaves modal cancellation to its owner")
{
    KeyboardFixture fixture;
    bool modal = false;
    SUBCASE("context menu with submenu") {}
    SUBCASE("dropdown inside a modal") { modal = true; }
    const auto frame = [&](bool open) {
        ImGui::NewFrame();
        ImGui::Begin("Scene");
        if (open) { ImGui::OpenPopup("Parent"); }
        const bool parent = modal ? ImGui::BeginPopupModal("Parent") : ImGui::BeginPopup("Parent");
        if (parent) {
            ImGui::TextUnformatted("Parent controls");
            if (open) { ImGui::OpenPopup("Child"); }
            if (ImGui::BeginPopup("Child")) {
                ImGui::TextUnformatted("Child controls");
                ImGui::EndPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::End();
        woby::dismissPopupOnEscape();
        ImGui::EndFrame();
    };
    frame(true);
    REQUIRE(ImGui::GetCurrentContext()->OpenPopupStack.Size == 2);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frame(false);
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    // A held Escape must not cascade to the parent.
    frame(false);
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frame(false);
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == (modal ? 1 : 0));
}

TEST_CASE("Escape cancels text editing before dismissing its popup")
{
    KeyboardFixture fixture;
    char value[32] = "Original";
    const auto frame = [&](bool open) {
        ImGui::NewFrame();
        ImGui::Begin("Scene");
        if (open) { ImGui::OpenPopup("Export PNG"); }
        if (ImGui::BeginPopup("Export PNG")) {
            if (open) { ImGui::SetKeyboardFocusHere(); }
            ImGui::InputText("Name", value, sizeof(value));
            ImGui::EndPopup();
        }
        ImGui::End();
        woby::dismissPopupOnEscape();
        ImGui::EndFrame();
    };
    frame(true);
    frame(false);
    REQUIRE(ImGui::IsAnyItemActive());
    ImGui::GetIO().AddInputCharactersUTF8("Changed");
    frame(false);
    REQUIRE(std::string(value) != "Original");
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frame(false);
    CHECK(std::string(value) == "Original");
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
    frame(false);
    ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
    frame(false);
    CHECK_FALSE(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup));
}

TEST_CASE("camera keyboard controls move orbit and zoom without command modifiers")
{
    for (const auto key : {ImGuiKey_S, ImGuiKey_LeftArrow, ImGuiKey_Equal}) {
        KeyboardFixture fixture;
        const auto before = fixture.camera;
        fixture.startFrame(key);
        fixture.update();
        if (key == ImGuiKey_S) {
            CHECK(fixture.camera.target != before.target);
        } else if (key == ImGuiKey_LeftArrow) {
            CHECK(fixture.camera.yawRadians != before.yawRadians);
        } else {
            CHECK(fixture.camera.distance != before.distance);
        }
    }
}

TEST_CASE("command modifiers suppress all camera keyboard controls")
{
    for (const auto modifier : {ImGuiMod_Ctrl, ImGuiMod_Alt, ImGuiMod_Super}) {
        for (const auto key : {ImGuiKey_S, ImGuiKey_LeftArrow, ImGuiKey_Equal}) {
            KeyboardFixture fixture;
            const auto before = fixture.camera;
            fixture.startFrame(key, modifier);
            fixture.update();
            CHECK(fixture.camera.target == before.target);
            CHECK(fixture.camera.yawRadians == before.yawRadians);
            CHECK(fixture.camera.distance == before.distance);
        }
    }
}

TEST_CASE("keyboard capture text editing and popups suppress camera movement")
{
    KeyboardFixture fixture;
    const auto before = fixture.camera;
    fixture.startFrame(ImGuiKey_S);
    SUBCASE("keyboard capture") { ImGui::GetIO().WantCaptureKeyboard = true; }
    SUBCASE("text input") { ImGui::GetIO().WantTextInput = true; }
    SUBCASE("popup") { ImGui::OpenPopup("Context menu"); }
    SUBCASE("active field") {
        ImGui::Begin("Properties");
        ImGui::SetKeyboardFocusHere();
        char value[32] = "name";
        ImGui::InputText("Name", value, sizeof(value));
        ImGui::End();
        ImGui::EndFrame();
        ImGui::NewFrame();
        ImGui::Begin("Properties");
        ImGui::InputText("Name", value, sizeof(value));
        REQUIRE(ImGui::IsAnyItemActive());
        // Test the active-item guard independently of previous-frame capture flags.
        ImGui::GetIO().WantCaptureKeyboard = false;
        ImGui::GetIO().WantTextInput = false;
        ImGui::End();
    }
    fixture.update();
    CHECK(fixture.camera.target == before.target);
}

TEST_CASE("shift retains accelerated camera movement")
{
    KeyboardFixture fixture;
    fixture.startFrame(ImGuiKey_S);
    fixture.update();
    const auto normal = fixture.camera.target;
    fixture.camera = {};
    ImGui::GetIO().KeyShift = true;
    fixture.update();
    for (size_t axis = 0; axis < normal.size(); ++axis) {
        CHECK(fixture.camera.target[axis] == doctest::Approx(normal[axis] * 4.0f));
    }
}

TEST_CASE("scene name rows align with visibility and remove controls at every UI scale")
{
    for (const float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
        KeyboardFixture fixture;
        ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
        ImGui::NewFrame();
        ImGui::SetNextWindowSize(ImVec2(700.0f, 400.0f));
        ImGui::Begin("Scene rows");
        woby::drawVisibilityButton("visible", true, "comparison");
        const auto eyeMin = ImGui::GetItemRectMin();
        const auto eyeMax = ImGui::GetItemRectMax();
        ImGui::SameLine();
        woby::drawSceneItemButton("A comparison with a long name###name", 200.0f, true);
        const auto nameMin = ImGui::GetItemRectMin();
        const auto nameMax = ImGui::GetItemRectMax();
        ImGui::SameLine();
        woby::drawRemoveButton("remove", "Remove comparison");
        CHECK(nameMin.y == eyeMin.y);
        CHECK(nameMax.y == eyeMax.y);
        CHECK(nameMin.y == ImGui::GetItemRectMin().y);
        CHECK(nameMax.y == ImGui::GetItemRectMax().y);
        CHECK(nameMax.x - nameMin.x == doctest::Approx(200.0f));
        CHECK(nameMax.x < ImGui::GetItemRectMin().x);
        ImGui::End();
    }
}

TEST_CASE("reset icons fit an input row and activate only their own enabled control")
{
    for (const float scale : {0.75f, 1.0f, 1.5f, 2.0f}) {
        for (const bool disabled : {false, true}) {
            KeyboardFixture fixture;
            ImGui::GetStyle() = woby::scaledUiStyle(ImGui::GetStyle(), scale);
            ImVec2 clickPosition;
            int firstResets = 0;
            int secondResets = 0;
            const auto frame = [&]() {
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(700, 400));
                ImGui::Begin("Reset controls");
                float value = 2.0f;
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputFloat("##value", &value);
                const auto inputMin = ImGui::GetItemRectMin();
                const auto inputMax = ImGui::GetItemRectMax();
                ImGui::SameLine();
                ImGui::BeginDisabled(disabled);
                ImGui::PushID("scale");
                if (woby::drawResetIconButton("reset", "Reset scale")) { ++firstResets; }
                const auto low = ImGui::GetItemRectMin();
                const auto high = ImGui::GetItemRectMax();
                CHECK(low.y == inputMin.y);
                CHECK(high.y == inputMax.y);
                CHECK(low.x > inputMax.x);
                CHECK(high.x - low.x == doctest::Approx(high.y - low.y));
                clickPosition = ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f);
                ImGui::PopID();
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::PushID("appearance");
                if (woby::drawResetIconButton("reset", "Reset appearance")) { ++secondResets; }
                ImGui::PopID();
                ImGui::End();
                ImGui::EndFrame();
            };
            frame();
            auto& io = ImGui::GetIO();
            io.AddMousePosEvent(clickPosition.x, clickPosition.y);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            CHECK(firstResets == 0);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
            frame();
            CHECK(firstResets == (disabled ? 0 : 1));
            CHECK(secondResets == 0);
        }
    }
}

TEST_CASE("selected tree row outlines end inside the reserved delete column clip")
{
    KeyboardFixture fixture;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(600.0f, 300.0f));
    ImGui::Begin("Objects");
    auto* draw = ImGui::GetWindowDrawList();
    const auto clipLow = draw->GetClipRectMin();
    const ImVec2 clipHigh(ImGui::GetCursorScreenPos().x + 300.0f, draw->GetClipRectMax().y);
    ImGui::PushClipRect(clipLow, clipHigh, true);
    const bool open = ImGui::TreeNodeEx("selected.obj", ImGuiTreeNodeFlags_SpanAvailWidth);
    REQUIRE(ImGui::GetItemRectMax().x > clipHigh.x);
    const int firstVertex = draw->VtxBuffer.Size;
    woby::drawSceneItemOutline();
    REQUIRE(draw->VtxBuffer.Size > firstVertex);
    float rightmost = 0.0f;
    for (int index = firstVertex; index < draw->VtxBuffer.Size; ++index) {
        const auto x = draw->VtxBuffer[index].pos.x;
        // Rounded-corner antialiasing can overshoot by floating-point noise.
        CHECK(x <= clipHigh.x + 0.001f);
        rightmost = std::max(rightmost, x);
    }
    CHECK(rightmost > clipHigh.x - 2.0f);
    if (open) { ImGui::TreePop(); }
    ImGui::PopClipRect();
    ImGui::End();
}

TEST_CASE("fractional UI scales keep separators drawable across scale changes")
{
    KeyboardFixture fixture;
    const ImGuiStyle baseStyle = ImGui::GetStyle();
    // Reproduce the upstream rounding that caused SeparatorEx to assert.
    auto truncated = baseStyle;
    truncated.ScaleAllSizes(0.75f);
    CHECK(truncated.SeparatorSize == 0.0f);

    for (const float scale : {0.5f, 0.75f, 0.8f, 1.0f, 1.25f, 2.0f, 0.5f, 1.0f}) {
        INFO("logical UI scale: ", scale);
        auto& style = ImGui::GetStyle();
        style = woby::scaledUiStyle(baseStyle, scale);
        REQUIRE(style.SeparatorSize >= 1.0f);
        CHECK(style.FontScaleMain == scale);
        CHECK(style.WindowPadding.x == std::floor(baseStyle.WindowPadding.x * scale));
        if (scale == 1.0f) {
            CHECK(style.SeparatorSize == baseStyle.SeparatorSize);
            CHECK(style.FramePadding.y == baseStyle.FramePadding.y);
        }
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(500, 400));
        const bool visible = ImGui::Begin("Scaled separator regression");
        CHECK(visible);
        if (visible) {
            ImGui::TextUnformatted("Comparison");
            ImGui::Separator();
            ImGui::TextUnformatted("Properties");
        }
        ImGui::End();
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData() != nullptr);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }
}
