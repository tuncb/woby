#include "recent_files_runtime.h"

#include <chrono>
#include <utility>

namespace woby {
namespace {
void startWork(RecentFilesRuntime& runtime, bool loadOnly)
{
    if (runtime.work.valid() || runtime.sessionOnly || !runtime.enabled) { return; }
    const auto path = runtime.storagePath;
    const auto revision = runtime.revision;
    auto changes = std::move(runtime.pending);
    runtime.pending.clear();
    try {
        runtime.work = std::async(std::launch::async, [path, revision, changes = std::move(changes), loadOnly]() {
            RecentFilesWorkResult result;
            result.revision = revision;
            try { result.storage = loadOnly ? readRecentFiles(path) : updateRecentFiles(path, changes); }
            catch (const std::exception& error) { result.error = error.what(); }
            return result;
        });
    } catch (const std::exception& error) {
        runtime.sessionOnly = true;
        runtime.warning = std::string("Recent files are session-only: ") + error.what();
    }
}

void consumeWork(RecentFilesRuntime& runtime)
{
    auto result = runtime.work.get();
    if (!result.error.empty()) {
        runtime.sessionOnly = true;
        runtime.pending.clear();
        runtime.warning = "Recent files are session-only: " + result.error;
    } else {
        // A load or write must never replace a newer menu edit.
        if (result.revision == runtime.revision) { runtime.files = std::move(result.storage.files); }
        if (!result.storage.warning.empty()) { runtime.warning = std::move(result.storage.warning); }
    }
}

void queueChange(RecentFilesRuntime& runtime, RecentFileChange change)
{
    if (!runtime.enabled) { return; }
    applyRecentFileChange(runtime.files, change);
    ++runtime.revision;
    if (!runtime.sessionOnly) { runtime.pending.push_back(std::move(change)); }
}
} // namespace

void startRecentFiles(RecentFilesRuntime& runtime, const std::filesystem::path& storagePath)
{
    runtime.enabled = true;
    try {
        runtime.storagePath = storagePath.empty() ? recentFilesStoragePath() : storagePath;
        startWork(runtime, true);
    } catch (const std::exception& error) {
        runtime.sessionOnly = true;
        runtime.warning = std::string("Recent files are session-only: ") + error.what();
    }
}

void rememberRecentFile(RecentFilesRuntime& runtime, RecentFileKind kind, const std::filesystem::path& path)
{
    if (!runtime.enabled) { return; }
    try {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        queueChange(runtime, {RecentFileAction::remember,
            {kind, std::filesystem::absolute(path).lexically_normal(), static_cast<uint64_t>(now)}});
    } catch (const std::exception& error) { runtime.warning = std::string("Could not remember recent file: ") + error.what(); }
}

void removeRecentFile(RecentFilesRuntime& runtime, RecentFileKind kind, const std::filesystem::path& path)
{
    queueChange(runtime, {RecentFileAction::remove, {kind, path, 0}});
}

void clearRecentFiles(RecentFilesRuntime& runtime)
{
    queueChange(runtime, {RecentFileAction::clear, {}});
}

std::string pollRecentFiles(RecentFilesRuntime& runtime)
{
    if (runtime.work.valid() && runtime.work.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        consumeWork(runtime);
    }
    if (!runtime.pending.empty()) { startWork(runtime, false); }
    return std::exchange(runtime.warning, {});
}

void finishRecentFiles(RecentFilesRuntime& runtime)
{
    while (runtime.work.valid() || (!runtime.sessionOnly && !runtime.pending.empty())) {
        if (runtime.work.valid()) { consumeWork(runtime); }
        if (!runtime.pending.empty()) { startWork(runtime, false); }
    }
}
} // namespace woby
