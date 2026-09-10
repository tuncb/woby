#include "update_ui.h"

#include <chrono>
#include <stdexcept>

namespace woby {

bool updateBusy(const UpdateUiState& state)
{
    return state.activeCommand != UpdateCommand::none;
}

bool canInstallUpdate(const UpdateUiState& state, bool sceneDirty)
{
    return state.managedDeployment && state.available && !sceneDirty
        && !updateBusy(state) && !state.closeRequested;
}

void applyUpdateResult(UpdateUiState& state, const UpdateResult& result)
{
    const bool installing = state.activeCommand == UpdateCommand::install;
    const bool checked = state.activeCommand == UpdateCommand::check && result.exitCode == 0;
    state.activeCommand = UpdateCommand::none;
    state.message = checked ? std::string{} : result.data.at("message").get<std::string>();
    state.latestVersion = result.data.value("latest", state.latestVersion);
    state.available = result.exitCode == 0 && result.data.value("updateAvailable", false);
    state.closeRequested = installing && result.exitCode == 2 && result.data.value("state", "") == "pending";
}

void startUiUpdate(UpdateUiRuntime& runtime, UpdateCommand command, bool sceneDirty,
    DeploymentOwner& deploymentGuard)
{
    auto& state = runtime.state;
    if (updateBusy(state) || state.closeRequested) { return; }
    if (command != UpdateCommand::check && command != UpdateCommand::install) { return; }
    if (command == UpdateCommand::install && !canInstallUpdate(state, sceneDirty)) { return; }
    try {
        if (command == UpdateCommand::install) {
            acquireViewerUpdateLock(updateExecutablePath().parent_path(), deploymentGuard);
        }
        std::packaged_task<UpdateResult(std::stop_token)> task(
            [command, version = state.currentVersion, &deploymentGuard](std::stop_token cancellation) {
                return executeUpdate({command, true}, version,
                    command == UpdateCommand::install ? &deploymentGuard : nullptr, cancellation);
            });
        runtime.result = task.get_future();
        runtime.worker = std::jthread(std::move(task));
        state.activeCommand = command;
        state.available = false;
        state.message = command == UpdateCommand::check ? "Checking for updates..."
            : "Downloading and verifying update... Woby will close when ready.";
    } catch (const std::exception& error) {
        // Losing both locks means another updater could now change the deployment.
        if (command == UpdateCommand::install && !deploymentGuard) { throw; }
        state.message = error.what();
    }
}

void pollUiUpdate(UpdateUiRuntime& runtime)
{
    if (!updateBusy(runtime.state) || !runtime.result.valid()
        || runtime.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return; }
    try { applyUpdateResult(runtime.state, runtime.result.get()); }
    catch (const std::exception& error) {
        runtime.state.activeCommand = UpdateCommand::none;
        runtime.state.available = false;
        runtime.state.message = error.what();
    }
}

} // namespace woby
