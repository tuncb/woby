#include "update_internal.h"
#include "utf8_path.h"
#include <doctest/doctest.h>
#include <archive.h>
#include <archive_entry.h>
#include <chrono>
#include <fstream>
#include <map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;
using nlohmann::json;

void cleanDirectory(fs::path* path)
{
    std::error_code ignored;
    fs::remove_all(*path, ignored);
    delete path;
}

auto temporaryDirectory()
{
    static unsigned int serial = 0;
    auto path = fs::temp_directory_path() / ("woby-update-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
    fs::create_directory(path);
    return std::unique_ptr<fs::path, decltype(&cleanDirectory)>(new fs::path(path), cleanDirectory);
}

void writeFile(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

std::string readFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

json makePackage(const fs::path& root, const std::string& version,
    const std::map<std::string, std::string>& extra = {})
{
    std::map<std::string, std::string> files{{woby::updateExecutableName(), "binary " + version},
        {woby::updateExecutableName(true), "helper " + version},
        {"assets/fonts/RobotoMonoNerdFont-Regular.ttf", "font " + version}};
    files.insert(extra.begin(), extra.end());
    const auto platform = woby::updatePlatform();
    const auto shader = platform == "windows-x64" ? "dx11" : platform == "macos-arm64" ? "metal" : "glsl";
    for (const auto* stage : {"vs", "fs"}) {
        for (const auto* name : {"mesh", "color", "comparison", "imgui", "point_sprite"}) {
            files[std::string("assets/shaders/") + shader + "/" + stage + "_" + name + ".bin"] = "shader";
        }
    }
    json manifest{{"schema", 1}, {"version", version}, {"platform", woby::updatePlatform()}, {"files", json::array()}};
    for (const auto& [name, content] : files) {
        writeFile(root / woby::pathFromUtf8(name), content);
        const bool executable = name == woby::updateExecutableName() || name == woby::updateExecutableName(true);
#ifndef _WIN32
        if (executable) { fs::permissions(root / name, fs::perms::owner_exec, fs::perm_options::add); }
#endif
        manifest["files"].push_back({{"path", name}, {"sha256", woby::updateSha256(root / woby::pathFromUtf8(name))},
            {"size", content.size()}, {"executable", executable}});
    }
    woby::writeUpdateJson(root / woby::packageManifestName, manifest);
    return manifest;
}

fs::path jobDirectory(const fs::path& root)
{
    const auto job = root / ".woby-update/job-0123456789abcdef0123456789abcdef";
    fs::create_directories(job);
    return job;
}

json releaseMetadata()
{
    return {{"tag_name", "v1.10.0"}, {"draft", false}, {"prerelease", false}, {"assets", json::array({
        {{"name", "woby-windows-x64.zip"}, {"browser_download_url", "https://github.com/tuncb/woby/releases/download/v1.10.0/woby-windows-x64.zip"},
         {"size", 100}, {"digest", "sha256:" + std::string(64, 'a')}}})}};
}

void writeArchive(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files,
    bool tar = false, bool symlink = false)
{
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    REQUIRE(writer);
    if (tar) { REQUIRE(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK); REQUIRE(archive_write_add_filter_gzip(writer.get()) == ARCHIVE_OK); }
    else { REQUIRE(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK); }
#ifdef _WIN32
    REQUIRE(archive_write_open_filename_w(writer.get(), path.c_str()) == ARCHIVE_OK);
#else
    REQUIRE(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
#endif
    for (const auto& [name, content] : files) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname(entry.get(), name.c_str());
        archive_entry_set_filetype(entry.get(), symlink ? AE_IFLNK : AE_IFREG);
        const bool executable = name.ends_with("/" + woby::updateExecutableName()) || name.ends_with("/" + woby::updateExecutableName(true));
        archive_entry_set_perm(entry.get(), executable ? 0755 : 0644);
        archive_entry_set_size(entry.get(), symlink ? 0 : static_cast<la_int64_t>(content.size()));
        if (symlink) { archive_entry_set_symlink(entry.get(), content.c_str()); }
        REQUIRE(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
        if (!symlink) { REQUIRE(archive_write_data(writer.get(), content.data(), content.size()) == static_cast<la_ssize_t>(content.size())); }
    }
    REQUIRE(archive_write_close(writer.get()) == ARCHIVE_OK);
}
} // namespace

TEST_CASE("updater compares numeric versions and rejects ambiguous versions")
{
    CHECK(woby::parseUpdateVersion("1.10.0") > woby::parseUpdateVersion("1.9.99"));
    for (const auto* version : {"v1.2.3", "1.2", "1.2.3.4", "01.2.3", "1.2.-1", "1.2.3-rc1", "1.2.4294967296", "1.2.3 "}) {
        CHECK_THROWS((void)woby::parseUpdateVersion(version));
    }
}

TEST_CASE("updater selects exact GitHub assets and requires verification metadata")
{
    auto metadata = releaseMetadata();
    CHECK(woby::parseUpdateRelease(metadata.dump(), "windows-x64").version == "1.10.0");
    CHECK_THROWS((void)woby::parseUpdateRelease(metadata.dump(), "linux-x64"));
    SUBCASE("draft") { metadata["draft"] = true; }
    SUBCASE("prerelease") { metadata["prerelease"] = true; }
    SUBCASE("no digest") { metadata["assets"][0]["digest"] = nullptr; }
    SUBCASE("other host") { metadata["assets"][0]["browser_download_url"] = "https://example.com/woby.zip"; }
    SUBCASE("HTTP") { metadata["assets"][0]["browser_download_url"] = "http://github.com/tuncb/woby/releases/download/v1.10.0/woby-windows-x64.zip"; }
    SUBCASE("oversized") { metadata["assets"][0]["size"] = 1024ull * 1024 * 1024; }
    SUBCASE("duplicate") { metadata["assets"].push_back(metadata["assets"][0]); }
    CHECK_THROWS((void)woby::parseUpdateRelease(metadata.dump(), "windows-x64"));
}

TEST_CASE("updater rejects nonportable and escaping paths")
{
    CHECK(woby::validPackagePath("assets/fonts/font.ttf"));
    for (const auto* path : {"", ".", "..", "../x", "a/../x", "/x", "C:/x", "a\\b", "a//b", "file:stream", "a/CON.txt", "aux", "dir/file.", "dir/file ", "dir/", "a\nb"}) {
        CHECK_FALSE(woby::validPackagePath(path));
    }
}

TEST_CASE("updater SHA-256 and manifest detect altered files")
{
    const auto root = temporaryDirectory();
    writeFile(*root / "hash", "abc");
    CHECK(woby::updateSha256(*root / "hash") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    makePackage(*root, "1.0.0");
    const auto manifest = woby::readPackageManifest(*root);
    CHECK_NOTHROW(woby::validateUpdatePackage(*root, manifest));
    writeFile(*root / woby::updateExecutableName(), "corrupt");
    CHECK_THROWS(woby::validateUpdatePackage(*root, manifest));
}

TEST_CASE("updater validates manifest ownership before file operations")
{
    const auto root = temporaryDirectory();
    auto manifest = makePackage(*root, "1.0.0");
    SUBCASE("duplicate case") { auto file = manifest["files"][0]; file["path"] = "ASSETS/fonts/RobotoMonoNerdFont-Regular.ttf"; manifest["files"].push_back(file); }
    SUBCASE("reserved") { manifest["files"][0]["path"] = ".woby-update/status.json"; }
    SUBCASE("parent escape") { manifest["files"][0]["path"] = "../outside"; }
    SUBCASE("wrong platform") { manifest["platform"] = "unsupported"; }
    SUBCASE("wrong schema") { manifest["schema"] = 99; }
    SUBCASE("missing executable") { manifest["files"] = json::array(); }
    woby::writeUpdateJson(*root / woby::packageManifestName, manifest);
    CHECK_THROWS((void)woby::readPackageManifest(*root));
}

TEST_CASE("updater transaction replaces managed files and can recover repeatedly")
{
    const auto root = temporaryDirectory();
    makePackage(*root, "1.0.0", {{"obsolete.dll", "old runtime"}});
    writeFile(*root / "user/scene.woby", "user scene");
    writeFile(*root / "plugins/custom.dll", "user plugin");
    const auto job = jobDirectory(*root);
    makePackage(job / "package", "1.1.0", {{"new.dll", "new runtime"}});
    woby::applyUpdateTransaction(*root, job);
    CHECK(woby::readPackageManifest(*root).version == "1.1.0");
    CHECK_FALSE(fs::exists(*root / "obsolete.dll"));
    CHECK(readFile(*root / "new.dll") == "new runtime");
    CHECK(readFile(*root / "user/scene.woby") == "user scene");
    CHECK(readFile(*root / "plugins/custom.dll") == "user plugin");
    woby::recoverUpdateTransaction(*root, job);
    woby::recoverUpdateTransaction(*root, job);
    CHECK(woby::readPackageManifest(*root).version == "1.0.0");
    CHECK(readFile(*root / "obsolete.dll") == "old runtime");
    CHECK_FALSE(fs::exists(*root / "new.dll"));
    CHECK(readFile(*root / "user/scene.woby") == "user scene");
}

TEST_CASE("updater refuses collisions corrupt packages and downgrades without starting transaction")
{
    const auto root = temporaryDirectory();
    makePackage(*root, "1.0.0");
    const auto job = jobDirectory(*root);
    SUBCASE("unrelated file collision") { writeFile(*root / "custom.dll", "mine"); makePackage(job / "package", "1.1.0", {{"custom.dll", "new"}}); }
    SUBCASE("corrupt download") { makePackage(job / "package", "1.1.0"); writeFile(job / "package" / woby::updateExecutableName(), "corrupt"); }
    SUBCASE("downgrade") { makePackage(job / "package", "0.9.0"); }
    SUBCASE("same version") { makePackage(job / "package", "1.0.0"); }
    CHECK_THROWS(woby::applyUpdateTransaction(*root, job));
    CHECK_FALSE(fs::exists(job / "journal.json"));
    CHECK(woby::readPackageManifest(*root).version == "1.0.0");
}

TEST_CASE("updater ignores incomplete backups when deployment has not been touched")
{
    const auto root = temporaryDirectory();
    makePackage(*root, "1.0.0");
    const auto job = jobDirectory(*root);
    const auto name = woby::updateExecutableName();
    woby::writeUpdateJson(job / "journal.json", {{"schema", 1}, {"root", woby::pathToUtf8(*root)},
        {"files", json::array({{{"path", name}, {"existed", true}}})}});
    writeFile(job / "backup" / name, "partial backup");
    CHECK_NOTHROW(woby::recoverUpdateTransaction(*root, job));
    CHECK(readFile(*root / name) == "binary 1.0.0");
}

TEST_CASE("updater deployment locks permit viewers together and exclude updates")
{
    const auto root = temporaryDirectory();
    auto viewer = woby::lockDeployment(*root, false);
    auto otherViewer = woby::lockDeployment(*root, false);
    CHECK_THROWS((void)woby::lockDeployment(*root, true));
    viewer.reset(); otherViewer.reset();
    auto update = woby::lockDeployment(*root, true);
    CHECK_THROWS((void)woby::lockDeployment(*root, false));
    CHECK_THROWS((void)woby::lockDeployment(*root, true));
}

TEST_CASE("updater extracts verified zip and tar gzip packages")
{
    const auto root = temporaryDirectory();
    const auto manifest = makePackage(*root / "source", "1.0.0");
    const auto prefix = "woby-" + woby::updatePlatform() + "/";
    std::vector<std::pair<std::string, std::string>> files{{prefix + woby::packageManifestName, manifest.dump()}};
    for (const auto& file : manifest.at("files")) {
        const auto name = file.at("path").get<std::string>();
        files.emplace_back(prefix + name, readFile(*root / "source" / name));
    }
    bool tar = false;
    SUBCASE("zip") {}
    SUBCASE("tar.gz") { tar = true; }
    writeArchive(*root / "archive", files, tar);
    CHECK_NOTHROW(woby::extractUpdateArchive(*root / "archive", *root / "unpacked", woby::updatePlatform()));
    CHECK(woby::readPackageManifest(*root / "unpacked").version == "1.0.0");
}

TEST_CASE("updater rejects archive traversal links duplicates and incomplete archives")
{
    const auto root = temporaryDirectory();
    const auto prefix = "woby-" + woby::updatePlatform() + "/";
    std::vector<std::pair<std::string, std::string>> files;
    bool link = false;
    SUBCASE("escape") { files = {{prefix + "../outside", "bad"}}; }
    SUBCASE("absolute") { files = {{"/outside", "bad"}}; }
    SUBCASE("backslash") { files = {{prefix + "..\\outside", "bad"}}; }
    SUBCASE("symlink") { files = {{prefix + "link", "../../outside"}}; link = true; }
    SUBCASE("case collision") { files = {{prefix + "file", "one"}, {prefix + "FILE", "two"}}; }
    SUBCASE("missing manifest") { files = {{prefix + "woby", "binary"}}; }
    SUBCASE("reserved") { files = {{prefix + ".woby-update/journal.json", "bad"}}; }
    writeArchive(*root / "archive", files, true, link);
    CHECK_THROWS(woby::extractUpdateArchive(*root / "archive", *root / "unpacked", woby::updatePlatform()));
    CHECK_FALSE(fs::exists(*root / "outside"));
}

#ifdef _WIN32
TEST_CASE("updater recovers earlier replacements after a locked file blocks installation")
{
    const auto root = temporaryDirectory();
    makePackage(*root, "1.0.0", {{"zzz-locked.dll", "old"}});
    const auto job = jobDirectory(*root);
    makePackage(job / "package", "1.1.0", {{"zzz-locked.dll", "new"}});
    const auto file = CreateFileW((*root / "zzz-locked.dll").c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    CHECK_THROWS(woby::applyUpdateTransaction(*root, job));
    CHECK_NOTHROW(woby::recoverUpdateTransaction(*root, job));
    CloseHandle(file);
    CHECK(woby::readPackageManifest(*root).version == "1.0.0");
}
#endif
