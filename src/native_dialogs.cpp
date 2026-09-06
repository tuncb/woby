#include "native_dialogs.h"
#include "utf8_path.h"
#include "importer_host.h"

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
            selectedPaths.push_back(woby::pathFromUtf8(filelist[index]));
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

void SDLCALL modelFolderTreeDialogCallback(void* userdata, const char* const* filelist, int filter)
{
    (void)filter;

    auto* state = static_cast<ModelFileDialogState*>(userdata);
    std::vector<std::filesystem::path> selectedPaths;
    std::string status;
    bool showStatus = false;

    if (filelist == nullptr) {
        status = std::string("Open folder dialog failed: ") + SDL_GetError();
        showStatus = true;
    } else if (filelist[0] == nullptr) {
        status = "Open folder canceled";
        showStatus = true;
    } else {
        for (size_t index = 0; filelist[index] != nullptr; ++index) {
            selectedPaths.push_back(woby::pathFromUtf8(filelist[index]));
        }
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->pendingFolderTreeRoots.insert(
        state->pendingFolderTreeRoots.end(),
        selectedPaths.begin(),
        selectedPaths.end());
    if (showStatus) {
        state->status = std::move(status);
        ++state->statusVersion;
    }
    state->folderTreeOpen = false;
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
        selectedPath = woby::pathFromUtf8(filelist[0]);
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
        selectedPath = woby::pathFromUtf8(filelist[0]);
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
        selectedPath = woby::pathFromUtf8(filelist[0]);
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
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.open || state.folderTreeOpen) {
            return;
        }
        state.filterNames = {"3D Models", "Wavefront OBJ", "STL"};
        state.filterPatterns = {"obj;stl", "obj", "stl"};
        for (const auto& importer : loadedImporters()) {
            std::string pattern;
            for (const auto& extension : importer.extensions) {
                if (!pattern.empty()) { pattern += ';'; }
                pattern += extension;
                state.filterPatterns[0] += ';' + extension;
            }
            state.filterNames.push_back(importer.name);
            state.filterPatterns.push_back(std::move(pattern));
        }
        state.filterNames.push_back("All files");
        state.filterPatterns.push_back("*");
        state.filters.clear();
        for (size_t i = 0; i < state.filterNames.size(); ++i) {
            state.filters.push_back({state.filterNames[i].c_str(), state.filterPatterns[i].c_str()});
        }
        state.open = true;
    }

    SDL_ShowOpenFileDialog(
        modelFileDialogCallback,
        &state,
        window,
        state.filters.data(),
        static_cast<int>(state.filters.size()),
        nullptr,
        true);
}

void showModelFolderTreeDialog(SDL_Window* window, ModelFileDialogState& state)
{
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.open || state.folderTreeOpen) {
            return;
        }
        state.folderTreeOpen = true;
    }

    SDL_ShowOpenFolderDialog(
        modelFolderTreeDialogCallback,
        &state,
        window,
        nullptr,
        false);
}

std::vector<std::filesystem::path> takePendingModelPaths(ModelFileDialogState& state)
{
    std::vector<std::filesystem::path> paths;
    std::lock_guard<std::mutex> lock(state.mutex);
    paths.swap(state.pendingPaths);
    return paths;
}

std::vector<std::filesystem::path> takePendingModelFolderTreeRoots(ModelFileDialogState& state)
{
    std::vector<std::filesystem::path> paths;
    std::lock_guard<std::mutex> lock(state.mutex);
    paths.swap(state.pendingFolderTreeRoots);
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
    return state.open || state.folderTreeOpen;
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
