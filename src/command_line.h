#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace woby {

enum class LogLevel {
    off,
    trace,
    debug,
    info,
    warn,
    error,
    critical,
};

struct ModelPathOption {
    bool folder = false;
    bool folderTree = false;
    std::filesystem::path path;
};

struct PluginPathOption {
    bool folder = false;
    std::filesystem::path path;
};

enum class ControlCommand {
    none,
    instances,
    screenshot,
    objects,
    object,
};

struct ControlArguments {
    ControlCommand command = ControlCommand::none;
    std::optional<std::string> instanceId;
    std::filesystem::path outputPath;
    std::string objectId;
    int timeoutSeconds = 60;
    bool json = false;
};

struct AppArguments {
    bool showHelp = false;
    bool showVersion = false;
    std::optional<std::string> instanceId;
    ControlArguments control;
    LogLevel logLevel = LogLevel::off;
    std::optional<std::filesystem::path> logFile;
    bool logPerformance = false;
    size_t logFrameInterval = 120u;
    std::optional<double> logSlowFrameMilliseconds;
    std::optional<std::filesystem::path> scenePath;
    std::vector<ModelPathOption> inputPaths;
    std::vector<PluginPathOption> pluginPaths;
};

AppArguments parseCommandLine(int argc, char** argv);
[[nodiscard]] bool validInstanceId(const std::string& value);

} // namespace woby
