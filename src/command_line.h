#pragma once

#include <cstddef>
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
    bool logPerformance = false;
    size_t logFrameInterval = 120u;
    std::optional<double> logSlowFrameMilliseconds;
    std::optional<std::filesystem::path> scenePath;
    std::vector<ModelPathOption> inputPaths;
};

AppArguments parseCommandLine(int argc, char** argv);

} // namespace woby
