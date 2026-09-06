#include "camera.h"
#include "ui_state.h"
#include "ui_layout.h"
#include "ui_icon_controls.h"

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
