#include "scene_pane_layout.h"
#include "annotation_ui.h"
#include "comparison_view.h"
#include "ui_views.h"
#include "ui_icon_controls.h"
#include "ui_layout.h"
#include "ui_operations.h"

#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <numeric>

namespace {
using namespace woby;

struct ScenePaneFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    UiState state;
    ScenePaneLayout layout;
    ViewNameEdit viewEdit;
    ComparisonNameEdit analysisEdit;
    AnnotationNameEdit annotationEdit;
    std::array<ImVec2, scenePaneSectionCount> headers{}, dividers{};
    std::array<ImGuiWindow*, scenePaneSectionCount> bodies{};
    ImGuiWindow* pane = nullptr;
    float windowHeight = 900;

    explicit ScenePaneFixture(float scale = 1)
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = {1200, 1600};
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::GetStyle() = scaledUiStyle(ImGui::GetStyle(), scale);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        for (int i = 0; i < 24; ++i) { createView(state); }
        createComparison(state);
        frame(); frame(); frame();
    }
    ~ScenePaneFixture()
    {
        ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous);
    }
    void record(size_t index, const char* tableName)
    {
        if (const auto* table = ImGui::TableFindByID(ImGui::GetID(tableName))) {
            headers[index] = {table->OuterRect.Min.x + 100, table->OuterRect.GetCenter().y};
        }
        ImGui::PushID(static_cast<int>(index));
        const auto id = ImGui::GetID("section_rows");
        ImGui::PopID();
        bodies[index] = nullptr;
        for (auto* window : context->Windows) {
            if (window->ParentWindow != pane || window->ChildId != id || !window->Active) { continue; }
            bodies[index] = window;
            dividers[index] = {window->Pos.x + 150, window->Pos.y + window->Size.y
                + ImGui::GetStyle().ItemSpacing.y + layout.dividerHeight * 0.5f};
        }
    }
    void frame()
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({20, 20});
        ImGui::SetNextWindowSize({500, windowHeight});
        ImGui::Begin("Scene pane", nullptr, ImGuiWindowFlags_NoTitleBar);
        pane = ImGui::GetCurrentWindow();
        beginScenePaneLayout(layout);
        bool modelsOpen = layout.open[0];
        if (ImGui::BeginTable("models_header", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("visibility", ImGuiTableColumnFlags_WidthFixed, renderModeButtonSize());
            ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::Button("Eye", {renderModeButtonSize(), renderModeButtonSize()});
            ImGui::TableNextColumn();
            modelsOpen = ImGui::CollapsingHeader("Models", ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::EndTable();
        }
        if (beginScenePaneSection(layout, ScenePaneSection::models, modelsOpen)) {
            for (int i = 0; i < 80; ++i) { ImGui::Text("Model %d", i); }
        }
        endScenePaneSection(layout, ScenePaneSection::models);
        record(0, "models_header");
        drawComparisonObjects(state, analysisEdit, {}, &layout);
        record(1, "analysis_header");
        drawAnnotationObjects(state, annotationEdit, &layout);
        record(2, "annotations_header");
        drawViews(state, viewEdit, layout);
        ImGui::PushID("views");
        record(3, "header");
        ImGui::PopID();
        ImGui::End();
        ImGui::EndFrame();
    }
    void click(ImVec2 position)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
        frame(); frame();
    }
    void drag(ImVec2 position, float delta)
    {
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); frame();
        io.AddMousePosEvent(position.x, position.y + delta); frame();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); frame();
        frame(); frame();
    }
};
} // namespace

TEST_CASE("scene pane sections fill the available height and keep minimum row sizes")
{
    const ScenePaneHeights weights{1, 2, 1, 4};
    CHECK(scenePaneSectionHeights({false, false, false, true}, weights, 800, 40)
        == ScenePaneHeights{0, 0, 0, 800});
    CHECK(scenePaneSectionHeights({false, false, false, false}, weights, 800, 40)
        == ScenePaneHeights{});
    const auto heights = scenePaneSectionHeights({true, true, true, true}, weights, 800, 40);
    CHECK(heights == ScenePaneHeights{120, 200, 120, 360});
    CHECK(scenePaneSectionHeights({true, true, true, true}, weights, 70, 40)
        == ScenePaneHeights{40, 40, 40, 40});
    CHECK(scenePaneSectionHeights({true, false, false, true}, {}, 300, 40)
        == ScenePaneHeights{150, 0, 0, 150});
}

TEST_CASE("scene pane resizing transfers space between adjacent open sections")
{
    ScenePaneLayout layout;
    layout.minimumHeight = 40;
    layout.heights = scenePaneSectionHeights(layout.open, layout.weights, 600, 40);
    resizeScenePaneSections(layout, ScenePaneSection::analyses, 80);
    CHECK(layout.heights == ScenePaneHeights{200, 280, 0, 120});
    CHECK(scenePaneSectionHeights(layout.open, layout.weights, 600, 40) == layout.heights);
    const auto weights = layout.weights;
    layout.open[1] = false;
    const auto collapsed = scenePaneSectionHeights(layout.open, layout.weights, 600, 40);
    CHECK(collapsed[0] + collapsed[3] == doctest::Approx(600));
    layout.open[1] = true;
    CHECK(scenePaneSectionHeights(layout.open, layout.weights, 600, 40) == layout.heights);
    CHECK(layout.weights == weights);
    resizeScenePaneSections(layout, ScenePaneSection::analyses, 10000);
    CHECK(layout.heights[3] == 40);
    resizeScenePaneSections(layout, ScenePaneSection::analyses, -10000);
    CHECK(layout.heights[1] == 40);
    const auto resized = scenePaneSectionHeights(layout.open, layout.weights, 1200, 40);
    CHECK(std::accumulate(resized.begin(), resized.end(), 0.0f) == doctest::Approx(1200));
    CHECK(resized[1] >= 40);
}

TEST_CASE("scene pane Views fills the pane after other sections collapse")
{
    for (const float scale : {1.0f, 1.25f, 2.0f}) {
        CAPTURE(scale);
        ScenePaneFixture f(scale);
        f.click(f.headers[0]);
        f.click(f.headers[1]);
        REQUIRE(f.layout.open == ScenePaneOpen{false, false, false, true});
        REQUIRE(f.bodies[3]);
        CHECK(f.bodies[3]->Size.y > 5 * renderModeButtonSize());
        CHECK(f.bodies[3]->Pos.y + f.bodies[3]->Size.y == doctest::Approx(f.pane->WorkRect.Max.y).epsilon(0.003));
        CHECK(f.pane->ScrollMax.y == 0);
        const float previous = f.bodies[3]->Size.y;
        f.windowHeight += 200;
        f.frame(); f.frame();
        CHECK(f.bodies[3]->Size.y == doctest::Approx(previous + 200));
    }
}

TEST_CASE("scene pane resizing still works after sections with all spare space close")
{
    ScenePaneLayout layout;
    layout.open = {true, false, false, true};
    layout.weights = {0, 4, 0, 0};
    layout.minimumHeight = 40;
    layout.heights = scenePaneSectionHeights(layout.open, layout.weights, 400, 40);
    resizeScenePaneSections(layout, ScenePaneSection::models, 50);
    CHECK(scenePaneSectionHeights(layout.open, layout.weights, 400, 40)
        == ScenePaneHeights{250, 0, 0, 150});
    CHECK(layout.weights[1] == 4);
}

TEST_CASE("scene pane dividers resize each section without moving the bottom edge")
{
    ScenePaneFixture f;
    f.click(f.headers[2]);
    REQUIRE(f.layout.open == ScenePaneOpen{true, true, true, true});
    for (size_t i = 0; i < 3; ++i) {
        CAPTURE(i);
        REQUIRE(f.bodies[i]);
        REQUIRE(f.bodies[i + 1]);
        const float before = f.bodies[i]->Size.y;
        const float after = f.bodies[i + 1]->Size.y;
        f.drag(f.dividers[i], 35);
        CHECK(f.bodies[i]->Size.y == doctest::Approx(before + 35).epsilon(0.01));
        CHECK(f.bodies[i + 1]->Size.y == doctest::Approx(after - 35).epsilon(0.01));
        CHECK(f.pane->ScrollMax.y == 0);
    }
    const auto weights = f.layout.weights;
    f.click(f.headers[0]);
    f.click(f.headers[0]);
    CHECK(f.layout.weights == weights);
}

TEST_CASE("scene pane divider double click restores automatic sizes")
{
    ScenePaneFixture f;
    f.drag(f.dividers[0], 50);
    REQUIRE(f.layout.weights[0] != f.layout.weights[1]);
    // Keep the first click outside the preceding drag's double-click interval.
    for (int i = 0; i < 30; ++i) { f.frame(); }
    const auto divider = f.dividers[0];
    f.click(divider);
    f.click(divider);
    CHECK(f.layout.weights == ScenePaneHeights{1, 1, 1, 1});
    CHECK(f.bodies[0]->Size.y == f.bodies[1]->Size.y);
}

TEST_CASE("scene pane keeps open sections stable when the window is too short")
{
    ScenePaneFixture f;
    f.click(f.headers[2]);
    f.windowHeight = 200;
    for (int i = 0; i < 8; ++i) { f.frame(); }
    CHECK(f.layout.open == ScenePaneOpen{true, true, true, true});
    CHECK(f.pane->ScrollMax.y > 0);
    for (const float height : f.layout.heights) { CHECK(height >= f.layout.minimumHeight); }
    f.windowHeight = 900;
    f.frame(); f.frame(); f.frame();
    CHECK(f.layout.open == ScenePaneOpen{true, true, true, true});
    CHECK(f.pane->ScrollMax.y == 0);
}

TEST_CASE("scene pane lists scroll independently while headers remain visible")
{
    ScenePaneFixture f;
    REQUIRE(f.bodies[0]);
    REQUIRE(f.bodies[3]);
    const auto headers = f.headers;
    auto* models = f.bodies[0];
    const float viewsScroll = f.bodies[3]->Scroll.y;
    REQUIRE(models->ScrollMax.y > 0);
    ImGui::GetIO().AddMousePosEvent(models->Pos.x + 100, models->Pos.y + 30);
    f.frame();
    ImGui::GetIO().AddMouseWheelEvent(0, -4);
    f.frame(); f.frame();
    CHECK(models->Scroll.y > 0);
    CHECK(f.bodies[3]->Scroll.y == viewsScroll);
    CHECK(f.pane->Scroll.y == 0);
    for (size_t i = 0; i < 4; ++i) { CHECK(f.headers[i].y == headers[i].y); }
}
