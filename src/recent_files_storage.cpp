#include "recent_files_storage.h"
#include "utf8_path.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <random>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#elif defined(__APPLE__)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#endif
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace woby {
namespace {
void requireStoragePath(const std::filesystem::path& path)
{
    if (!path.is_absolute() || !path.has_filename()) {
        throw std::invalid_argument("The recent files storage path must be absolute.");
    }
}

struct HistoryLock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~HistoryLock() { if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); } }
#else
    int handle = -1;
    ~HistoryLock() { if (handle >= 0) { ::close(handle); } }
#endif
};

void acquireHistoryLock(HistoryLock& lock, const std::filesystem::path& path)
{
    auto lockPath = path;
    lockPath += ".lock";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
#ifndef _WIN32
    lock.handle = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock.handle < 0) { throw std::runtime_error("Cannot open the recent files lock."); }
#endif
    do {
#ifdef _WIN32
        lock.handle = CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (lock.handle != INVALID_HANDLE_VALUE) { return; }
        if (GetLastError() != ERROR_SHARING_VIOLATION) { break; }
#else
        if (::flock(lock.handle, LOCK_EX | LOCK_NB) == 0) { return; }
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) { break; }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Cannot lock recent files history.");
}

struct TemporaryHistory {
    std::filesystem::path directory;
    ~TemporaryHistory() {
        if (!directory.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    }
};

void writeHistory(const std::filesystem::path& path, const RecentFiles& files)
{
    nlohmann::json data = {{"version", 1}, {"entries", nlohmann::json::array()}};
    for (const auto& entry : files.entries) {
        data["entries"].push_back({{"kind", entry.kind == RecentFileKind::scene ? "scene" : "model"},
            {"path", pathToUtf8(entry.path)}, {"last_used_unix_ms", entry.lastUsedUnixMilliseconds}});
    }
    TemporaryHistory temporary;
    std::random_device random;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto directory = path.parent_path() / (".woby-recents-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (std::filesystem::create_directory(directory)) { temporary.directory = std::move(directory); break; }
    }
    if (temporary.directory.empty()) { throw std::runtime_error("Cannot create a temporary recent files directory."); }
#ifndef _WIN32
    std::filesystem::permissions(temporary.directory, std::filesystem::perms::owner_all);
#endif
    const auto temporaryPath = temporary.directory / "recents.tmp";
    std::ofstream output;
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.open(temporaryPath, std::ios::binary | std::ios::trunc);
    output << data.dump(2) << '\n';
    output.flush();
    output.close();
#ifdef _WIN32
    const auto handle = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { throw std::runtime_error("Cannot flush recent files history."); }
    const bool flushed = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    if (!flushed || !MoveFileExW(temporaryPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Cannot replace recent files history.");
    }
#else
    std::filesystem::permissions(temporaryPath, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const int handle = ::open(temporaryPath.c_str(), O_RDWR | O_CLOEXEC);
    if (handle < 0) { throw std::runtime_error("Cannot flush recent files history."); }
    const bool flushed = ::fsync(handle) == 0;
    ::close(handle);
    if (!flushed) { throw std::runtime_error("Cannot flush recent files history."); }
    std::filesystem::rename(temporaryPath, path);
#endif
}
} // namespace

std::filesystem::path linuxRecentFilesStoragePath(std::string_view stateHome, std::string_view userHome)
{
    const auto state = pathFromUtf8(stateHome);
    if (state.is_absolute()) { return state / "woby" / "recents.json"; }
    const auto home = pathFromUtf8(userHome);
    if (!home.is_absolute()) { throw std::runtime_error("Cannot locate the recent files state directory."); }
    return home / ".local" / "state" / "woby" / "recents.json";
}

std::filesystem::path recentFilesStoragePath()
{
#ifdef _WIN32
    PWSTR directory = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &directory))) {
        throw std::runtime_error("Cannot locate Local AppData for recent files.");
    }
    const std::filesystem::path root(directory);
    CoTaskMemFree(directory);
    return root / "woby" / "recents.json";
#elif defined(__APPLE__)
    char* directory = SDL_GetPrefPath("woby", "woby");
    if (!directory) { throw std::runtime_error("Cannot locate Application Support for recent files."); }
    const auto root = pathFromUtf8(directory);
    SDL_free(directory);
    return root / "recents.json";
#else
    const char* state = std::getenv("XDG_STATE_HOME");
    const char* home = std::getenv("HOME");
    return linuxRecentFilesStoragePath(state ? state : "", home ? home : "");
#endif
}

namespace {
RecentFilesStorageResult readHistory(const std::filesystem::path& path)
{
    requireStoragePath(path);
    if (!std::filesystem::exists(path)) { return {}; }
    if (!std::filesystem::is_regular_file(path)) { throw std::runtime_error("Recent files history is not a regular file."); }
    if (std::filesystem::file_size(path) > 1024 * 1024) {
        return {{}, "Recent files history is too large; the history was reset."};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot read recent files history."); }
    const auto data = nlohmann::json::parse(input, nullptr, false);
    if (input.bad()) { throw std::runtime_error("Cannot read recent files history."); }
    if (!data.is_object() || !data.contains("version") || !data["version"].is_number_integer()) {
        return {{}, "Invalid recent files history; the history was reset."};
    }
    if (data["version"] != 1) { throw std::runtime_error("Unsupported recent files history version."); }
    if (!data.contains("entries") || !data["entries"].is_array()) {
        return {{}, "Invalid recent files history; the history was reset."};
    }
    RecentFilesStorageResult result;
    for (const auto& item : data["entries"]) {
        if (!item.is_object() || !item.contains("kind") || !item["kind"].is_string()
            || (item["kind"] != "scene" && item["kind"] != "model")
            || !item.contains("path") || !item["path"].is_string()
            || !item.contains("last_used_unix_ms") || !item["last_used_unix_ms"].is_number_unsigned()) {
            result.warning = "Some recent files entries were invalid and were removed.";
            continue;
        }
        try {
            RecentFileEntry entry{item["kind"] == "scene" ? RecentFileKind::scene : RecentFileKind::model,
                pathFromUtf8(item["path"].get<std::string>()), item["last_used_unix_ms"].get<uint64_t>()};
            RecentFiles checked{{entry}};
            normalizeRecentFiles(checked);
            if (checked.entries.empty()) { result.warning = "Some recent files entries were invalid and were removed."; }
            else { result.files.entries.push_back(std::move(checked.entries.front())); }
        } catch (const std::exception&) {
            result.warning = "Some recent files entries were invalid and were removed.";
        }
    }
    normalizeRecentFiles(result.files);
    return result;
}
} // namespace

RecentFilesStorageResult readRecentFiles(const std::filesystem::path& path)
{
    requireStoragePath(path);
    if (!std::filesystem::exists(path)) { return {}; }
    // Readers use the stable lock too: Windows streams need not share delete access.
    HistoryLock lock;
    acquireHistoryLock(lock, path);
    return readHistory(path);
}

RecentFilesStorageResult updateRecentFiles(const std::filesystem::path& path,
    const std::vector<RecentFileChange>& changes)
{
    requireStoragePath(path);
    const bool created = std::filesystem::create_directories(path.parent_path());
#ifndef _WIN32
    if (created) { std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all); }
#else
    (void)created;
#endif
    HistoryLock lock;
    acquireHistoryLock(lock, path);
    auto result = readHistory(path);
    for (const auto& change : changes) {
        // Filesystem identity handles case rules and aliases without lowercasing paths.
        if (change.action != RecentFileAction::clear) {
            const auto sameFile = [&](const auto& entry) {
                if (entry.kind != change.entry.kind) { return false; }
                if (entry.path == change.entry.path.lexically_normal()) { return true; }
                std::error_code error;
                return std::filesystem::equivalent(entry.path, change.entry.path, error) && !error;
            };
            // A delayed writer must not replace a more recent use from another instance.
            if (change.action == RecentFileAction::remember && std::any_of(result.files.entries.begin(), result.files.entries.end(), [&](const auto& entry) {
                    return entry.lastUsedUnixMilliseconds > change.entry.lastUsedUnixMilliseconds && sameFile(entry);
                })) { continue; }
            std::erase_if(result.files.entries, sameFile);
        }
        applyRecentFileChange(result.files, change);
    }
    writeHistory(path, result.files);
    return result;
}
} // namespace woby
