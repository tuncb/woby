#include "command_line.h"

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

} // namespace

AppArguments parseCommandLine(int argc, char** argv)
{
    AppArguments arguments;
    bool logLevelSpecified = false;

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

        if (argument == "--file") {
            requireValue(argc, index, argument, "an OBJ filename");

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

    return arguments;
}

} // namespace woby
