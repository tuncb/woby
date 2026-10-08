#pragma once

#include "recent_files_storage.h"
#include <future>

namespace woby {

struct RecentFilesWorkResult {
    RecentFilesStorageResult storage;
    std::string error;
    uint64_t revision = 0;
};

struct RecentFilesRuntime {
    RecentFiles files;
    std::filesystem::path storagePath;
    std::vector<RecentFileChange> pending;
    std::future<RecentFilesWorkResult> work;
    uint64_t revision = 0;
    bool enabled = false;
    bool sessionOnly = false;
    std::string warning;
};

// Call only for desktop sessions. An explicit storage path is used by tests.
void startRecentFiles(RecentFilesRuntime& runtime, const std::filesystem::path& storagePath = {});
void rememberRecentFile(RecentFilesRuntime& runtime, RecentFileKind kind, const std::filesystem::path& path);
void removeRecentFile(RecentFilesRuntime& runtime, RecentFileKind kind, const std::filesystem::path& path);
void clearRecentFiles(RecentFilesRuntime& runtime);
// Returns a notification once. No blocking or filesystem access on the UI thread.
[[nodiscard]] std::string pollRecentFiles(RecentFilesRuntime& runtime);
// Drain queued writes before normal shutdown.
void finishRecentFiles(RecentFilesRuntime& runtime);

} // namespace woby
