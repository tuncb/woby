#pragma once

#include "scene_file.h"
#include "ui_state.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace woby {

enum class BackgroundLoadStage {
    loading,
};

struct BackgroundLoadProgress {
    BackgroundLoadStage stage = BackgroundLoadStage::loading;
    std::filesystem::path currentPath;
    size_t completedCount = 0;
    size_t totalCount = 0;
};

using BackgroundLoadProgressCallback = std::function<void(const BackgroundLoadProgress&)>;
using BackgroundLoadCancelCallback = std::function<bool()>;

struct ObjBatchCpuLoadResult {
    std::vector<UiFileState> files;
    size_t requestedCount = 0;
    size_t addedCount = 0;
    size_t skippedCount = 0;
    size_t failedCount = 0;
    bool canceled = false;
    std::string lastError;
    std::string status;
};

struct SceneCpuLoadResult {
    std::filesystem::path scenePath;
    SceneDocument document;
    std::vector<UiFileState> files;
    bool canceled = false;
};

[[nodiscard]] ObjBatchCpuLoadResult loadObjBatchCpu(
    const std::vector<std::filesystem::path>& modelPaths,
    size_t firstColorIndex,
    const BackgroundLoadProgressCallback& progress,
    const BackgroundLoadCancelCallback& shouldCancel);

[[nodiscard]] SceneCpuLoadResult loadSceneCpu(
    const std::filesystem::path& scenePath,
    const BackgroundLoadProgressCallback& progress,
    const BackgroundLoadCancelCallback& shouldCancel);

} // namespace woby
