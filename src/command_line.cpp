#include "command_line.h"
#include "utf8_path.h"

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

std::string parseInstanceId(int argc, char** argv, int& index)
{
    requireValue(argc, index, "--instance", "an ID");
    const std::string value = argv[++index];
    if (!validInstanceId(value)) {
        throw std::runtime_error("Instance IDs must be 1-64 lowercase letters, digits, hyphens or underscores, starting with a letter or digit.");
    }
    return value;
}

AppArguments parseControlArguments(int argc, char** argv)
{
    AppArguments arguments;
    auto& control = arguments.control;
    bool timeoutSpecified = false;
    bool waitSpecified = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            arguments.showHelp = true;
        } else if (argument == "--instance") {
            if (control.instanceId) {
                throw std::runtime_error("Only one --instance option can be specified.");
            }
            control.instanceId = parseInstanceId(argc, argv, index);
        } else if (argument == "--json") {
            if (control.json) {
                throw std::runtime_error("Only one --json option can be specified.");
            }
            control.json = true;
        } else if (argument == "--timeout") {
            requireValue(argc, index, argument, "a timeout in seconds");
            const size_t seconds = parsePositiveSize(argv[++index], argument);
            if (timeoutSpecified || seconds > 3600u) {
                throw std::runtime_error("Specify --timeout once, with a value from 1 to 3600 seconds.");
            }
            timeoutSpecified = true;
            control.timeoutSeconds = static_cast<int>(seconds);
        } else if (argument == "--wait") {
            if (waitSpecified) {
                throw std::runtime_error("Only one --wait option can be specified.");
            }
            waitSpecified = true;
        } else if (control.command == ControlCommand::none && argument == "instances") {
            control.command = ControlCommand::instances;
        } else if (control.command == ControlCommand::none && argument == "screenshot") {
            control.command = ControlCommand::screenshot;
        } else if (control.command == ControlCommand::screenshot && control.outputPath.empty()
                   && !argument.empty() && argument.front() != '-') {
            control.outputPath = pathFromUtf8(argument);
        } else {
            throw std::runtime_error("Unexpected control argument: " + argument);
        }
    }
    if (arguments.showHelp) {
        return arguments;
    }
    if (control.command == ControlCommand::none) {
        throw std::runtime_error("Expected 'ctl instances' or 'ctl --instance ID screenshot PATH'.");
    }
    if (control.command == ControlCommand::instances && (control.instanceId || timeoutSpecified || waitSpecified)) {
        throw std::runtime_error("ctl instances only accepts --json.");
    }
    if (control.command == ControlCommand::screenshot && (!control.instanceId || control.outputPath.empty())) {
        throw std::runtime_error("Screenshot requires --instance ID and an output path.");
    }
    return arguments;
}

} // namespace

bool validInstanceId(const std::string& value)
{
    const auto alphanumeric = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    };
    if (value.empty() || value.size() > 64u || !alphanumeric(value.front())) {
        return false;
    }
    for (const char ch : value) {
        if (!alphanumeric(ch) && ch != '-' && ch != '_') {
            return false;
        }
    }
    return true;
}

AppArguments parseCommandLine(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "ctl") {
        return parseControlArguments(argc, argv);
    }
    AppArguments arguments;
    bool logLevelSpecified = false;
    bool logPerformanceSpecified = false;
    bool logFrameIntervalSpecified = false;
    bool logSlowFrameSpecified = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];

        if (argument == "--help" || argument == "-h") {
            arguments.showHelp = true;
            continue;
        }
        if (argument == "--instance") {
            if (arguments.instanceId) {
                throw std::runtime_error("Only one --instance option can be specified.");
            }
            arguments.instanceId = parseInstanceId(argc, argv, index);
            continue;
        }

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

        if (argument == "--plugin" || argument == "--plugin-folder") {
            requireValue(argc, index, argument, "a plugin path");
            const std::string value = argv[++index];
            if (value.empty() || value.rfind("--", 0u) == 0u) {
                throw std::runtime_error(argument + " requires a plugin path.");
            }
            arguments.pluginPaths.push_back({argument == "--plugin-folder", woby::pathFromUtf8(value)});
            continue;
        }

        if (argument == "--file") {
            requireValue(argc, index, argument, "a model filename");

            ModelPathOption inputPath;
            inputPath.path = woby::pathFromUtf8(argv[++index]);
            arguments.inputPaths.push_back(std::move(inputPath));
            continue;
        }

        if (argument == "--woby" || argument == "--scene") {
            requireValue(argc, index, argument, "a woby scene filename");
            if (arguments.scenePath.has_value()) {
                throw std::runtime_error("Only one woby scene file can be specified.");
            }

            arguments.scenePath = woby::pathFromUtf8(argv[++index]);
            continue;
        }

        if (argument == "--folder") {
            requireValue(argc, index, argument, "a folder path");

            ModelPathOption inputPath;
            inputPath.folder = true;
            inputPath.path = woby::pathFromUtf8(argv[++index]);
            arguments.inputPaths.push_back(std::move(inputPath));
            continue;
        }

        if (argument == "--folder-tree") {
            requireValue(argc, index, argument, "a folder path");

            ModelPathOption inputPath;
            inputPath.folder = true;
            inputPath.folderTree = true;
            inputPath.path = woby::pathFromUtf8(argv[++index]);
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
