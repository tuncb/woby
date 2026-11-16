#include "settings_dialog.h"
#include "ui_state.h"
#include <doctest/doctest.h>
#include <imgui_internal.h>
#include <future>
#include <stdexcept>
#include <utility>

namespace {
struct SettingsUiFixture {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    SettingsUiFixture()
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1000, 800);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~SettingsUiFixture() { ImGui::DestroyContext(context); ImGui::SetCurrentContext(previous); }
};
}

TEST_CASE("Windows settings return association commands without changing the scene")
{
    SettingsUiFixture ui;
    woby::UiState scene;
    woby::FileAssociationsState associations;
    associations.supported = true;
    associations.checked = true;
    int row = 0;
    const char* button = "Woby scenes (.woby)";
    bool rowControl = true;
    auto action = woby::FileAssociationAction::registerCopy;
    SUBCASE("register scene") {}
    SUBCASE("register OBJ") { row = 1; button = "OBJ models (.obj)"; }
    SUBCASE("remove scene") {
        associations.types[0].registeredCurrentCopy = true;
        action = woby::FileAssociationAction::removeRegistration;
    }
    SUBCASE("remove OBJ") {
        row = 1;
        button = "OBJ models (.obj)";
        associations.types[1].registeredCurrentCopy = true;
        action = woby::FileAssociationAction::removeRegistration;
    }
    SUBCASE("open defaults") {
        associations.types[0].registeredCurrentCopy = true;
        button = "Windows defaults...";
        rowControl = false;
        action = woby::FileAssociationAction::openDefaults;
    }
    SUBCASE("OBJ registration enables Windows defaults") {
        associations.types[1].registeredCurrentCopy = true;
        button = "Windows defaults...";
        rowControl = false;
        action = woby::FileAssociationAction::openDefaults;
    }
    SUBCASE("busy blocks actions") {
        associations.busy = true;
        action = woby::FileAssociationAction::none;
    }
    SUBCASE("unchecked blocks actions") {
        associations.checked = false;
        action = woby::FileAssociationAction::none;
    }
    SUBCASE("busy blocks Windows defaults") {
        associations.types[0].registeredCurrentCopy = true;
        associations.busy = true;
        button = "Windows defaults...";
        rowControl = false;
        action = woby::FileAssociationAction::none;
    }
    SUBCASE("unregistered copy cannot select defaults") {
        button = "Windows defaults...";
        rowControl = false;
        action = woby::FileAssociationAction::none;
    }
    SUBCASE("another copy can be replaced") {
        associations.types[0].registeredExecutable = std::filesystem::absolute(std::filesystem::temp_directory_path())
            / ("woby-other-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) / "woby.exe";
    }
    bool open = true;
    const auto frame = [&]() {
        ImGui::NewFrame();
        const auto result = woby::drawSettingsDialog(scene, std::exchange(open, false), ImVec2(100, 60), associations);
        ImGui::EndFrame();
        return result;
    };
    frame();
    frame();
    auto* window = ImGui::FindWindowByName("Settings");
    REQUIRE(window);
    ImGui::ActivateItemByID(rowControl ? ImHashStr(button, 0, window->GetID(row)) : window->GetID(button));
    const auto result = frame();
    CHECK(result.open);
    CHECK(result.fileAssociationRequest.action == action);
    if (rowControl && action != woby::FileAssociationAction::none) {
        CHECK(result.fileAssociationRequest.type == (row == 0 ? woby::FileAssociationType::scene : woby::FileAssociationType::obj));
    }
    CHECK_FALSE(scene.isDirty);
    CHECK_FALSE(result.scaleChanged);
}

TEST_CASE("Windows association settings keep long paths and default identifiers out of the layout")
{
    SettingsUiFixture ui;
    ImGui::GetIO().DisplaySize = ImVec2(640, 480);
    woby::UiState scene;
    woby::FileAssociationsState associations;
    associations.supported = true;
    associations.checked = true;
    associations.message = std::string(1000, 'x');
    const auto root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (auto& status : associations.types) {
        status.registeredExecutable = root / std::string(200, 'a') / "woby.exe";
        status.defaultApp = std::string(200, 'x');
    }
    SUBCASE("available") {
        associations.types[0].registeredCurrentCopy = true;
    }
    SUBCASE("default") {
        associations.types[0].registeredCurrentCopy = true;
        associations.types[0].defaultCurrentCopy = true;
    }
    SUBCASE("error") { associations.failed = true; }
    for (int frame = 0; frame < 3; ++frame) {
        ImGui::NewFrame();
        woby::drawSettingsDialog(scene, frame == 0, ImVec2(20, 20), associations);
        ImGui::EndFrame();
    }
    auto* window = ImGui::FindWindowByName("Settings");
    REQUIRE(window);
    CHECK(window->Size.y <= 360);
    CHECK(window->Size.x == doctest::Approx(400));
    CHECK(window->ScrollMax.y == 0);
}

TEST_CASE("Windows association worker failures remain visible and allow a status retry")
{
    woby::FileAssociationsRuntime runtime;
    runtime.state.supported = true;
    runtime.state.busy = true;
    std::promise<woby::FileAssociationsState> result;
    runtime.work = result.get_future();
    result.set_exception(std::make_exception_ptr(std::runtime_error("Registry access failed")));
    woby::pollFileAssociations(runtime);
    CHECK_FALSE(runtime.state.busy);
    CHECK(runtime.state.failed);
    CHECK_FALSE(runtime.work.valid());

    SettingsUiFixture ui;
    woby::UiState scene;
    bool open = true;
    const auto frame = [&]() {
        ImGui::NewFrame();
        const auto action = woby::drawSettingsDialog(scene, std::exchange(open, false), ImVec2(20, 20), runtime.state);
        ImGui::EndFrame();
        return action;
    };
    frame(); frame();
    auto* window = ImGui::FindWindowByName("Settings");
    REQUIRE(window);
    ImGui::ActivateItemByID(window->GetID("Retry"));
    CHECK(frame().fileAssociationRequest.action == woby::FileAssociationAction::refresh);
}
