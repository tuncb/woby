#include "file_associations_windows.h"
#include "utf8_path.h"
#include "settings_dialog.h"
#include "ui_state.h"

#include <doctest/doctest.h>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>
#include <imgui_internal.h>

namespace {
struct AssociationFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-associations-" + std::to_string(GetCurrentProcessId()) + "-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::wstring keyPath = L"Software\\" + root.filename().wstring();
    HKEY key = nullptr;
    std::filesystem::path executable = root / woby::pathFromUtf8("Woby space \xc3\xa9") / "woby.exe";

    AssociationFixture()
    {
        REQUIRE(RegCreateKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, nullptr, 0,
            KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS);
    }
    ~AssociationFixture()
    {
        if (key) { RegCloseKey(key); }
        CHECK(RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str()) == ERROR_SUCCESS);
    }
};

void setString(HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value)
{
    HKEY key = nullptr;
    REQUIRE(RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS);
    const auto status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    REQUIRE(status == ERROR_SUCCESS);
}

std::wstring getString(HKEY root, const wchar_t* path, const wchar_t* name)
{
    DWORD bytes = 0;
    const auto status = RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) { return {}; }
    REQUIRE(status == ERROR_SUCCESS);
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    REQUIRE(RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) == ERROR_SUCCESS);
    return buffer.data();
}

void eraseValue(HKEY root, const wchar_t* path, const wchar_t* name)
{
    HKEY key = nullptr;
    REQUIRE(RegOpenKeyExW(root, path, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS);
    const auto status = RegDeleteValueW(key, name);
    RegCloseKey(key);
    REQUIRE(status == ERROR_SUCCESS);
}

constexpr const wchar_t* mappings = L"Software\\Woby\\FileAssociations\\Capabilities\\FileAssociations";

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
} // namespace

TEST_CASE("Windows file association commands quote Unicode paths and select the correct loader")
{
    AssociationFixture fixture;
    CHECK(woby::fileAssociationOpenCommand(woby::FileAssociationType::scene, fixture.executable)
        == L"\"" + fixture.executable.native() + L"\" --woby \"%1\"");
    CHECK(woby::fileAssociationOpenCommand(woby::FileAssociationType::obj, fixture.executable)
        == L"\"" + fixture.executable.native() + L"\" --file \"%1\"");
    CHECK_THROWS_AS(woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene,
        "relative/woby.exe"), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(woby::fileAssociationOpenCommand(woby::FileAssociationType::scene,
        fixture.root / "bad\"name.exe")), std::invalid_argument);
    CHECK(getString(fixture.key, L"Software\\RegisteredApplications", L"Woby").empty());
}

TEST_CASE("Windows registration preserves existing defaults and other application entries")
{
    AssociationFixture fixture;
    setString(fixture.key, L"Software\\Classes\\.obj", nullptr, L"Other.Obj");
    setString(fixture.key, L"Software\\Classes\\.obj\\OpenWithProgids", L"Other.Obj", L"keep");
    const auto* userChoice = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.obj\\UserChoice";
    setString(fixture.key, userChoice, L"ProgId", L"Other.Obj");
    setString(fixture.key, userChoice, L"Hash", L"protected-value");
    setString(fixture.key, L"Software\\RegisteredApplications", L"Other", L"Other\\Capabilities");
    for (int repeat = 0; repeat < 2; ++repeat) {
        woby::registerFileAssociation(fixture.key, woby::FileAssociationType::obj, fixture.executable);
        CHECK(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::obj,
            fixture.executable).registeredCurrentCopy);
    }
    CHECK(getString(fixture.key, L"Software\\Classes\\Woby.WavefrontObj\\DefaultIcon", nullptr)
        == L"\"" + fixture.executable.native() + L"\",0");
    CHECK(getString(fixture.key, mappings, L".obj") == L"Woby.WavefrontObj");
    CHECK(getString(fixture.key, mappings, L".woby").empty());
    woby::removeFileAssociation(fixture.key, woby::FileAssociationType::obj, fixture.executable);
    woby::removeFileAssociation(fixture.key, woby::FileAssociationType::obj, fixture.executable);
    CHECK_FALSE(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::obj,
        fixture.executable).registeredCurrentCopy);
    CHECK(getString(fixture.key, L"Software\\Classes\\.obj", nullptr) == L"Other.Obj");
    CHECK(getString(fixture.key, L"Software\\Classes\\.obj\\OpenWithProgids", L"Other.Obj") == L"keep");
    CHECK(getString(fixture.key, userChoice, L"ProgId") == L"Other.Obj");
    CHECK(getString(fixture.key, userChoice, L"Hash") == L"protected-value");
    CHECK(getString(fixture.key, L"Software\\RegisteredApplications", L"Other") == L"Other\\Capabilities");
    CHECK(getString(fixture.key, L"Software\\RegisteredApplications", L"Woby").empty());
}

TEST_CASE("Windows scene and OBJ registrations can be removed independently")
{
    AssociationFixture fixture;
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::obj, fixture.executable);
    woby::removeFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    CHECK(getString(fixture.key, mappings, L".woby").empty());
    CHECK(getString(fixture.key, L"Software\\Classes\\Woby.Scene", L"WobyExecutable").empty());
    CHECK(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::obj,
        fixture.executable).registeredCurrentCopy);
    woby::removeFileAssociation(fixture.key, woby::FileAssociationType::obj, fixture.executable);
    CHECK(getString(fixture.key, L"Software\\RegisteredApplications", L"Woby").empty());
}

TEST_CASE("Windows removal cannot remove another portable copy or a changed open command")
{
    AssociationFixture fixture;
    const auto other = fixture.root / "other copy" / "woby.exe";
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, other);
    const auto status = woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::scene,
        fixture.executable);
    CHECK(status.registeredExecutable == other);
    CHECK_FALSE(status.registeredCurrentCopy);
    CHECK_THROWS(woby::removeFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable));
    CHECK(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::scene, other).registeredCurrentCopy);
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    setString(fixture.key, L"Software\\Classes\\Woby.Scene\\shell\\open\\command", nullptr, L"modified command");
    CHECK_FALSE(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::scene,
        fixture.executable).registeredCurrentCopy);
    CHECK_THROWS(woby::removeFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable));
    CHECK(getString(fixture.key, L"Software\\Classes\\Woby.Scene\\shell\\open\\command", nullptr) == L"modified command");
}

TEST_CASE("Windows registration detects and repairs incomplete entries")
{
    AssociationFixture fixture;
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    eraseValue(fixture.key, L"Software\\Classes\\.woby\\OpenWithProgids", L"Woby.Scene");
    CHECK_FALSE(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::scene,
        fixture.executable).registeredCurrentCopy);
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    eraseValue(fixture.key, mappings, L".woby");
    CHECK_FALSE(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::scene,
        fixture.executable).registeredCurrentCopy);
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    CHECK(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::scene,
        fixture.executable).registeredCurrentCopy);
}

TEST_CASE("Windows removal preserves shared registrations owned by another copy")
{
    AssociationFixture fixture;
    const auto other = fixture.root / "other" / "woby.exe";
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::obj, other);
    woby::removeFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    CHECK(woby::readFileAssociationRegistration(fixture.key, woby::FileAssociationType::obj, other).registeredCurrentCopy);
}

TEST_CASE("Windows removal preserves replacement capability mappings")
{
    AssociationFixture fixture;
    woby::registerFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    setString(fixture.key, mappings, L".woby", L"Replacement.Scene");
    setString(fixture.key, L"Software\\RegisteredApplications", L"Woby", L"Replacement\\Capabilities");
    woby::removeFileAssociation(fixture.key, woby::FileAssociationType::scene, fixture.executable);
    CHECK(getString(fixture.key, mappings, L".woby") == L"Replacement.Scene");
    CHECK(getString(fixture.key, L"Software\\RegisteredApplications", L"Woby") == L"Replacement\\Capabilities");
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
