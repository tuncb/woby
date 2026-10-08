#pragma once

#include "recent_files.h"
#include <imgui.h>

namespace woby {
struct UiState;
struct AnnotationInteraction;

enum class MainMenuCommand {
    none, newScene, openScene, saveScene, saveSceneAs, addModels, addModelFolder,
    exportPng, exit, undo, redo, settings, checkUpdates,
    openRecentScene, addRecentModel, removeRecentScene, removeRecentModel, clearRecentFiles
};

struct MainMenuAvailability {
    bool fileActionsDisabled = false;
    bool capturePending = false;
    bool canUndo = false;
    bool canRedo = false;
    bool updateBusy = false;
};

struct MainMenuResult {
    MainMenuCommand command = MainMenuCommand::none;
    ImVec2 popupPosition;
    std::filesystem::path recentPath;
};

MainMenuResult drawMainMenu(UiState& state, AnnotationInteraction& annotation,
    const MainMenuAvailability& availability, const RecentFiles& recentFiles = {});
} // namespace woby
