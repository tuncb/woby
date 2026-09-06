#include "camera.h"
#include "ui_state.h"

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
