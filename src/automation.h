#pragma once

#include "command_line.h"
#include "scene_objects.h"

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

struct AutomationObjectsCommand {};

struct AutomationObjectCommand {
    SceneObjectId objectId = invalidSceneObjectId;
};

using AutomationCommandPayload = std::variant<AutomationScreenshotCommand, AutomationObjectsCommand, AutomationObjectCommand>;
using AutomationCommandId = uint64_t;
inline constexpr size_t maxAutomationCommands = 8;
inline constexpr size_t maxAutomationHistory = 128;
inline constexpr size_t maxAutomationHistoryBytes = 8 * 1024 * 1024;
inline constexpr size_t maxAutomationRequestKeys = 1024;

struct AutomationCommand {
    AutomationCommandId id = 0;
    AutomationCommandPayload payload;
};

struct AutomationScreenshotResult {
    std::filesystem::path savedPath;
};

struct AutomationCommandError {
    std::string message;
    int code = -32004;
};

struct AutomationObjectsResult {
    std::vector<SceneObjectInfo> objects;
};

struct AutomationObjectResult {
    SceneObjectInfo object;
};

using AutomationCommandResult = std::variant<AutomationScreenshotResult, AutomationObjectsResult, AutomationObjectResult, AutomationCommandError>;

// Main thread only, after UI/state updates and before rendering. Starts the oldest
// queued command whose deadline has not expired. Only one
// command executes at a time, until completion even after an HTTP timeout.
[[nodiscard]] std::optional<AutomationCommand> takeAutomationCommand(AutomationRuntime& runtime);
// Returns false for an unknown, unstarted, or completed command ID, or a result
// whose type does not match the command.
bool completeAutomationCommand(
    AutomationRuntime& runtime,
    AutomationCommandId id,
    const AutomationCommandResult& result);

[[nodiscard]] int runAutomationCommand(
    const ControlArguments& arguments,
    const std::filesystem::path& registryDirectory = {});
void printCommandLineHelp();

} // namespace woby
