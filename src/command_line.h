#pragma once

#include <filesystem>
#include <optional>
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
    std::filesystem::path path;
};

struct AppArguments {
    bool showVersion = false;
    LogLevel logLevel = LogLevel::off;
    std::optional<std::filesystem::path> logFile;
    std::optional<std::filesystem::path> scenePath;
    std::vector<ModelPathOption> inputPaths;
};

AppArguments parseCommandLine(int argc, char** argv);

} // namespace woby
