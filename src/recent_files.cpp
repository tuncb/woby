#include "recent_files.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace woby {
namespace {
bool validEntry(const RecentFileEntry& entry)
{
    return (entry.kind == RecentFileKind::scene || entry.kind == RecentFileKind::model)
        && entry.path.is_absolute() && entry.path.lexically_normal().has_filename()
        && entry.path.native().find(std::filesystem::path::value_type{}) == std::filesystem::path::string_type::npos;
}
} // namespace

void normalizeRecentFiles(RecentFiles& files)
{
    std::erase_if(files.entries, [](const auto& entry) { return !validEntry(entry); });
    for (auto& entry : files.entries) { entry.path = entry.path.lexically_normal(); }
    std::stable_sort(files.entries.begin(), files.entries.end(), [](const auto& left, const auto& right) {
        return left.lastUsedUnixMilliseconds > right.lastUsedUnixMilliseconds;
    });
    RecentFiles normalized;
    normalized.entries.reserve(std::min(files.entries.size(), 2 * recentFileLimit));
    for (auto& entry : files.entries) {
        if (recentFileCount(normalized, entry.kind) >= recentFileLimit) { continue; }
        const bool duplicate = std::any_of(normalized.entries.begin(), normalized.entries.end(), [&](const auto& item) {
            return item.kind == entry.kind && item.path == entry.path;
        });
        if (!duplicate) { normalized.entries.push_back(std::move(entry)); }
    }
    files = std::move(normalized);
}

void applyRecentFileChange(RecentFiles& files, const RecentFileChange& change)
{
    if (change.action == RecentFileAction::clear) { files.entries.clear(); return; }
    if (!validEntry(change.entry)) { throw std::invalid_argument("Recent file paths must be absolute file paths."); }
    auto entry = change.entry;
    entry.path = entry.path.lexically_normal();
    if (change.action == RecentFileAction::remember && std::any_of(files.entries.begin(), files.entries.end(), [&](const auto& item) {
            return item.kind == entry.kind && item.path == entry.path
                && item.lastUsedUnixMilliseconds > entry.lastUsedUnixMilliseconds;
        })) { return; }
    std::erase_if(files.entries, [&](const auto& item) { return item.kind == entry.kind && item.path == entry.path; });
    if (change.action == RecentFileAction::remember) { files.entries.insert(files.entries.begin(), std::move(entry)); }
    normalizeRecentFiles(files);
}

size_t recentFileCount(const RecentFiles& files, RecentFileKind kind)
{
    return static_cast<size_t>(std::count_if(files.entries.begin(), files.entries.end(),
        [kind](const auto& entry) { return entry.kind == kind; }));
}
} // namespace woby
