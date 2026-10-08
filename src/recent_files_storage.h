#pragma once

#include "recent_files.h"
#include <string>
#include <string_view>

namespace woby {

struct RecentFilesStorageResult {
    RecentFiles files;
    std::string warning;
};

[[nodiscard]] std::filesystem::path recentFilesStoragePath();
// Explicit inputs allow directory rules to be tested without changing the environment.
[[nodiscard]] std::filesystem::path linuxRecentFilesStoragePath(std::string_view stateHome, std::string_view userHome);
// Missing history is empty. Invalid JSON is recoverable; IO errors and newer versions throw.
[[nodiscard]] RecentFilesStorageResult readRecentFiles(const std::filesystem::path& path);
// Lock, reread, apply changes, and replace with a sibling temporary file.
[[nodiscard]] RecentFilesStorageResult updateRecentFiles(const std::filesystem::path& path,
    const std::vector<RecentFileChange>& changes);

} // namespace woby
