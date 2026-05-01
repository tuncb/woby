#include "native_dialogs.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#include <iterator>
#include <utility>

namespace woby {
namespace {

void SDLCALL modelFileDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<ModelFileDialogState*>(userdata);
    std::vector<std::filesystem::path> selectedPaths;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Open dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Open canceled";
        showStatus = true;
    } else {
        for (size_t index = 0; filelist[index] != nullptr; ++index) {
            selectedPaths.emplace_back(filelist[index]);
        }
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingPaths.insert(
        state->pendingPaths.end(),
        selectedPaths.begin(),
        selectedPaths.end());
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->open = false;
}

void SDLCALL openSceneDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<SceneFileDialogState*>(userdata);
    std::optional<std::filesystem::path> selectedPath;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Open scene dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Open scene canceled";
        showStatus = true;
    } else {
        selectedPath = std::filesystem::path(filelist[0]);
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingOpenPath = std::move(selectedPath);
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->openDialogOpen = false;
}

void SDLCALL saveSceneDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<SceneFileDialogState*>(userdata);
    std::optional<std::filesystem::path> selectedPath;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Save scene dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Save scene canceled";
        showStatus = true;
    } else {
        selectedPath = std::filesystem::path(filelist[0]);
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingSavePath = std::move(selectedPath);
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->saveDialogOpen = false;
}

void SDLCALL saveSceneScreenshotDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<SceneScreenshotDialogState*>(userdata);
    std::optional<std::filesystem::path> selectedPath;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Save screenshot dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Save screenshot canceled";
        showStatus = true;
    } else {
        selectedPath = std::filesystem::path(filelist[0]);
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingSavePath = std::move(selectedPath);
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->saveDialogOpen = false;
}

} // namespace

void showModelFileDialog(SDL_Window* window, ModelFileDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"3D Models", "obj;stl"},
        {"Wavefront OBJ", "obj"},
        {"STL", "stl"},
        {"All files", "*"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.open) {
            return;
        }
        state.open = true;
    }

    SDL_ShowOpenFileDialog(
        modelFileDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr,
        true);
}

std::vector<std::filesystem::path> takePendingModelPaths(ModelFileDialogState& state)
{
    std::vector<std::filesystem::path> paths;
    std::lock_guard<std::mutex> lock(state.mutex);
    paths.swap(state.pendingPaths);
    return paths;
}

void setModelFileDialogStatus(ModelFileDialogState& state, std::string status)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    ++state.statusVersion;
}

std::string modelFileDialogStatus(ModelFileDialogState& state, uint64_t& statusVersion)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.statusVersion == statusVersion) {
        return {};
    }

    statusVersion = state.statusVersion;
    return state.status;
}

bool modelFileDialogIsOpen(ModelFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.open;
}

void showOpenSceneDialog(SDL_Window* window, SceneFileDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"woby scene", "woby"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.openDialogOpen) {
            return;
        }
        state.openDialogOpen = true;
    }

    SDL_ShowOpenFileDialog(
        openSceneDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr,
        false);
}

void showSaveSceneDialog(SDL_Window* window, SceneFileDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"woby scene", "woby"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.saveDialogOpen) {
            return;
        }
        state.saveDialogOpen = true;
    }

    SDL_ShowSaveFileDialog(
        saveSceneDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr);
}

std::optional<std::filesystem::path> takePendingOpenScenePath(SceneFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    std::optional<std::filesystem::path> path = std::move(state.pendingOpenPath);
    state.pendingOpenPath.reset();
    return path;
}

std::optional<std::filesystem::path> takePendingSaveScenePath(SceneFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    std::optional<std::filesystem::path> path = std::move(state.pendingSavePath);
    state.pendingSavePath.reset();
    return path;
}

void setSceneFileDialogStatus(SceneFileDialogState& state, std::string status)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    ++state.statusVersion;
}

std::string sceneFileDialogStatus(SceneFileDialogState& state, uint64_t& statusVersion)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.statusVersion == statusVersion) {
        return {};
    }

    statusVersion = state.statusVersion;
    return state.status;
}

bool sceneFileDialogIsOpen(SceneFileDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.openDialogOpen || state.saveDialogOpen;
}

void showSaveSceneScreenshotDialog(SDL_Window* window, SceneScreenshotDialogState& state)
{
    static constexpr SDL_DialogFileFilter filters[] = {
        {"PNG image", "png"},
    };

    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.saveDialogOpen) {
            return;
        }
        state.saveDialogOpen = true;
    }

    SDL_ShowSaveFileDialog(
        saveSceneScreenshotDialogCallback,
        &state,
        window,
        filters,
        static_cast<int>(std::size(filters)),
        nullptr);
}

std::optional<std::filesystem::path> takePendingSaveSceneScreenshotPath(SceneScreenshotDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    std::optional<std::filesystem::path> path = std::move(state.pendingSavePath);
    state.pendingSavePath.reset();
    return path;
}

void setSceneScreenshotDialogStatus(SceneScreenshotDialogState& state, std::string status)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.status = std::move(status);
    ++state.statusVersion;
}

std::string sceneScreenshotDialogStatus(SceneScreenshotDialogState& state, uint64_t& statusVersion)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.statusVersion == statusVersion) {
        return {};
    }

    statusVersion = state.statusVersion;
    return state.status;
}

bool sceneScreenshotDialogIsOpen(SceneScreenshotDialogState& state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.saveDialogOpen;
}

} // namespace woby
