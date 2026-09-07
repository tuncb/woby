#include "camera.h"
#include "ui_state.h"
#include "ui_layout.h"
#include "ui_icon_controls.h"
#include "ui_popup_controls.h"

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
