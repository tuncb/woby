#include "automation.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

struct AutomationFixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / ("woby-automation-test-" + woby::automationRandomHex(8u));
    woby::AutomationOwner server = woby::startAutomation("test", directory);
    woby::AutomationInstance instance = woby::readAutomationInstance(directory, "test");

    ~AutomationFixture()
    {
        server.reset();
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

Json request(const woby::AutomationInstance& instance, const std::string& method, const Json& params = Json::object())
{
    httplib::Client client("127.0.0.1", instance.port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    const Json body = {{"jsonrpc", "2.0"}, {"id", "test-request"}, {"method", method}, {"params", params}};
    const auto result = client.Post("/rpc", {{"Authorization", "Bearer " + instance.token}}, body.dump(), "application/json");
    if (!result || result->status != 200) {
        throw std::runtime_error("Automation test could not get an HTTP response.");
    }
    return Json::parse(result->body);
}

std::optional<std::filesystem::path> waitForCapture(woby::AutomationRuntime& runtime)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto path = woby::takeAutomationScreenshot(runtime)) {
            return path;
        }
        std::this_thread::sleep_for(1ms);
    }
    return {};
}

} // namespace

TEST_CASE("automation instances reserve IDs and publish independent authenticated endpoints")
{
    AutomationFixture fixture;
    auto other = woby::startAutomation({}, fixture.directory);
    const std::string otherId = woby::automationInstanceId(*other);
    CHECK(woby::validInstanceId(otherId));
    CHECK(otherId != "test");
    const auto otherInfo = woby::readAutomationInstance(fixture.directory, otherId);
    CHECK(otherInfo.port != fixture.instance.port);
    CHECK(otherInfo.token != fixture.instance.token);
    CHECK_THROWS_AS((void)woby::startAutomation("test", fixture.directory), std::runtime_error);
    CHECK(woby::readAutomationInstances(fixture.directory).size() == 2u);
    const auto info = request(fixture.instance, "instance.info");
    CHECK(info.at("id") == "test-request");
    CHECK(info.at("result").at("id") == "test");
    CHECK_FALSE(info.at("result").at("ready").get<bool>());
    CHECK(info.dump().find(fixture.instance.token) == std::string::npos);
    woby::setAutomationReady(*fixture.server);
    CHECK(request(fixture.instance, "instance.info").at("result").at("ready").get<bool>());
    other.reset();
    CHECK(woby::readAutomationInstances(fixture.directory).size() == 1u);
    auto reused = woby::startAutomation(otherId, fixture.directory);
    CHECK(woby::automationInstanceId(*reused) == otherId);
}

TEST_CASE("automation rejects unauthenticated and browser requests")
{
    AutomationFixture fixture;
    httplib::Client client("127.0.0.1", fixture.instance.port);
    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"instance.info"})";
    const auto unauthorized = client.Post("/rpc", body, "application/json");
    REQUIRE(unauthorized);
    CHECK(unauthorized->status == 401);
    const auto wrongToken = client.Post("/rpc", {{"Authorization", "Bearer wrong"}}, body, "application/json");
    REQUIRE(wrongToken);
    CHECK(wrongToken->status == 401);
    const httplib::Headers auth = {{"Authorization", "Bearer " + fixture.instance.token}};
    auto browserHeaders = auth;
    browserHeaders.emplace("Origin", "https://example.com");
    const auto browser = client.Post("/rpc", browserHeaders, body, "application/json");
    REQUIRE(browser);
    CHECK(browser->status == 403);
    auto wrongHostHeaders = auth;
    wrongHostHeaders.emplace("Host", "example.com");
    const auto wrongHost = client.Post("/rpc", wrongHostHeaders, body, "application/json");
    REQUIRE(wrongHost);
    CHECK(wrongHost->status == 403);
    const auto text = client.Post("/rpc", auth, body, "text/plain");
    REQUIRE(text);
    CHECK(text->status == 415);
    CHECK_FALSE(woby::takeAutomationScreenshot(*fixture.server).has_value());
}

TEST_CASE("automation validates JSON RPC and capture parameters without scheduling work")
{
    AutomationFixture fixture;
    httplib::Client client("127.0.0.1", fixture.instance.port);
    const httplib::Headers auth = {{"Authorization", "Bearer " + fixture.instance.token}};
    const auto malformed = client.Post("/rpc", auth, "{", "application/json");
    REQUIRE(malformed);
    CHECK(Json::parse(malformed->body).at("error").at("code") == -32700);
    const auto batch = client.Post("/rpc", auth, "[]", "application/json");
    REQUIRE(batch);
    CHECK(Json::parse(batch->body).at("error").at("code") == -32600);
    const auto notification = client.Post("/rpc", auth, R"({"jsonrpc":"2.0","method":"screenshot.capture"})", "application/json");
    REQUIRE(notification);
    CHECK(notification->status == 204);
    CHECK(request(fixture.instance, "missing").at("error").at("code") == -32601);
    CHECK(request(fixture.instance, "instance.info", Json::array()).at("error").at("code") == -32602);
    const auto outputPath = woby::pathToUtf8(fixture.directory / "capture.png");
    CHECK(request(fixture.instance, "screenshot.capture", {{"path", outputPath}}).at("error").at("code") == -32001);
    woby::setAutomationReady(*fixture.server);
    const std::vector<Json> invalid = {
        Json::object(), {{"path", 42}}, {{"path", "relative.png"}}, {{"path", ""}},
        {{"path", outputPath}, {"timeoutSeconds", 0}}, {{"path", outputPath}, {"timeoutSeconds", 3601}},
        {{"path", outputPath}, {"timeoutSeconds", 1.5}}, {{"path", outputPath}, {"timeoutSeconds", "2"}},
        {{"path", outputPath}, {"unexpected", true}},
        {{"path", outputPath + std::string(1u, '\0') + "other"}},
    };
    for (const auto& params : invalid) {
        CHECK(request(fixture.instance, "screenshot.capture", params).at("error").at("code") == -32602);
    }
    CHECK_FALSE(woby::takeAutomationScreenshot(*fixture.server).has_value());
}

TEST_CASE("automation screenshot waits for main thread completion and rejects concurrent captures")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "capture.png";
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 4}});
    });
    const auto requested = waitForCapture(*fixture.server);
    REQUIRE(requested.has_value());
    CHECK(*requested == path);
    CHECK_FALSE(woby::takeAutomationScreenshot(*fixture.server).has_value());
    CHECK(response.wait_for(0ms) == std::future_status::timeout);
    CHECK(request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}}).at("error").at("code") == -32002);
    woby::completeAutomationScreenshot(*fixture.server, path);
    const auto result = response.get();
    CHECK(result.at("id") == "test-request");
    CHECK(result.at("result").at("instance") == "test");
    CHECK(result.at("result").at("path") == woby::pathToUtf8(path));
}

TEST_CASE("automation propagates capture failures and releases timed out queued work")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = woby::pathToUtf8(fixture.directory / "capture.png");
    CHECK(request(fixture.instance, "screenshot.capture", {{"path", path}, {"timeoutSeconds", 1}}).at("error").at("code") == -32003);
    CHECK_FALSE(woby::takeAutomationScreenshot(*fixture.server).has_value());
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", path}, {"timeoutSeconds", 4}});
    });
    REQUIRE(waitForCapture(*fixture.server).has_value());
    woby::completeAutomationScreenshot(*fixture.server, {}, "Cannot write PNG.");
    const auto failure = response.get();
    CHECK(failure.at("error").at("code") == -32004);
    CHECK(failure.at("error").at("message") == "Cannot write PNG.");
}

TEST_CASE("timed out GPU captures retain their slot until completion")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "capture.png";
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 1}});
    });
    REQUIRE(waitForCapture(*fixture.server).has_value());
    CHECK(response.get().at("error").at("code") == -32003);
    CHECK(request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}}).at("error").at("code") == -32002);
    woby::completeAutomationScreenshot(*fixture.server, path);
    CHECK_FALSE(woby::takeAutomationScreenshot(*fixture.server).has_value());
}

TEST_CASE("automation shutdown wakes waiting clients and removes discovery records")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(fixture.directory / "capture.png")}, {"timeoutSeconds", 4}});
    });
    REQUIRE(waitForCapture(*fixture.server).has_value());
    fixture.server.reset();
    CHECK(response.get().at("error").at("code") == -32001);
    CHECK(woby::readAutomationInstances(fixture.directory).empty());
}

TEST_CASE("automation discovery ignores corrupt records and handles missing instances")
{
    AutomationFixture fixture;
    std::ofstream(fixture.directory / "broken.json") << "{";
    CHECK(woby::readAutomationInstances(fixture.directory).size() == 1u);
    CHECK_THROWS_AS((void)woby::readAutomationInstance(fixture.directory, "missing"), std::runtime_error);
    CHECK_THROWS_AS((void)woby::readAutomationInstance(fixture.directory, "../test"), std::runtime_error);
    woby::ControlArguments arguments;
    arguments.command = woby::ControlCommand::screenshot;
    arguments.instanceId = "missing";
    arguments.outputPath = fixture.directory / "capture.png";
    arguments.json = true;
    CHECK(woby::runAutomationCommand(arguments, fixture.directory) == 1);
}

TEST_CASE("discovery skips stale records without contacting their old ports")
{
    AutomationFixture fixture;
    const auto record = fixture.directory / "instance-test.json";
    std::ifstream input(record);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();
    fixture.server.reset();
    std::ofstream(record) << contents;
    CHECK(woby::readAutomationInstances(fixture.directory).empty());
    fixture.server = woby::startAutomation("test", fixture.directory);
    CHECK(woby::readAutomationInstances(fixture.directory).size() == 1u);
    CHECK(woby::readAutomationInstance(fixture.directory, "test").token != fixture.instance.token);
}
