#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace woby {

inline constexpr size_t recentFileLimit = 20;
enum class RecentFileKind { scene, model };
enum class RecentFileAction { remember, remove, clear };

struct RecentFileEntry {
    RecentFileKind kind = RecentFileKind::scene;
    std::filesystem::path path;
    uint64_t lastUsedUnixMilliseconds = 0;
    bool operator==(const RecentFileEntry&) const = default;
};

// Application history, independent of scene content, dirty checks, and Undo/Redo.
struct RecentFiles {
    std::vector<RecentFileEntry> entries;
    bool operator==(const RecentFiles&) const = default;
};

struct RecentFileChange {
    RecentFileAction action = RecentFileAction::remember;
    RecentFileEntry entry;
};

// Deterministic operations: paths must be absolute; no filesystem access.
void normalizeRecentFiles(RecentFiles& files);
void applyRecentFileChange(RecentFiles& files, const RecentFileChange& change);
[[nodiscard]] size_t recentFileCount(const RecentFiles& files, RecentFileKind kind);

} // namespace woby
