#pragma once

#include "command_line.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

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

struct AutomationScreenshotCommand {
    std::filesystem::path outputPath;
};

using AutomationCommandPayload = std::variant<AutomationScreenshotCommand>;
using AutomationCommandId = uint64_t;

struct AutomationCommand {
    AutomationCommandId id = 0;
    AutomationCommandPayload payload;
};

struct AutomationScreenshotResult {
    std::filesystem::path savedPath;
};

struct AutomationCommandError {
    std::string message;
};

using AutomationCommandResult = std::variant<AutomationScreenshotResult, AutomationCommandError>;

// Called only by the main thread. At most one command is outstanding. Taking a
// command starts it; its slot remains reserved until completion, even after timeout.
[[nodiscard]] std::optional<AutomationCommand> takeAutomationCommand(AutomationRuntime& runtime);
// Returns false for an unknown, unstarted, or already completed command ID.
bool completeAutomationCommand(
    AutomationRuntime& runtime,
    AutomationCommandId id,
    const AutomationCommandResult& result);

[[nodiscard]] int runAutomationCommand(
    const ControlArguments& arguments,
    const std::filesystem::path& registryDirectory = {});
void printCommandLineHelp();

} // namespace woby
