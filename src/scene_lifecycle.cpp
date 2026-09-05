#include "scene_lifecycle.h"
#include "ui_operations.h"
#include "performance_log.h"
#include <spdlog/spdlog.h>

namespace woby {

std::optional<SceneLifecycleError> saveSceneState(UiState& state,
    std::optional<std::filesystem::path>& currentPath, SceneDocument& cleanDocument,
    const std::filesystem::path& requestedPath, bool overwrite)
{
    if (requestedPath.empty()) {
        return SceneLifecycleError{"save_path_required", "Supply a savePath for this untitled scene.", -32012};
    }
    try {
        const auto started = PerformanceClock::now();
        auto path = std::filesystem::absolute(sceneSavePathWithExtension(requestedPath)).lexically_normal();
        const auto documentStarted = PerformanceClock::now();
        auto document = createSceneDocument(state);
        const auto writeStarted = PerformanceClock::now();
        writeSceneDocument(path, document, overwrite);
        const auto written = PerformanceClock::now();
        currentPath = std::move(path);
        cleanDocument = std::move(document);
        clearSceneDirty(state);
        try {
            spdlog::info("perf scene_save path=\"{}\" files={} document_ms={} write_ms={} total_ms={}",
                currentPath->string(), cleanDocument.files.size(),
                millisecondsBetween(documentStarted, writeStarted), millisecondsBetween(writeStarted, written),
                millisecondsBetween(started, written));
        } catch (...) {
            // Diagnostics cannot turn an already committed save into a failure.
        }
        return {};
    } catch (const std::filesystem::filesystem_error& error) {
        if (error.code() == std::errc::file_exists) {
            return SceneLifecycleError{"destination_exists", "Destination exists; supply overwrite: true to replace it.", -32013};
        }
        return SceneLifecycleError{"save_failed", error.what(), -32015};
    } catch (const std::exception& error) {
        return SceneLifecycleError{"save_failed", error.what(), -32015};
    }
}

std::optional<SceneLifecycleError> beginSceneLifecycle(const SceneLifecycleCommand& command,
    UiState& state, std::optional<std::filesystem::path>& currentPath,
    SceneDocument& cleanDocument, bool busy)
{
    if (busy) {
        return SceneLifecycleError{"scene_busy", "Wait for file processing, screenshots, and dialogs to finish.", -32014};
    }
    updateSceneDirty(state, cleanDocument);
    if (command.action == SceneAction::save || command.action == SceneAction::saveAs) {
        const bool saveAs = command.action == SceneAction::saveAs;
        return saveSceneState(state, currentPath, cleanDocument,
            saveAs ? command.path : currentPath.value_or(std::filesystem::path{}),
            saveAs ? command.overwrite : true);
    }
    if (!state.isDirty || command.onDirty == DirtyPolicy::discard) {
        return {};
    }
    if (command.onDirty == DirtyPolicy::error) {
        return SceneLifecycleError{"dirty_scene", "Unsaved scene changes; supply onDirty: save or discard.", -32011};
    }
    return saveSceneState(state, currentPath, cleanDocument,
        command.savePath.empty() ? currentPath.value_or(std::filesystem::path{}) : command.savePath,
        command.savePath.empty() || command.overwrite);
}

} // namespace woby
