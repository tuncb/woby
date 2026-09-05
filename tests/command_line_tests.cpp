#include "command_line.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

woby::AppArguments parse(std::vector<std::string> arguments)
{
    std::vector<char*> argv;
    argv.reserve(arguments.size());
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }

    return woby::parseCommandLine(static_cast<int>(argv.size()), argv.data());
}

struct GeneratedLogLevel {
    const char* name;
    woby::LogLevel value;
    bool supportsPerformance;
};

} // namespace

TEST_CASE("command line defaults keep logging off")
{
    const woby::AppArguments arguments = parse({"woby"});

    CHECK_FALSE(arguments.showVersion);
    CHECK(arguments.logLevel == woby::LogLevel::off);
    CHECK_FALSE(arguments.logFile.has_value());
    CHECK_FALSE(arguments.logPerformance);
    CHECK(arguments.logFrameInterval == 120u);
    CHECK_FALSE(arguments.logSlowFrameMilliseconds.has_value());
    CHECK_FALSE(arguments.scenePath.has_value());
    CHECK(arguments.inputPaths.empty());
    CHECK_FALSE(arguments.instanceId.has_value());
    CHECK(arguments.control.command == woby::ControlCommand::none);
}

TEST_CASE("instance IDs are explicit unique names with portable spelling")
{
    const auto arguments = parse({"woby", "--instance", "review-01", "--file", "model.obj"});
    REQUIRE(arguments.instanceId.has_value());
    CHECK(*arguments.instanceId == "review-01");
    CHECK(arguments.inputPaths.size() == 1u);
    for (const std::string id : {"", "Main", "../main", "has space", "-main", "a.b", "a/b", "a\\b"}) {
        CHECK_FALSE(woby::validInstanceId(id));
        CHECK_THROWS_AS(parse({"woby", "--instance", id}), std::runtime_error);
    }
    CHECK(woby::validInstanceId("main_2"));
    CHECK(woby::validInstanceId(std::string(64u, 'a')));
    CHECK_FALSE(woby::validInstanceId(std::string(65u, 'a')));
    CHECK_THROWS_AS(parse({"woby", "--instance"}), std::runtime_error);
    CHECK_THROWS_AS(parse({"woby", "--instance", "a", "--instance", "b"}), std::runtime_error);
}

TEST_CASE("control CLI supports discovery and synchronous screenshots")
{
    const auto list = parse({"woby", "ctl", "instances", "--json"});
    CHECK(list.control.command == woby::ControlCommand::instances);
    CHECK(list.control.json);
    const auto capture = parse({"woby", "ctl", "--instance", "review", "screenshot", "folder/view.png", "--wait", "--timeout", "120", "--json"});
    CHECK(capture.control.command == woby::ControlCommand::screenshot);
    CHECK(capture.control.instanceId == "review");
    CHECK(capture.control.outputPath == "folder/view.png");
    CHECK(capture.control.timeoutSeconds == 120);
    CHECK(capture.control.json);
    const auto reordered = parse({"woby", "ctl", "screenshot", "view.png", "--instance", "main"});
    CHECK(reordered.control.timeoutSeconds == 60);
    const std::string unicodePath = "folder/\xc3\xbc-\xe6\xb5\x8b\xe8\xaf\x95.png";
    const auto unicode = parse({"woby", "ctl", "--instance", "main", "screenshot", unicodePath});
    const auto encodedPath = unicode.control.outputPath.u8string();
    CHECK(std::string(encodedPath.begin(), encodedPath.end()) == unicodePath);
    CHECK(parse({"woby", "--help"}).showHelp);
    CHECK(parse({"woby", "ctl", "--help"}).showHelp);
}

TEST_CASE("control CLI rejects ambiguous and incomplete commands")
{
    const std::vector<std::vector<std::string>> cases = {
        {"woby", "ctl"},
        {"woby", "ctl", "screenshot", "view.png"},
        {"woby", "ctl", "--instance", "main", "screenshot"},
        {"woby", "ctl", "instances", "--instance", "main"},
        {"woby", "ctl", "instances", "--wait"},
        {"woby", "ctl", "instances", "--timeout", "2"},
        {"woby", "ctl", "instances", "--json", "--json"},
        {"woby", "ctl", "instances", "--file", "a.obj"},
        {"woby", "ctl", "--instance", "main", "screenshot", "a.png", "b.png"},
        {"woby", "ctl", "--instance", "main", "--instance", "other", "screenshot", "a.png"},
        {"woby", "ctl", "--instance", "main", "screenshot", "a.png", "--timeout", "0"},
        {"woby", "ctl", "--instance", "main", "screenshot", "a.png", "--timeout", "3601"},
        {"woby", "ctl", "--instance", "main", "screenshot", "a.png", "--timeout", "1.5"},
        {"woby", "ctl", "--instance", "main", "screenshot", "a.png", "--timeout", "1", "--timeout", "2"},
    };
    for (const auto& arguments : cases) {
        CHECK_THROWS_AS(parse(arguments), std::runtime_error);
    }
}

TEST_CASE("command line parses scene model and logging options")
{
    const woby::AppArguments arguments = parse({
        "woby",
        "--scene",
        "scene.woby",
        "--file",
        "a.obj",
        "--folder",
        "models",
        "--folder-tree",
        "tree_models",
        "--log-level",
        "info",
        "--log-file",
        "woby.log",
        "--log-performance",
        "--log-frame-interval",
        "60",
        "--log-slow-frame-ms",
        "20.5",
    });

    REQUIRE(arguments.scenePath.has_value());
    CHECK(arguments.scenePath.value() == "scene.woby");
    CHECK(arguments.logLevel == woby::LogLevel::info);
    REQUIRE(arguments.logFile.has_value());
    CHECK(arguments.logFile.value() == "woby.log");
    CHECK(arguments.logPerformance);
    CHECK(arguments.logFrameInterval == 60u);
    REQUIRE(arguments.logSlowFrameMilliseconds.has_value());
    CHECK(arguments.logSlowFrameMilliseconds.value() == 20.5);

    REQUIRE(arguments.inputPaths.size() == 3u);
    CHECK_FALSE(arguments.inputPaths[0].folder);
    CHECK_FALSE(arguments.inputPaths[0].folderTree);
    CHECK(arguments.inputPaths[0].path == "a.obj");
    CHECK(arguments.inputPaths[1].folder);
    CHECK_FALSE(arguments.inputPaths[1].folderTree);
    CHECK(arguments.inputPaths[1].path == "models");
    CHECK(arguments.inputPaths[2].folder);
    CHECK(arguments.inputPaths[2].folderTree);
    CHECK(arguments.inputPaths[2].path == "tree_models");
}

TEST_CASE("command line validates performance logging options")
{
    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-performance"}),
        "--log-performance requires --log-level to be trace, debug, or info.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "warn", "--log-file", "woby.log", "--log-performance"}),
        "--log-performance requires --log-level to be trace, debug, or info.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "info", "--log-file", "woby.log", "--log-frame-interval", "60"}),
        "--log-frame-interval requires --log-performance.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "info", "--log-file", "woby.log", "--log-performance", "--log-frame-interval", "0"}),
        "--log-frame-interval requires a positive integer.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "info", "--log-file", "woby.log", "--log-performance", "--log-slow-frame-ms", "-1"}),
        "--log-slow-frame-ms requires a positive number.",
        std::runtime_error);
}

TEST_CASE("command line requires log file for enabled logging")
{
    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "debug"}),
        "--log-file is required when --log-level is not off.",
        std::runtime_error);
}

TEST_CASE("command line rejects log file when logging is off")
{
    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-file", "woby.log"}),
        "--log-file requires --log-level to be set to a non-off level.",
        std::runtime_error);
}

TEST_CASE("command line rejects invalid and duplicate logging options")
{
    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "verbose", "--log-file", "woby.log"}),
        "Invalid --log-level value. Expected one of: off, trace, debug, info, warn, error, critical.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "info", "--log-level", "debug", "--log-file", "woby.log"}),
        "Only one --log-level option can be specified.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--log-level", "info", "--log-file", "one.log", "--log-file", "two.log"}),
        "Only one --log-file option can be specified.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({
            "woby",
            "--log-level",
            "info",
            "--log-file",
            "woby.log",
            "--log-performance",
            "--log-performance",
        }),
        "Only one --log-performance option can be specified.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({
            "woby",
            "--log-level",
            "info",
            "--log-file",
            "woby.log",
            "--log-performance",
            "--log-frame-interval",
            "60",
            "--log-frame-interval",
            "120",
        }),
        "Only one --log-frame-interval option can be specified.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({
            "woby",
            "--log-level",
            "info",
            "--log-file",
            "woby.log",
            "--log-performance",
            "--log-slow-frame-ms",
            "20",
            "--log-slow-frame-ms",
            "40",
        }),
        "Only one --log-slow-frame-ms option can be specified.",
        std::runtime_error);
}

TEST_CASE("command line rejects malformed numeric values and duplicate scene inputs")
{
    CHECK_THROWS_WITH_AS(
        parse({
            "woby",
            "--log-level",
            "info",
            "--log-file",
            "woby.log",
            "--log-performance",
            "--log-frame-interval",
            "-1",
        }),
        "--log-frame-interval requires a positive integer.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({
            "woby",
            "--log-level",
            "info",
            "--log-file",
            "woby.log",
            "--log-performance",
            "--log-frame-interval",
            "12x",
        }),
        "--log-frame-interval requires a positive integer.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({
            "woby",
            "--log-level",
            "info",
            "--log-file",
            "woby.log",
            "--log-performance",
            "--log-slow-frame-ms",
            "nan",
        }),
        "--log-slow-frame-ms requires a positive number.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "--scene", "one.woby", "--woby", "two.woby"}),
        "Only one woby scene file can be specified.",
        std::runtime_error);

    CHECK_THROWS_WITH_AS(
        parse({"woby", "model.obj"}),
        "Unexpected argument: model.obj",
        std::runtime_error);
}

TEST_CASE("command line parses generated valid argv cases")
{
    constexpr std::array<GeneratedLogLevel, 7> logLevels = {{
        {"off", woby::LogLevel::off, false},
        {"trace", woby::LogLevel::trace, true},
        {"debug", woby::LogLevel::debug, true},
        {"info", woby::LogLevel::info, true},
        {"warn", woby::LogLevel::warn, false},
        {"error", woby::LogLevel::error, false},
        {"critical", woby::LogLevel::critical, false},
    }};

    std::mt19937 random(0xc011d1u);
    for (size_t iteration = 0; iteration < 96u; ++iteration) {
        const GeneratedLogLevel level = logLevels[iteration % logLevels.size()];
        const bool version = (iteration & 1u) != 0u;
        const bool scene = (iteration & 2u) != 0u;
        const bool performance = level.supportsPerformance && (iteration & 4u) != 0u;
        const bool frameInterval = performance && (iteration & 8u) != 0u;
        const bool slowFrame = performance && (iteration & 16u) != 0u;

        std::vector<std::vector<std::string>> groups;
        if (version) {
            groups.push_back({"--version"});
        }
        if (scene) {
            groups.push_back({"--scene", "scene_" + std::to_string(iteration) + ".woby"});
        }
        if (level.value != woby::LogLevel::off) {
            groups.push_back({"--log-level", level.name});
            groups.push_back({"--log-file", "generated_" + std::to_string(iteration) + ".log"});
        }
        if (performance) {
            groups.push_back({"--log-performance"});
        }
        if (frameInterval) {
            groups.push_back({"--log-frame-interval", std::to_string(1u + iteration)});
        }
        if (slowFrame) {
            groups.push_back({"--log-slow-frame-ms", std::to_string(0.5 + double(iteration))});
        }

        const size_t inputCount = 1u + iteration % 4u;
        for (size_t inputIndex = 0; inputIndex < inputCount; ++inputIndex) {
            if ((inputIndex + iteration) % 2u == 0u) {
                groups.push_back({"--file", "model_" + std::to_string(iteration) + "_"
                    + std::to_string(inputIndex) + ".obj"});
            } else {
                groups.push_back({"--folder", "models_" + std::to_string(iteration) + "_"
                    + std::to_string(inputIndex)});
            }
        }

        std::shuffle(groups.begin(), groups.end(), random);

        std::vector<std::string> argv = {"woby"};
        std::vector<woby::ModelPathOption> expectedInputs;
        for (const auto& group : groups) {
            if (group[0] == "--file" || group[0] == "--folder") {
                woby::ModelPathOption input;
                input.folder = group[0] == "--folder";
                input.path = group[1];
                expectedInputs.push_back(input);
            }
            argv.insert(argv.end(), group.begin(), group.end());
        }

        const woby::AppArguments parsed = parse(argv);

        CHECK(parsed.showVersion == version);
        CHECK(parsed.logLevel == level.value);
        CHECK(parsed.logPerformance == performance);
        if (level.value == woby::LogLevel::off) {
            CHECK_FALSE(parsed.logFile.has_value());
        } else {
            REQUIRE(parsed.logFile.has_value());
            CHECK(parsed.logFile.value() == "generated_" + std::to_string(iteration) + ".log");
        }
        if (scene) {
            REQUIRE(parsed.scenePath.has_value());
            CHECK(parsed.scenePath.value() == "scene_" + std::to_string(iteration) + ".woby");
        } else {
            CHECK_FALSE(parsed.scenePath.has_value());
        }
        CHECK(parsed.logFrameInterval == (frameInterval ? 1u + iteration : 120u));
        CHECK(parsed.logSlowFrameMilliseconds.has_value() == slowFrame);
        if (slowFrame) {
            CHECK(parsed.logSlowFrameMilliseconds.value() == doctest::Approx(0.5 + double(iteration)));
        }
        REQUIRE(parsed.inputPaths.size() == expectedInputs.size());
        for (size_t inputIndex = 0; inputIndex < expectedInputs.size(); ++inputIndex) {
            CHECK(parsed.inputPaths[inputIndex].folder == expectedInputs[inputIndex].folder);
            CHECK(parsed.inputPaths[inputIndex].folderTree == expectedInputs[inputIndex].folderTree);
            CHECK(parsed.inputPaths[inputIndex].path == expectedInputs[inputIndex].path);
        }
    }
}

TEST_CASE("command line rejects generated missing value and unknown option cases")
{
    const std::array<const char*, 9> valueOptions = {{
        "--scene",
        "--woby",
        "--file",
        "--folder",
        "--folder-tree",
        "--log-level",
        "--log-file",
        "--log-frame-interval",
        "--log-slow-frame-ms",
    }};

    for (const char* option : valueOptions) {
        CHECK_THROWS_AS(parse({"woby", option}), std::runtime_error);
    }

    for (size_t iteration = 0; iteration < 32u; ++iteration) {
        CHECK_THROWS_AS(
            parse({"woby", "--generated-unknown-" + std::to_string(iteration)}),
            std::runtime_error);
    }
}

TEST_CASE("command line plugin options repeat and preserve file folder order")
{
    const auto arguments = parse({"woby", "--plugin-folder", "first folder", "--plugin", "one.dll",
        "--plugin-folder", "second", "--plugin", "two.dll", "--file", "model.off"});
    REQUIRE(arguments.pluginPaths.size() == 4u);
    CHECK(arguments.pluginPaths[0].folder);
    CHECK(arguments.pluginPaths[0].path == "first folder");
    CHECK_FALSE(arguments.pluginPaths[1].folder);
    CHECK(arguments.pluginPaths[1].path == "one.dll");
    CHECK(arguments.pluginPaths[2].folder);
    CHECK(arguments.pluginPaths[2].path == "second");
    CHECK_FALSE(arguments.pluginPaths[3].folder);
    CHECK(arguments.pluginPaths[3].path == "two.dll");
    REQUIRE(arguments.inputPaths.size() == 1u);
    for (const auto* option : {"--plugin", "--plugin-folder"}) {
        CHECK_THROWS_AS(parse({"woby", option}), std::runtime_error);
        CHECK_THROWS_AS(parse({"woby", option, ""}), std::runtime_error);
        CHECK_THROWS_AS(parse({"woby", option, "--file", "a.obj"}), std::runtime_error);
    }
}
