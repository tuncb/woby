#include "command_line.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

namespace woby {
namespace {

LogLevel parseLogLevel(const std::string& value)
{
    if (value == "off") {
        return LogLevel::off;
    }
    if (value == "trace") {
        return LogLevel::trace;
    }
    if (value == "debug") {
        return LogLevel::debug;
    }
    if (value == "info") {
        return LogLevel::info;
    }
    if (value == "warn") {
        return LogLevel::warn;
    }
    if (value == "error") {
        return LogLevel::error;
    }
    if (value == "critical") {
        return LogLevel::critical;
    }

    throw std::runtime_error(
        "Invalid --log-level value. Expected one of: off, trace, debug, info, warn, error, critical.");
}

void requireValue(int argc, int index, const std::string& option, const std::string& message)
{
    if (index + 1 >= argc) {
        throw std::runtime_error(option + " requires " + message + ".");
    }
}

size_t parsePositiveSize(const std::string& value, const std::string& option)
{
    if (!value.empty() && (value.front() == '-' || value.front() == '+')) {
        throw std::runtime_error(option + " requires a positive integer.");
    }

    size_t parsedCharacters = 0;
    size_t result = 0;
    try {
        result = static_cast<size_t>(std::stoull(value, &parsedCharacters));
    } catch (const std::exception&) {
        throw std::runtime_error(option + " requires a positive integer.");
    }

    if (parsedCharacters != value.size() || result == 0u) {
        throw std::runtime_error(option + " requires a positive integer.");
    }

    return result;
}

double parsePositiveDouble(const std::string& value, const std::string& option)
{
    size_t parsedCharacters = 0;
    double result = 0.0;
    try {
        result = std::stod(value, &parsedCharacters);
    } catch (const std::exception&) {
        throw std::runtime_error(option + " requires a positive number.");
    }

    if (parsedCharacters != value.size() || !std::isfinite(result) || result <= 0.0) {
        throw std::runtime_error(option + " requires a positive number.");
    }

    return result;
}

bool writesInfoLogs(LogLevel level)
{
    return level == LogLevel::trace || level == LogLevel::debug || level == LogLevel::info;
}

} // namespace

AppArguments parseCommandLine(int argc, char** argv)
{
    AppArguments arguments;
    bool logLevelSpecified = false;
    bool logPerformanceSpecified = false;
    bool logFrameIntervalSpecified = false;
    bool logSlowFrameSpecified = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--version") {
            arguments.showVersion = true;
            continue;
        }

        if (argument == "--log-level") {
            requireValue(argc, index, argument, "a level");
            if (logLevelSpecified) {
                throw std::runtime_error("Only one --log-level option can be specified.");
            }

            arguments.logLevel = parseLogLevel(argv[++index]);
            logLevelSpecified = true;
            continue;
        }

        if (argument == "--log-file") {
            requireValue(argc, index, argument, "a file path");
            if (arguments.logFile.has_value()) {
                throw std::runtime_error("Only one --log-file option can be specified.");
            }

            arguments.logFile = argv[++index];
            continue;
        }

        if (argument == "--log-performance") {
            if (logPerformanceSpecified) {
                throw std::runtime_error("Only one --log-performance option can be specified.");
            }

            arguments.logPerformance = true;
            logPerformanceSpecified = true;
            continue;
        }

        if (argument == "--log-frame-interval") {
            requireValue(argc, index, argument, "a frame count");
            if (logFrameIntervalSpecified) {
                throw std::runtime_error("Only one --log-frame-interval option can be specified.");
            }

            arguments.logFrameInterval = parsePositiveSize(argv[++index], argument);
            logFrameIntervalSpecified = true;
            continue;
        }

        if (argument == "--log-slow-frame-ms") {
            requireValue(argc, index, argument, "a millisecond threshold");
            if (logSlowFrameSpecified) {
                throw std::runtime_error("Only one --log-slow-frame-ms option can be specified.");
            }

            arguments.logSlowFrameMilliseconds = parsePositiveDouble(argv[++index], argument);
            logSlowFrameSpecified = true;
            continue;
        }

        if (argument == "--file") {
            requireValue(argc, index, argument, "a model filename");

            ModelPathOption inputPath;
            inputPath.path = argv[++index];
            arguments.inputPaths.push_back(std::move(inputPath));
            continue;
        }

        if (argument == "--woby" || argument == "--scene") {
            requireValue(argc, index, argument, "a woby scene filename");
            if (arguments.scenePath.has_value()) {
                throw std::runtime_error("Only one woby scene file can be specified.");
            }

            arguments.scenePath = argv[++index];
            continue;
        }

        if (argument == "--folder") {
            requireValue(argc, index, argument, "a folder path");

            ModelPathOption inputPath;
            inputPath.folder = true;
            inputPath.path = argv[++index];
            arguments.inputPaths.push_back(std::move(inputPath));
            continue;
        }

        if (argument.rfind("--", 0) == 0) {
            throw std::runtime_error("Unknown option: " + argument);
        }

        throw std::runtime_error("Unexpected argument: " + argument);
    }

    if (arguments.logLevel != LogLevel::off && !arguments.logFile.has_value()) {
        throw std::runtime_error("--log-file is required when --log-level is not off.");
    }
    if (arguments.logLevel == LogLevel::off && arguments.logFile.has_value()) {
        throw std::runtime_error("--log-file requires --log-level to be set to a non-off level.");
    }
    if (arguments.logPerformance && !writesInfoLogs(arguments.logLevel)) {
        throw std::runtime_error("--log-performance requires --log-level to be trace, debug, or info.");
    }
    if (logFrameIntervalSpecified && !arguments.logPerformance) {
        throw std::runtime_error("--log-frame-interval requires --log-performance.");
    }
    if (logSlowFrameSpecified && !arguments.logPerformance) {
        throw std::runtime_error("--log-slow-frame-ms requires --log-performance.");
    }

    return arguments;
}

} // namespace woby
