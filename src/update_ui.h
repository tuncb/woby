#pragma once

#include "update.h"
#include <future>
#include <thread>

namespace woby {

// Session-only updater/adaptor state; never part of a saved scene.
struct UpdateUiState {
    std::string currentVersion;
    std::string latestVersion;
    std::string message;
    bool managedDeployment = false;
    bool available = false;
    bool closeRequested = false;
    UpdateCommand activeCommand = UpdateCommand::none;
};

struct UpdateUiRuntime {
    UpdateUiState state;
    std::future<UpdateResult> result;
    // Destroyed first: requests cancellation and joins before the future/state disappear.
    std::jthread worker;
};

[[nodiscard]] bool updateBusy(const UpdateUiState& state);
[[nodiscard]] bool canInstallUpdate(const UpdateUiState& state, bool sceneDirty);
void applyUpdateResult(UpdateUiState& state, const UpdateResult& result);
void startUiUpdate(UpdateUiRuntime& runtime, UpdateCommand command, bool sceneDirty,
    DeploymentOwner& deploymentGuard);
void pollUiUpdate(UpdateUiRuntime& runtime);

} // namespace woby
