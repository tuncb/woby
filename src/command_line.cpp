#include "command_line.h"
#include "utf8_path.h"

#include <algorithm>
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
    if (argc > 2 && std::string(argv[2]) == "help") {
        arguments.showHelp = true;
        return arguments;
    }
    for (int index = 2; index < argc; ++index) {
        if (std::string(argv[index]) == "--help" || std::string(argv[index]) == "-h") {
            arguments.showHelp = true;
            return arguments;
        }
    }
    if (parseExtendedControlArguments(argc, argv, arguments.control)) { return arguments; }
    auto& control = arguments.control;
    bool timeoutSpecified = false;
    bool waitSpecified = false;
    bool sceneActionSpecified = false;
    bool dirtySpecified = false;
    bool savePathSpecified = false;
    bool overwriteSpecified = false;
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
        } else if (argument == "--request-key") {
            requireValue(argc, index, argument, "a request key");
            const std::string key = argv[++index];
            if (control.requestKey || !validAutomationRequestKey(key)) {
                throw std::runtime_error("Specify --request-key once, with 1-128 ASCII letters, digits, '-', '_', '.', or ':'.");
            }
            control.requestKey = key;
        } else if (argument == "--on-dirty") {
            requireValue(argc, index, argument, "error, save, or discard");
            const std::string value = argv[++index];
            if (dirtySpecified || (value != "error" && value != "save" && value != "discard")) {
                throw std::runtime_error("Specify --on-dirty once: error, save, or discard.");
            }
            dirtySpecified = true;
            control.lifecycle.onDirty = value == "save" ? DirtyPolicy::save : value == "discard" ? DirtyPolicy::discard : DirtyPolicy::error;
        } else if (argument == "--save-path") {
            requireValue(argc, index, argument, "a filename");
            if (savePathSpecified) { throw std::runtime_error("Specify --save-path once."); }
            savePathSpecified = true;
            control.lifecycle.savePath = pathFromUtf8(argv[++index]);
            if (control.lifecycle.savePath.empty()) { throw std::runtime_error("--save-path requires a filename."); }
        } else if (argument == "--overwrite") {
            if (overwriteSpecified) { throw std::runtime_error("Specify --overwrite once."); }
            overwriteSpecified = true;
            control.lifecycle.overwrite = true;
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
        } else if (control.command == ControlCommand::none && argument == "scene") {
            control.command = ControlCommand::scene;
        } else if (control.command == ControlCommand::none && argument == "quit") {
            control.command = ControlCommand::quit;
            control.lifecycle.action = SceneAction::quit;
        } else if (control.command == ControlCommand::scene && !sceneActionSpecified) {
            if (argument == "save") { control.lifecycle.action = SceneAction::save; }
            else if (argument == "save-as") { control.lifecycle.action = SceneAction::saveAs; }
            else if (argument == "open") { control.lifecycle.action = SceneAction::open; }
            else if (argument == "new") { control.lifecycle.action = SceneAction::newScene; }
            else { throw std::runtime_error("Expected scene save, save-as, open, or new."); }
            sceneActionSpecified = true;
        } else if (control.command == ControlCommand::scene && control.lifecycle.path.empty()
            && (control.lifecycle.action == SceneAction::saveAs || control.lifecycle.action == SceneAction::open)
            && !argument.empty() && argument.front() != '-') {
            control.lifecycle.path = pathFromUtf8(argument);
        } else if (control.command == ControlCommand::none && argument == "instances") {
            control.command = ControlCommand::instances;
        } else if (control.command == ControlCommand::none && argument == "screenshot") {
            control.command = ControlCommand::screenshot;
        } else if (control.command == ControlCommand::none && argument == "objects") {
            control.command = ControlCommand::objects;
        } else if (control.command == ControlCommand::none && argument == "object") {
            control.command = ControlCommand::object;
        } else if (control.command == ControlCommand::none && argument == "command") {
            control.command = ControlCommand::command;
        } else if (control.command == ControlCommand::command && control.commandId.empty()
                   && !argument.empty() && argument.front() != '-') {
            control.commandId = argument;
        } else if (control.command == ControlCommand::object && control.objectId.empty()
                   && !argument.empty() && argument.front() != '-') {
            control.objectId = argument;
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
    const bool lifecycle = control.command == ControlCommand::scene || control.command == ControlCommand::quit;
    const bool destructive = lifecycle && (control.lifecycle.action == SceneAction::open
        || control.lifecycle.action == SceneAction::newScene || control.lifecycle.action == SceneAction::quit);
    if ((dirtySpecified || savePathSpecified) && !destructive) {
        throw std::runtime_error("Dirty policies apply only to scene open, scene new, and quit.");
    }
    if (savePathSpecified && control.lifecycle.onDirty != DirtyPolicy::save) {
        throw std::runtime_error("--save-path requires --on-dirty save.");
    }
    if (overwriteSpecified && (!lifecycle || (control.lifecycle.action != SceneAction::saveAs && !savePathSpecified))) {
        throw std::runtime_error("--overwrite requires an explicit save destination.");
    }
    if (lifecycle && (!control.instanceId || (control.command == ControlCommand::scene && !sceneActionSpecified)
        || ((control.lifecycle.action == SceneAction::open || control.lifecycle.action == SceneAction::saveAs) && control.lifecycle.path.empty()))) {
        throw std::runtime_error("Scene commands require --instance ID, a scene action, and a path for open/save-as.");
    }
    if (control.command == ControlCommand::none) {
        throw std::runtime_error("Expected ctl instances, screenshot, objects, object, or command. See --help.");
    }
    if (control.command == ControlCommand::instances && (control.instanceId || timeoutSpecified || waitSpecified || control.requestKey)) {
        throw std::runtime_error("ctl instances only accepts --json.");
    }
    if (control.command == ControlCommand::screenshot && (!control.instanceId || control.outputPath.empty())) {
        throw std::runtime_error("Screenshot requires --instance ID and an output path.");
    }
    if ((control.command == ControlCommand::objects || control.command == ControlCommand::object) && !control.instanceId) {
        throw std::runtime_error("Object queries require --instance ID.");
    }
    if (control.command == ControlCommand::object && control.objectId.empty()) {
        throw std::runtime_error("Object query requires an object ID from 'ctl objects'.");
    }
    if (control.command == ControlCommand::command
        && (!control.instanceId || (!control.commandId.empty() == control.requestKey.has_value())
            || timeoutSpecified || waitSpecified)) {
        throw std::runtime_error("Command lookup requires --instance ID and either COMMAND_ID or --request-key KEY; it does not wait.");
    }
    return arguments;
}

} // namespace

bool validAutomationRequestKey(const std::string& value)
{
    return !value.empty() && value.size() <= 128u && std::all_of(value.begin(), value.end(), [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
            || ch == '-' || ch == '_' || ch == '.' || ch == ':';
    });
}

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
    if (argc > 1 && std::string(argv[1]) == "help") {
        AppArguments arguments;
        arguments.showHelp = true;
        return arguments;
    }
    if (argc > 1 && std::string(argv[1]) == "update") {
        AppArguments arguments;
        arguments.update.command = UpdateCommand::install;
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--help" || argument == "-h") { arguments.showHelp = true; }
            else if (argument == "--json" && !arguments.update.json) { arguments.update.json = true; }
            else if ((argument == "--check" || argument == "--status") && arguments.update.command == UpdateCommand::install) {
                arguments.update.command = argument == "--check" ? UpdateCommand::check : UpdateCommand::status;
            } else { throw std::runtime_error("Unexpected update argument: " + argument + ". Use update [--check | --status] [--json]."); }
        }
        return arguments;
    }
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
