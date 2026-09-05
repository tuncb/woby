#pragma once

#include "command_line.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace woby {

// Application/runtime metadata: never part of a saved scene or UiState.
struct AutomationRuntime;
using AutomationOwner = std::unique_ptr<AutomationRuntime, void (*)(AutomationRuntime*)>;

[[nodiscard]] AutomationOwner startAutomation(
    const std::optional<std::string>& instanceId,
    const std::filesystem::path& registryDirectory = {});
void stopAutomation(AutomationRuntime* runtime);
[[nodiscard]] const std::string& automationInstanceId(const AutomationRuntime& runtime);
void setAutomationReady(AutomationRuntime& runtime);

// Called only by the main thread. At most one capture is outstanding.
[[nodiscard]] std::optional<std::filesystem::path> takeAutomationScreenshot(AutomationRuntime& runtime);
void completeAutomationScreenshot(
    AutomationRuntime& runtime,
    const std::filesystem::path& savedPath,
    const std::string& error = {});

[[nodiscard]] int runAutomationCommand(
    const ControlArguments& arguments,
    const std::filesystem::path& registryDirectory = {});
void printCommandLineHelp();

} // namespace woby
