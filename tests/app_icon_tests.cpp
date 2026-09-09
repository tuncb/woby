#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Bytes = std::vector<unsigned char>;

Bytes readIconFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE_MESSAGE(stream.is_open(), path.string());
    return Bytes(std::istreambuf_iterator<char>(stream), {});
}

uint32_t littleEndian(const Bytes& bytes, size_t offset, size_t count)
{
    REQUIRE(count <= 4);
    REQUIRE(offset + count <= bytes.size());
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        value |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

constexpr std::array<uint32_t, 9> windowsIconSizes{16, 20, 24, 32, 40, 48, 64, 128, 256};

#ifdef _WIN32
Bytes iconResource(HMODULE module, WORD id, WORD type)
{
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(type));
    REQUIRE(resource != nullptr);
    const HGLOBAL loaded = LoadResource(module, resource);
    REQUIRE(loaded != nullptr);
    const auto* bytes = static_cast<const unsigned char*>(LockResource(loaded));
    REQUIRE(bytes != nullptr);
    return Bytes(bytes, bytes + SizeofResource(module, resource));
}
#endif

} // namespace

TEST_CASE("app window icon is a staged 256 pixel bitmap")
{
    const auto staged = readIconFile(std::filesystem::path(WOBY_TEST_ASSET_DIRECTORY) / "icons/woby.bmp");
    const auto source = readIconFile(std::filesystem::path(WOBY_TEST_ICON_DIRECTORY).parent_path().parent_path()
        / "assets/icons/woby.bmp");
    CHECK(staged == source);
    CHECK(littleEndian(staged, 0, 2) == 0x4d42);
    CHECK(littleEndian(staged, 2, 4) == staged.size());
    CHECK(littleEndian(staged, 18, 4) == 256);
    CHECK(littleEndian(staged, 22, 4) == 256);
    CHECK(littleEndian(staged, 26, 2) == 1);
    CHECK(littleEndian(staged, 28, 2) == 24);
    CHECK(littleEndian(staged, 30, 4) == 0);
    CHECK(staged.size() == littleEndian(staged, 10, 4) + 256 * 256 * 3);
}

TEST_CASE("Windows app icon includes standard and high DPI sizes")
{
    const auto icon = readIconFile(std::filesystem::path(WOBY_TEST_ICON_DIRECTORY) / "woby.ico");
    CHECK(littleEndian(icon, 0, 2) == 0);
    CHECK(littleEndian(icon, 2, 2) == 1);
    REQUIRE(littleEndian(icon, 4, 2) == windowsIconSizes.size());
    for (size_t i = 0; i < windowsIconSizes.size(); ++i) {
        const size_t entry = 6 + 16 * i;
        const auto encodedSize = windowsIconSizes[i] % 256;
        CHECK(littleEndian(icon, entry, 1) == encodedSize);
        CHECK(littleEndian(icon, entry + 1, 1) == encodedSize);
        const auto length = littleEndian(icon, entry + 8, 4);
        const auto offset = littleEndian(icon, entry + 12, 4);
        REQUIRE(static_cast<size_t>(offset) + length <= icon.size());
        REQUIRE(length >= 24);
        CHECK(littleEndian(icon, offset, 4) == 0x474e5089); // Embedded PNG.
    }
}

#ifdef _WIN32
TEST_CASE("Windows executable embeds the selected icon at every size")
{
    const auto app = std::filesystem::path(WOBY_TEST_APP);
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module(
        LoadLibraryExW(app.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE), &FreeLibrary);
    REQUIRE(module != nullptr);
    const auto group = iconResource(module.get(), 1, 14);
    REQUIRE(littleEndian(group, 4, 2) == windowsIconSizes.size());
    const auto source = readIconFile(std::filesystem::path(WOBY_TEST_ICON_DIRECTORY) / "woby.ico");
    for (size_t i = 0; i < windowsIconSizes.size(); ++i) {
        const size_t entry = 6 + 14 * i;
        const auto encodedSize = windowsIconSizes[i] % 256;
        CHECK(littleEndian(group, entry, 1) == encodedSize);
        CHECK(littleEndian(group, entry + 1, 1) == encodedSize);
        const auto id = static_cast<WORD>(littleEndian(group, entry + 12, 2));
        const auto embedded = iconResource(module.get(), id, 3);
        const auto length = littleEndian(source, 6 + 16 * i + 8, 4);
        const auto offset = littleEndian(source, 6 + 16 * i + 12, 4);
        REQUIRE(static_cast<size_t>(offset) + length <= source.size());
        CHECK(embedded == Bytes(source.data() + offset, source.data() + offset + length));
    }
}
#endif

TEST_CASE("macOS app icon is stored as an ICNS asset")
{
    const auto source = readIconFile(std::filesystem::path(WOBY_TEST_ICON_DIRECTORY) / "woby.icns");
    CHECK(littleEndian(source, 0, 4) == 0x736e6369);
#ifdef __APPLE__
    const auto contents = std::filesystem::path(WOBY_TEST_APP).parent_path().parent_path();
    CHECK(readIconFile(contents / "Resources/woby.icns") == source);
    const auto plist = readIconFile(contents / "Info.plist");
    CHECK(std::string(plist.begin(), plist.end()).find("<string>woby.icns</string>") != std::string::npos);
#endif
}
