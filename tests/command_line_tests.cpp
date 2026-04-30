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

    REQUIRE(arguments.inputPaths.size() == 2u);
    CHECK_FALSE(arguments.inputPaths[0].folder);
    CHECK(arguments.inputPaths[0].path == "a.obj");
    CHECK(arguments.inputPaths[1].folder);
    CHECK(arguments.inputPaths[1].path == "models");
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
            CHECK(parsed.inputPaths[inputIndex].path == expectedInputs[inputIndex].path);
        }
    }
}

TEST_CASE("command line rejects generated missing value and unknown option cases")
{
    const std::array<const char*, 8> valueOptions = {{
        "--scene",
        "--woby",
        "--file",
        "--folder",
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
