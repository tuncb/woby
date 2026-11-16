module;

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "utf8_path.h"

#include <doctest/doctest.h>
#include <chrono>
#include <stdexcept>
#include <utility>
#include <vector>

module woby.file_associations;
import :windows;

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
