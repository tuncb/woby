#include "command_line.h"

#include <doctest/doctest.h>

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
