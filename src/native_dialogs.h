#pragma once

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace woby {

struct ModelFileDialogState {
    std::mutex mutex;
    std::vector<std::filesystem::path> pendingPaths;
    std::vector<std::filesystem::path> pendingFolderTreeRoots;
    std::string status;
    uint64_t statusVersion = 0;
    bool open = false;
    bool folderTreeOpen = false;
};

struct SceneFileDialogState {
    std::mutex mutex;
    std::optional<std::filesystem::path> pendingOpenPath;
    std::optional<std::filesystem::path> pendingSavePath;
    std::string status;
    uint64_t statusVersion = 0;
    bool openDialogOpen = false;
    bool saveDialogOpen = false;
};

struct SceneScreenshotDialogState {
    std::mutex mutex;
    std::optional<std::filesystem::path> pendingSavePath;
    std::string status;
    uint64_t statusVersion = 0;
    bool saveDialogOpen = false;
};

void showModelFileDialog(SDL_Window* window, ModelFileDialogState& state);
void showModelFolderTreeDialog(SDL_Window* window, ModelFileDialogState& state);
[[nodiscard]] std::vector<std::filesystem::path> takePendingModelPaths(ModelFileDialogState& state);
[[nodiscard]] std::vector<std::filesystem::path> takePendingModelFolderTreeRoots(
    ModelFileDialogState& state);
void setModelFileDialogStatus(ModelFileDialogState& state, std::string status);
[[nodiscard]] std::string modelFileDialogStatus(ModelFileDialogState& state, uint64_t& statusVersion);
[[nodiscard]] bool modelFileDialogIsOpen(ModelFileDialogState& state);

void showOpenSceneDialog(SDL_Window* window, SceneFileDialogState& state);
void showSaveSceneDialog(SDL_Window* window, SceneFileDialogState& state);
[[nodiscard]] std::optional<std::filesystem::path> takePendingOpenScenePath(SceneFileDialogState& state);
[[nodiscard]] std::optional<std::filesystem::path> takePendingSaveScenePath(SceneFileDialogState& state);
void setSceneFileDialogStatus(SceneFileDialogState& state, std::string status);
[[nodiscard]] std::string sceneFileDialogStatus(SceneFileDialogState& state, uint64_t& statusVersion);
[[nodiscard]] bool sceneFileDialogIsOpen(SceneFileDialogState& state);

void showSaveSceneScreenshotDialog(SDL_Window* window, SceneScreenshotDialogState& state);
[[nodiscard]] std::optional<std::filesystem::path> takePendingSaveSceneScreenshotPath(
    SceneScreenshotDialogState& state);
void setSceneScreenshotDialogStatus(SceneScreenshotDialogState& state, std::string status);
[[nodiscard]] std::string sceneScreenshotDialogStatus(
    SceneScreenshotDialogState& state,
    uint64_t& statusVersion);
[[nodiscard]] bool sceneScreenshotDialogIsOpen(SceneScreenshotDialogState& state);

} // namespace woby
