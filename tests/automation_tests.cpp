#include "automation.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ui_operations.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <set>
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

std::optional<woby::AutomationCommand> takeCommand(woby::AutomationRuntime& runtime)
{
    return woby::takeAutomationCommand(runtime);
}

bool completeCommand(woby::AutomationRuntime& runtime, woby::AutomationCommandId id,
    const woby::AutomationCommandResult& result)
{
    return woby::completeAutomationCommand(runtime, id, result);
}

bool waitForQueued(const AutomationFixture& fixture, size_t count)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (request(fixture.instance, "instance.info").at("result").at("queuedCommands") == count) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

std::optional<woby::AutomationCommand> waitForCommand(woby::AutomationRuntime& runtime)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto command = takeCommand(runtime)) {
            return command;
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
    CHECK_FALSE(takeCommand(*fixture.server).has_value());
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
    CHECK_FALSE(takeCommand(*fixture.server).has_value());
}

TEST_CASE("automation screenshot waits for main thread completion and is dispatched once")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "capture.png";
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 4}});
    });
    const auto requested = waitForCommand(*fixture.server);
    REQUIRE(requested.has_value());
    CHECK(requested->id != 0);
    REQUIRE(std::holds_alternative<woby::AutomationScreenshotCommand>(requested->payload));
    CHECK(std::get<woby::AutomationScreenshotCommand>(requested->payload).outputPath == path);
    CHECK_FALSE(takeCommand(*fixture.server).has_value());
    CHECK(response.wait_for(0ms) == std::future_status::timeout);
    CHECK(request(fixture.instance, "instance.info").at("result").at("ready").get<bool>());
    CHECK(completeCommand(*fixture.server, requested->id, woby::AutomationScreenshotResult{path}));
    const auto result = response.get();
    CHECK(result.at("id") == "test-request");
    CHECK(result.at("result").at("instance") == "test");
    CHECK(result.at("result").at("path") == woby::pathToUtf8(path));
    CHECK_FALSE(result.at("result").contains("revision"));
}

TEST_CASE("automation propagates capture failures and releases timed out queued work")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = woby::pathToUtf8(fixture.directory / "capture.png");
    CHECK(request(fixture.instance, "screenshot.capture", {{"path", path}, {"timeoutSeconds", 1}}).at("error").at("code") == -32003);
    CHECK_FALSE(takeCommand(*fixture.server).has_value());
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", path}, {"timeoutSeconds", 4}});
    });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command.has_value());
    CHECK(completeCommand(*fixture.server, command->id, woby::AutomationCommandError{"Cannot write PNG."}));
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
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command.has_value());
    CHECK(response.get().at("error").at("code") == -32003);
    CHECK(request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 1}})
              .at("error").at("code") == -32003);
    CHECK(completeCommand(*fixture.server, command->id, woby::AutomationScreenshotResult{path}));
    CHECK_FALSE(takeCommand(*fixture.server).has_value());
}

TEST_CASE("automation command IDs prevent unknown and duplicate completions from finishing other requests")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto inputPath = fixture.directory / "nested" / ".." / "capture.jpg";
    const auto savedPath = fixture.directory / "capture.png";
    const woby::AutomationScreenshotResult success{savedPath};
    const woby::AutomationCommandError failure{"Late failure from an older command."};
    CHECK_FALSE(completeCommand(*fixture.server, 0, success));

    auto firstResponse = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(inputPath)}, {"timeoutSeconds", 4}});
    });
    const auto first = waitForCommand(*fixture.server);
    REQUIRE(first.has_value());
    CHECK(std::get<woby::AutomationScreenshotCommand>(first->payload).outputPath == inputPath.lexically_normal());
    CHECK_FALSE(completeCommand(*fixture.server, 0, failure));
    CHECK(firstResponse.wait_for(0ms) == std::future_status::timeout);
    CHECK(completeCommand(*fixture.server, first->id, success));
    CHECK(firstResponse.get().at("result").at("path") == woby::pathToUtf8(savedPath));
    CHECK_FALSE(completeCommand(*fixture.server, first->id, success));

    // Clients may reuse their JSON-RPC ID; internal command IDs must still differ.
    auto secondResponse = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(savedPath)}, {"timeoutSeconds", 4}});
    });
    const auto second = waitForCommand(*fixture.server);
    REQUIRE(second.has_value());
    CHECK(second->id != first->id);
    CHECK_FALSE(completeCommand(*fixture.server, first->id, success));
    CHECK_FALSE(completeCommand(*fixture.server, first->id, failure));
    CHECK(secondResponse.wait_for(0ms) == std::future_status::timeout);
    CHECK_FALSE(takeCommand(*fixture.server).has_value());
    CHECK(completeCommand(*fixture.server, second->id, success));
    const auto secondResult = secondResponse.get();
    CHECK(secondResult.at("id") == "test-request");
    CHECK(secondResult.at("result").at("path") == woby::pathToUtf8(savedPath));
}

TEST_CASE("object queries dispatch typed commands and expose session scoped string IDs")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const woby::SceneObjectInfo file{41, woby::SceneObjectKind::file, "same", fixture.directory / "same.obj", 0};
    const woby::SceneObjectInfo group{42, woby::SceneObjectKind::group, "same", {}, 41};
    auto listing = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"timeoutSeconds", 4}});
    });
    const auto listCommand = waitForCommand(*fixture.server);
    REQUIRE(listCommand);
    CHECK(std::holds_alternative<woby::AutomationObjectsCommand>(listCommand->payload));
    CHECK_FALSE(completeCommand(*fixture.server, listCommand->id, woby::AutomationScreenshotResult{}));
    CHECK(listing.wait_for(0ms) == std::future_status::timeout);
    CHECK(completeCommand(*fixture.server, listCommand->id, woby::AutomationObjectsResult{{file, group}}));
    const auto list = listing.get();
    CHECK(list.at("id") == "test-request");
    CHECK(list.at("result").at("instance") == "test");
    const auto& objects = list.at("result").at("objects");
    REQUIRE(objects.size() == 2u);
    const auto fileId = objects[0].at("id").get<std::string>();
    const auto groupId = objects[1].at("id").get<std::string>();
    CHECK(fileId != groupId);
    CHECK(objects[0].at("path") == woby::pathToUtf8(file.path));
    CHECK(objects[1].at("fileId") == fileId);
    CHECK(objects[1].at("kind") == "group");
    CHECK(list.dump().find(fixture.instance.token) == std::string::npos);

    auto lookup = std::async(std::launch::async, [&] {
        return request(fixture.instance, "object.get", {{"id", groupId}, {"timeoutSeconds", 4}});
    });
    const auto getCommand = waitForCommand(*fixture.server);
    REQUIRE(getCommand);
    REQUIRE(std::holds_alternative<woby::AutomationObjectCommand>(getCommand->payload));
    CHECK(std::get<woby::AutomationObjectCommand>(getCommand->payload).objectId == group.id);
    CHECK(completeCommand(*fixture.server, getCommand->id, woby::AutomationObjectResult{group}));
    const auto lookupResult = lookup.get().at("result");
    CHECK(lookupResult.at("object") == objects[1]);
    CHECK_FALSE(lookupResult.contains("revision"));

    auto stale = std::async(std::launch::async, [&] {
        return request(fixture.instance, "object.get", {{"id", groupId}, {"timeoutSeconds", 4}});
    });
    const auto staleCommand = waitForCommand(*fixture.server);
    REQUIRE(staleCommand);
    CHECK(completeCommand(*fixture.server, staleCommand->id,
        woby::AutomationCommandError{"Unknown or stale object ID.", -32005}));
    CHECK(stale.get().at("error").at("code") == -32005);

    AutomationFixture other;
    woby::setAutomationReady(*other.server);
    CHECK(request(other.instance, "object.get", {{"id", fileId}}).at("error").at("code") == -32005);
    CHECK_FALSE(takeCommand(*other.server));
    fixture.server.reset();
    fixture.server = woby::startAutomation("test", fixture.directory);
    fixture.instance = woby::readAutomationInstance(fixture.directory, "test");
    woby::setAutomationReady(*fixture.server);
    CHECK(request(fixture.instance, "object.get", {{"id", fileId}}).at("error").at("code") == -32005);
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("object queries validate inputs before scheduling work")
{
    AutomationFixture fixture;
    CHECK(request(fixture.instance, "objects.list").at("error").at("code") == -32001);
    woby::setAutomationReady(*fixture.server);
    for (const auto& params : std::vector<Json>{
             Json::object(), {{"id", 1}}, {{"id", ""}}, {{"id", "same.obj"}},
             {{"id", "obj-" + std::string(32, 'a') + "-0000000000000000"}},
             {{"id", "obj-" + std::string(32, 'a') + "-000000000000000g"}},
             {{"id", std::string(100, 'a')}},
             {{"id", "obj-" + std::string(32, 'a') + "-0000000000000001"}, {"unexpected", true}},
         }) {
        CHECK(request(fixture.instance, "object.get", params).at("error").at("code") == -32602);
    }
    for (const auto& params : std::vector<Json>{
             {{"id", "unused"}}, {{"unexpected", true}}, {{"timeoutSeconds", 0}},
             {{"timeoutSeconds", 3601}}, {{"timeoutSeconds", 1.5}}, {{"timeoutSeconds", "2"}},
         }) {
        CHECK(request(fixture.instance, "objects.list", params).at("error").at("code") == -32602);
    }
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("removed revision methods and preconditions are rejected without queueing work")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    CHECK(request(fixture.instance, "scene.revision").at("error").at("code") == -32601);
    CHECK(request(fixture.instance, "objects.list", {{"ifRevision", "old-token"}}).at("error").at("code") == -32602);
    CHECK(request(fixture.instance, "object.get", {{"id", "unused"}, {"ifRevision", "old-token"}}).at("error").at("code") == -32602);
    CHECK(request(fixture.instance, "screenshot.capture",
        {{"path", woby::pathToUtf8(fixture.directory / "rejected.png")}, {"ifRevision", "old-token"}}).at("error").at("code") == -32602);
    CHECK_FALSE(takeCommand(*fixture.server));
    CHECK(request(fixture.instance, "instance.info").at("result").at("queuedCommands") == 0);
}

TEST_CASE("automation commands execute in bounded FIFO order and discovery stays responsive")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "ordered.png";
    auto capture = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 4}});
    });
    const auto first = waitForCommand(*fixture.server);
    REQUIRE(first);
    std::vector<std::future<Json>> queued;
    for (size_t i = 0; i < woby::maxAutomationCommands - 1; ++i) {
        queued.push_back(std::async(std::launch::async, [&] {
            return request(fixture.instance, "objects.list", {{"timeoutSeconds", 4}});
        }));
        REQUIRE(waitForQueued(fixture, i + 1));
    }
    const auto info = request(fixture.instance, "instance.info").at("result");
    CHECK(info.at("activeSequence") == std::to_string(first->id));
    CHECK(info.at("queuedCommands") == woby::maxAutomationCommands - 1);
    CHECK(request(fixture.instance, "objects.list").at("error").at("code") == -32002);
    CHECK_FALSE(takeCommand(*fixture.server));
    CHECK(completeCommand(*fixture.server, first->id, woby::AutomationScreenshotResult{path}));
    const auto captureResult = capture.get().at("result");
    CHECK(captureResult.at("sequence") == std::to_string(first->id));
    for (size_t i = 0; i < queued.size(); ++i) {
        const auto command = takeCommand(*fixture.server);
        REQUIRE(command);
        CHECK(command->id == first->id + i + 1);
        CHECK(std::holds_alternative<woby::AutomationObjectsCommand>(command->payload));
        CHECK(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{}));
        const auto result = queued[i].get().at("result");
        CHECK(result.at("sequence") == std::to_string(command->id));
        CHECK_FALSE(result.contains("revision"));
    }
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("concurrent unguarded clients preserve response ownership while the main thread edits the scene")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    constexpr size_t clientCount = 12;
    constexpr size_t requestsPerClient = 12;
    std::promise<void> start;
    auto gate = start.get_future().share();
    std::vector<std::future<std::vector<Json>>> clients;
    for (size_t client = 0; client < clientCount; ++client) {
        clients.push_back(std::async(std::launch::async, [&, client, gate] {
            gate.wait();
            std::vector<Json> responses;
            for (size_t index = 0; index < requestsPerClient; ++index) {
                if (index % 3 == 0) {
                    const auto path = fixture.directory / (std::to_string(client) + "-" + std::to_string(index) + ".png");
                    responses.push_back(request(fixture.instance, "screenshot.capture",
                        {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 2}}));
                } else {
                    responses.push_back(request(fixture.instance, "objects.list", {{"timeoutSeconds", 2}}));
                }
            }
            return responses;
        }));
    }

    woby::UiState state;
    std::map<woby::AutomationCommandId, std::string> expectedNames;
    std::set<woby::AutomationCommandId> executed;
    std::optional<woby::AutomationCommand> capture;
    size_t captureFrame = 0;
    size_t frame = 0;
    woby::AutomationCommandId previousId = 0;
    const auto finished = [&] {
        return std::all_of(clients.begin(), clients.end(), [](auto& client) {
            return client.wait_for(0ms) == std::future_status::ready;
        });
    };
    start.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!finished() && std::chrono::steady_clock::now() < deadline) {
        // Simulate input and scene replacement on the main thread. HTTP workers
        // must never retain references into this storage across frames.
        woby::orbitUiCamera(state, 1.0f, 0.5f);
        woby::toggleShowGrid(state);
        state.sceneNodes.clear();
        woby::UiSceneNode node;
        node.name = "frame-" + std::to_string(frame);
        state.sceneNodes.push_back(std::move(node));
        woby::assignSceneObjectIds(state);

        if (capture) {
            CHECK_FALSE(takeCommand(*fixture.server));
            if (frame >= captureFrame) {
                const auto& path = std::get<woby::AutomationScreenshotCommand>(capture->payload).outputPath;
                CHECK(completeCommand(*fixture.server, capture->id, woby::AutomationScreenshotResult{path}));
                capture.reset();
            }
        }
        if (const auto command = takeCommand(*fixture.server)) {
            CHECK(command->id > previousId);
            previousId = command->id;
            CHECK(executed.insert(command->id).second);
            if (std::holds_alternative<woby::AutomationScreenshotCommand>(command->payload)) {
                capture = command;
                captureFrame = frame + 3; // Simulated asynchronous readback, no GPU in this test.
            } else {
                CHECK(std::holds_alternative<woby::AutomationObjectsCommand>(command->payload));
                expectedNames.emplace(command->id, state.sceneNodes.front().name);
                CHECK(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{woby::sceneObjects(state)}));
            }
        }
        ++frame;
        std::this_thread::sleep_for(1ms);
    }
    const bool allFinished = finished();
    if (!allFinished) {
        fixture.server.reset(); // Wake requests before future destruction if the pump failed.
    }
    REQUIRE(allFinished);
    std::set<woby::AutomationCommandId> returned;
    for (size_t client = 0; client < clients.size(); ++client) {
        const auto responses = clients[client].get();
        REQUIRE(responses.size() == requestsPerClient);
        for (size_t index = 0; index < responses.size(); ++index) {
            const auto& response = responses[index];
            CHECK(response.at("id") == "test-request");
            if (response.contains("error")) {
                CHECK(response.at("error").at("code") == -32002); // Bounded queue overload is allowed.
                continue;
            }
            const auto& result = response.at("result");
            const auto sequence = std::stoull(result.at("sequence").get<std::string>());
            CHECK(returned.insert(sequence).second);
            if (index % 3 == 0) {
                const auto path = fixture.directory / (std::to_string(client) + "-" + std::to_string(index) + ".png");
                CHECK(result.at("path") == woby::pathToUtf8(path));
            } else {
                REQUIRE(result.at("objects").size() == 1u);
                CHECK(result.at("objects")[0].at("name") == expectedNames.at(sequence));
            }
        }
    }
    CHECK_FALSE(returned.empty());
    CHECK(returned == executed);
    CHECK_FALSE(takeCommand(*fixture.server));
    CHECK(request(fixture.instance, "instance.info").at("result").at("queuedCommands") == 0);
}

TEST_CASE("queue timeouts preserve later command order and late capture completion ownership")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "late.png";
    auto capture = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(path)}, {"timeoutSeconds", 1}});
    });
    const auto active = waitForCommand(*fixture.server);
    REQUIRE(active);
    CHECK(capture.get().at("error").at("code") == -32003);
    auto expired = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"timeoutSeconds", 1}});
    });
    REQUIRE(waitForQueued(fixture, 1));
    auto later = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"timeoutSeconds", 4}});
    });
    REQUIRE(waitForQueued(fixture, 2));
    CHECK(expired.get().at("error").at("code") == -32003);
    REQUIRE(waitForQueued(fixture, 1));
    CHECK_FALSE(takeCommand(*fixture.server));
    CHECK(completeCommand(*fixture.server, active->id, woby::AutomationScreenshotResult{path}));
    const auto next = takeCommand(*fixture.server);
    REQUIRE(next);
    CHECK(next->id == active->id + 2);
    CHECK(std::holds_alternative<woby::AutomationObjectsCommand>(next->payload));
    CHECK_FALSE(completeCommand(*fixture.server, active->id, woby::AutomationScreenshotResult{path}));
    CHECK(completeCommand(*fixture.server, next->id, woby::AutomationObjectsResult{}));
    CHECK(later.get().contains("result"));
}

TEST_CASE("automation shutdown wakes waiting clients and removes discovery records")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "screenshot.capture", {{"path", woby::pathToUtf8(fixture.directory / "capture.png")}, {"timeoutSeconds", 4}});
    });
    REQUIRE(waitForCommand(*fixture.server).has_value());
    auto queued = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"timeoutSeconds", 4}});
    });
    REQUIRE(waitForQueued(fixture, 1));
    fixture.server.reset();
    CHECK(response.get().at("error").at("code") == -32001);
    CHECK(queued.get().at("error").at("code") == -32001);
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
