#include "automation.h"
#include "automation_registry.h"

#include <doctest/doctest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include "ui_operations.h"
#include "scene_lifecycle.h"
#include "control_scene.h"

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

Json request(const woby::AutomationInstance& instance, const std::string& method, const Json& params = Json::object(), const Json& rpcId = "test-request")
{
    httplib::Client client("127.0.0.1", instance.port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(5, 0);
    const Json body = {{"jsonrpc", "2.0"}, {"id", rpcId}, {"method", method}, {"params", params}};
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

TEST_CASE("extended automation rejects invalid input before admission and resolves session targets")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    for (const auto& [method, params] : std::vector<std::pair<std::string, Json>>{
        {"grid.set", {{"visible", "false"}}}, {"render.set", {{"target", "scene"}}},
        {"camera.dolly", {{"factor", 0}}}, {"camera.orbit", {{"yawDegrees", 1e100}}},
        {"status", {{"unexpected", 1}}}, {"scene.tree", {{"timeoutSeconds", 0}}},
        {"model.add", {{"path", "relative.obj"}}}, {"color.reset", {{"target", "broken"}}}}) {
        CAPTURE(method);
        CHECK(request(fixture.instance, method, params)["error"]["code"] == -32602);
        CHECK_FALSE(takeCommand(*fixture.server));
    }
    const auto target = woby::automationObjectId(*fixture.server, 42);
    auto pending = std::async(std::launch::async, [&] {
        return request(fixture.instance, "opacity.set", {{"target", target}, {"value", 0.5}});
    });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    const auto* operation = std::get_if<woby::ControlOperation>(&command->payload);
    REQUIRE(operation);
    CHECK(operation->objectId == 42);
    CHECK(operation->value == 0.5f);
    CHECK_FALSE(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{}));
    CHECK(completeCommand(*fixture.server, command->id, woby::AutomationCommandError{"Unknown target", -32005}));
    CHECK(pending.get()["error"]["code"] == -32005);
    auto foreign = target;
    foreign[4] = foreign[4] == '0' ? '1' : '0';
    CHECK(request(fixture.instance, "opacity.set", {{"target", foreign}, {"value", 0.5}})["error"]["code"] == -32005);
}

TEST_CASE("extended camera commands execute once and replay the applied result under retry keys")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const Json params = {{"forward", 2}, {"requestKey", "move-once"}};
    auto pending = std::async(std::launch::async, [&] { return request(fixture.instance, "camera.move", params); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    woby::UiState state;
    const auto clean = woby::createSceneDocument(state);
    const auto result = woby::applyControlSceneOperation(state, clean, std::get<woby::ControlOperation>(command->payload),
        [](woby::SceneObjectId id) { return std::to_string(id); }, 200, 800);
    CHECK(request(fixture.instance, "camera.move", params)["error"]["code"] == -32008);
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationControlResult{result}));
    const auto original = pending.get();
    CHECK(original["result"]["camera"] == woby::controlCameraInfo(state));
    CHECK(request(fixture.instance, "camera.move", params)["result"] == original["result"]);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "move-once"}})["result"]["result"] == original["result"]);
    CHECK(request(fixture.instance, "camera.move", {{"forward", 3}, {"requestKey", "move-once"}})["error"]["code"] == -32006);
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("comparison RPC validates every input ID before admission and resolves membership IDs")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto target = woby::automationObjectId(*fixture.server, 10);
    const auto input = woby::automationObjectId(*fixture.server, 20);
    auto foreign = input;
    foreign[4] = foreign[4] == '0' ? '1' : '0';
    for (const auto* field : {"a", "b"}) {
        CHECK(request(fixture.instance, "comparison.create", {{field, "malformed"}})["error"]["code"] == -32602);
        CHECK(request(fixture.instance, "comparison.create", {{field, foreign}})["error"]["code"] == -32005);
        CHECK(request(fixture.instance, "comparison.create", {{field, "scene"}})["error"]["code"] == -32602);
        CHECK_FALSE(takeCommand(*fixture.server));
    }
    CHECK(request(fixture.instance, "comparison.add", {{"target", target}, {"side", "a"}, {"object", foreign}})["error"]["code"] == -32005);
    CHECK(request(fixture.instance, "comparison.set", {{"target", target}, {"mode", "invalid"}})["error"]["code"] == -32602);
    CHECK_FALSE(takeCommand(*fixture.server));
    for (const auto* method : {"comparison.add", "comparison.remove"}) {
        auto pending = std::async(std::launch::async, [&] {
            return request(fixture.instance, method, {{"target", target}, {"side", "b"}, {"object", input}});
        });
        const auto command = waitForCommand(*fixture.server);
        REQUIRE(command);
        const auto& operation = std::get<woby::ControlOperation>(command->payload);
        CHECK(operation.objectId == 10);
        CHECK(operation.memberId == 20);
        CHECK(operation.side == "b");
        CHECK(completeCommand(*fixture.server, command->id, woby::AutomationControlResult{Json::object()}));
        CHECK(pending.get().contains("result"));
    }
}

TEST_CASE("comparison creation retries reuse the created object and recover its result")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    woby::UiState state;
    woby::Mesh mesh;
    mesh.vertices = {{{0, 0, 0}, {}, {}}, {{1, 0, 0}, {}, {}}, {{0, 1, 0}, {}, {}}};
    mesh.indices = {0, 1, 2};
    mesh.nodes = {{"triangle", 0, 3}};
    mesh.bounds = woby::calculateBounds(mesh.vertices);
    state.files.push_back(woby::createUiFileState("triangle.obj", mesh, 0));
    woby::assignSceneObjectIds(state);
    const auto clean = woby::createSceneDocument(state);
    const auto input = woby::automationObjectId(*fixture.server, state.files[0].objectId);
    const Json params = {{"name", "RPC comparison"}, {"a", input}, {"b", input}, {"requestKey", "create-once"}};
    auto pending = std::async(std::launch::async, [&] { return request(fixture.instance, "comparison.create", params); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    const auto& operation = std::get<woby::ControlOperation>(command->payload);
    CHECK(operation.aId == state.files[0].objectId);
    CHECK(operation.bId == state.files[0].objectId);
    const auto result = woby::applyControlSceneOperation(state, clean, operation,
        [&](woby::SceneObjectId id) { return woby::automationObjectId(*fixture.server, id); }, 200, 800);
    CHECK(completeCommand(*fixture.server, command->id, woby::AutomationControlResult{result}));
    const auto original = pending.get();
    CHECK(original["result"]["object"]["valid"] == true);
    CHECK(request(fixture.instance, "comparison.create", params)["result"] == original["result"]);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "create-once"}})["result"]["result"] == original["result"]);
    CHECK(state.comparisons.size() == 1);
    auto conflicting = params;
    conflicting["name"] = "Different";
    CHECK(request(fixture.instance, "comparison.create", conflicting)["error"]["code"] == -32006);
    CHECK_FALSE(takeCommand(*fixture.server));
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


TEST_CASE("retry keys deduplicate queued and running captures and replay with the current RPC id")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "retry.png";
    Json params = {{"path", woby::pathToUtf8(path)}, {"requestKey", "capture:1"}, {"timeoutSeconds", 4}};
    auto first = std::async(std::launch::async, [&] { return request(fixture.instance, "screenshot.capture", params, 11); });
    REQUIRE(waitForQueued(fixture, 1));
    const auto queued = request(fixture.instance, "command.get", {{"requestKey", "capture:1"}}).at("result");
    CHECK(queued.at("state") == "queued");
    const auto commandId = queued.at("commandId");
    auto duplicate = request(fixture.instance, "screenshot.capture", params);
    CHECK(duplicate.at("error").at("code") == -32008);
    CHECK(duplicate.at("error").at("data").at("commandId") == commandId);
    CHECK(waitForQueued(fixture, 1));
    const auto active = waitForCommand(*fixture.server);
    REQUIRE(active);
    CHECK(request(fixture.instance, "command.get", {{"id", commandId}}).at("result").at("state") == "running");

    std::vector<std::future<Json>> duplicates;
    for (int i = 0; i < 20; ++i) {
        duplicates.push_back(std::async(std::launch::async, [&] { return request(fixture.instance, "screenshot.capture", params); }));
    }
    for (auto& response : duplicates) {
        CHECK(response.get().at("error").at("code") == -32008);
    }
    CHECK(request(fixture.instance, "instance.info").at("result").at("queuedCommands") == 0);
    CHECK_FALSE(takeCommand(*fixture.server));
    auto conflict = params;
    conflict["path"] = woby::pathToUtf8(fixture.directory / "other.png");
    CHECK(request(fixture.instance, "screenshot.capture", conflict).at("error").at("code") == -32006);
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "capture:1"}}).at("error").at("code") == -32006);
    REQUIRE(completeCommand(*fixture.server, active->id, woby::AutomationScreenshotResult{path}));
    const auto original = first.get();
    CHECK(original.at("id") == 11);
    CHECK(original.at("result").at("state") == "succeeded");
    // A new wait limit does not change the identity or extend the original deadline.
    params["timeoutSeconds"] = 1;
    const auto replay = request(fixture.instance, "screenshot.capture", params, "retry-response");
    CHECK(replay.at("id") == "retry-response");
    CHECK(replay.at("result") == original.at("result"));
    CHECK(request(fixture.instance, "command.get", {{"id", commandId}}).at("result").at("result") == original.at("result"));
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("timed out captures retain late results while queued expiration is terminal")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = fixture.directory / "late-retry.png";
    const Json params = {{"path", woby::pathToUtf8(path)}, {"requestKey", "late"}, {"timeoutSeconds", 1}};
    auto capture = std::async(std::launch::async, [&] { return request(fixture.instance, "screenshot.capture", params); });
    const auto active = waitForCommand(*fixture.server);
    REQUIRE(active);
    const auto timedOut = capture.get().at("error");
    CHECK(timedOut.at("code") == -32003);
    CHECK(timedOut.at("data").at("state") == "running");
    CHECK(request(fixture.instance, "screenshot.capture", params).at("error").at("code") == -32008);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "late"}}).at("result").at("state") == "running");

    const Json queuedParams = {{"requestKey", "queued"}, {"timeoutSeconds", 1}};
    const auto expired = request(fixture.instance, "objects.list", queuedParams);
    CHECK(expired.at("error").at("data").at("state") == "expired-before-start");
    CHECK(request(fixture.instance, "objects.list", queuedParams) == expired);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "queued"}}).at("result").at("state") == "expired-before-start");
    REQUIRE(completeCommand(*fixture.server, active->id, woby::AutomationScreenshotResult{path}));
    const auto replay = request(fixture.instance, "screenshot.capture", params).at("result");
    CHECK(replay.at("state") == "succeeded");
    CHECK(replay.at("commandId") == timedOut.at("data").at("commandId"));
    CHECK(replay.at("path") == woby::pathToUtf8(path));
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("retry keys retain failures and replay query snapshots")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    for (bool fail : {false, true}) {
        const Json params = {{"requestKey", fail ? "failure" : "snapshot"}, {"timeoutSeconds", 4}};
        auto response = std::async(std::launch::async, [&] { return request(fixture.instance, "objects.list", params); });
        const auto command = waitForCommand(*fixture.server);
        REQUIRE(command);
        if (fail) {
            REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationCommandError{"Cannot inspect scene.", -32004}));
        } else {
            woby::SceneObjectInfo object;
            object.id = 1;
            object.name = "Original name";
            REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{{object}}));
        }
        const auto original = response.get();
        CHECK(request(fixture.instance, "objects.list", params) == original);
        const auto status = request(fixture.instance, "command.get", {{"requestKey", params.at("requestKey")}}).at("result");
        CHECK(status.at("state") == (fail ? "failed" : "succeeded"));
        CHECK(status.at(fail ? "error" : "result") == original.at(fail ? "error" : "result"));
        CHECK_FALSE(takeCommand(*fixture.server));
    }
}

TEST_CASE("command history evicts results without ever reusing admitted retry keys")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    Json firstId;
    Json lastId;
    for (size_t i = 0; i < woby::maxAutomationRequestKeys; ++i) {
        const Json params = {{"requestKey", "key-" + std::to_string(i)}, {"timeoutSeconds", 4}};
        auto response = std::async(std::launch::async, [&] { return request(fixture.instance, "objects.list", params); });
        const auto command = waitForCommand(*fixture.server);
        REQUIRE(command);
        REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{}));
        lastId = response.get().at("result").at("commandId");
        if (i == 0) {
            firstId = lastId;
        }
        if (i == woby::maxAutomationHistory - 1) {
            CHECK(request(fixture.instance, "command.get", {{"id", firstId}}).at("result").at("state") == "succeeded");
        }
        if (i == woby::maxAutomationHistory) {
            CHECK(request(fixture.instance, "command.get", {{"id", firstId}}).at("error").at("code") == -32009);
        }
    }
    CHECK(request(fixture.instance, "command.get", {{"id", firstId}}).at("error").at("code") == -32009);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "key-0"}}).at("error").at("code") == -32009);
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "key-0"}}).at("error").at("code") == -32009);
    CHECK(request(fixture.instance, "command.get", {{"id", lastId}}).at("result").at("state") == "succeeded");
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "key-1023"}}).contains("result"));
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "new-key"}}).at("error").at("code") == -32010);
    CHECK_FALSE(takeCommand(*fixture.server));
    // The key ledger limit does not block ordinary unkeyed commands.
    auto unkeyed = std::async(std::launch::async, [&] { return request(fixture.instance, "objects.list"); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{}));
    CHECK(unkeyed.get().contains("result"));
}

TEST_CASE("oversized results are delivered but not retained and foreign command ids never alias")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    auto response = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"requestKey", "large"}, {"timeoutSeconds", 4}});
    });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    woby::SceneObjectInfo object;
    object.id = 1;
    object.name.assign(woby::maxAutomationHistoryBytes, 'x');
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationObjectsResult{{object}}));
    const auto result = response.get().at("result");
    CHECK(result.at("objects")[0].at("name").get_ref<const std::string&>().size() == woby::maxAutomationHistoryBytes);
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "large"}}).at("error").at("code") == -32009);
    const auto oldId = result.at("commandId");
    fixture.server.reset();
    fixture.server = woby::startAutomation("test", fixture.directory);
    fixture.instance = woby::readAutomationInstance(fixture.directory, "test");
    CHECK(request(fixture.instance, "command.get", {{"id", oldId}}).at("error").at("code") == -32007);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "large"}}).at("error").at("code") == -32007);
}

TEST_CASE("command lookup and retry key validation never schedule scene work")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    for (const Json& key : std::vector<Json>{nullptr, 1, "", "contains space", std::string(129, 'x')}) {
        CHECK(request(fixture.instance, "objects.list", {{"requestKey", key}}).at("error").at("code") == -32602);
        CHECK(request(fixture.instance, "command.get", {{"requestKey", key}}).at("error").at("code") == -32602);
    }
    for (const Json& params : std::vector<Json>{Json::object(), {{"id", 1}}, {{"id", "x"}, {"requestKey", "key"}},
             {{"requestKey", "key"}, {"timeoutSeconds", 1}}, {{"unexpected", "key"}}}) {
        CHECK(request(fixture.instance, "command.get", params).at("error").at("code") == -32602);
    }
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "unknown"}}).at("error").at("code") == -32007);
    CHECK_FALSE(takeCommand(*fixture.server));
}


TEST_CASE("a disconnected client recovers the original command by its preselected key")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const Json params = {{"path", woby::pathToUtf8(fixture.directory / "disconnected.png")},
        {"requestKey", "connection-lost"}, {"timeoutSeconds", 4}};
    auto disconnected = std::async(std::launch::async, [&] {
        httplib::Client client("127.0.0.1", fixture.instance.port);
        client.set_read_timeout(1, 0);
        const Json body = {{"jsonrpc", "2.0"}, {"id", "lost"}, {"method", "screenshot.capture"}, {"params", params}};
        const auto response = client.Post("/rpc", {{"Authorization", "Bearer " + fixture.instance.token}}, body.dump(), "application/json");
        return !response;
    });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    REQUIRE(disconnected.get());
    const auto status = request(fixture.instance, "command.get", {{"requestKey", "connection-lost"}}).at("result");
    CHECK(status.at("state") == "running");
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationScreenshotResult{fixture.directory / "disconnected.png"}));
    const auto replay = request(fixture.instance, "screenshot.capture", params).at("result");
    CHECK(replay.at("commandId") == status.at("commandId"));
    CHECK(replay.at("state") == "succeeded");
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("full queues still allow retry recovery and do not reserve rejected keys")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    auto first = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"requestKey", "first"}, {"timeoutSeconds", 4}});
    });
    const auto active = waitForCommand(*fixture.server);
    REQUIRE(active);
    std::vector<std::future<Json>> queued;
    for (size_t i = 1; i < woby::maxAutomationCommands; ++i) {
        queued.push_back(std::async(std::launch::async, [&] { return request(fixture.instance, "objects.list", {{"timeoutSeconds", 4}}); }));
    }
    REQUIRE(waitForQueued(fixture, woby::maxAutomationCommands - 1));
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "first"}}).at("error").at("code") == -32008);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "first"}}).at("result").at("state") == "running");
    CHECK(request(fixture.instance, "objects.list", {{"requestKey", "rejected"}}).at("error").at("code") == -32002);
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "rejected"}}).at("error").at("code") == -32007);
    REQUIRE(completeCommand(*fixture.server, active->id, woby::AutomationObjectsResult{}));
    CHECK(first.get().contains("result"));
    for (size_t i = 1; i < woby::maxAutomationCommands; ++i) {
        const auto next = waitForCommand(*fixture.server);
        REQUIRE(next);
        REQUIRE(completeCommand(*fixture.server, next->id, woby::AutomationObjectsResult{}));
    }
    for (auto& response : queued) {
        CHECK(response.get().contains("result"));
    }
    auto accepted = std::async(std::launch::async, [&] {
        return request(fixture.instance, "objects.list", {{"requestKey", "rejected"}, {"timeoutSeconds", 4}});
    });
    const auto next = waitForCommand(*fixture.server);
    REQUIRE(next);
    CHECK(next->id == active->id + woby::maxAutomationCommands);
    REQUIRE(completeCommand(*fixture.server, next->id, woby::AutomationObjectsResult{}));
    CHECK(accepted.get().contains("result"));
}

TEST_CASE("automation validates lifecycle parameters before admitting work")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const auto path = woby::pathToUtf8(fixture.directory / "scene.woby");
    const std::vector<std::pair<std::string, Json>> invalid = {
        {"scene.open", {}}, {"scene.save-as", {}},
        {"scene.open", {{"path", "relative.woby"}}},
        {"scene.open", {{"path", path}, {"onDirty", "ask"}}},
        {"scene.new", {{"onDirty", 42}}},
        {"quit", {{"onDirty", nullptr}}},
        {"scene.new", {{"savePath", path}}},
        {"quit", {{"onDirty", "discard"}, {"savePath", path}}},
        {"scene.save", {{"path", path}}},
        {"scene.save-as", {{"path", path}, {"overwrite", 1}}},
        {"scene.open", {{"path", path}, {"overwrite", false}}},
        {"scene.new", {{"path", path}}},
        {"scene.new", {{"timeoutSeconds", 0}}},
        {"quit", {{"unknown", true}}},
    };
    for (const auto& [method, parameters] : invalid) {
        INFO(method, " ", parameters.dump());
        auto params = parameters.is_object() ? parameters : Json::object();
        params["requestKey"] = "invalid";
        CHECK(request(fixture.instance, method, params)["error"]["code"] == -32602);
        CHECK_FALSE(takeCommand(*fixture.server));
    }
    CHECK(request(fixture.instance, "command.get", {{"requestKey", "invalid"}}).contains("error"));
}

TEST_CASE("scene save retries replay success without writing the destination again")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    woby::UiState state;
    woby::SceneDocument clean = woby::createSceneDocument(state);
    woby::setShowGrid(state, true);
    std::optional<std::filesystem::path> currentPath;
    const auto path = fixture.directory / "saved.woby";
    const Json params = {{"path", woby::pathToUtf8(path)}, {"requestKey", "save-once"}};
    auto future = std::async(std::launch::async, [&] { return request(fixture.instance, "scene.save-as", params); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    const auto& payload = std::get<woby::SceneLifecycleCommand>(command->payload);
    REQUIRE_FALSE(woby::beginSceneLifecycle(payload, state, currentPath, clean, false));
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationSceneResult{currentPath, state.isDirty}));
    const auto response = future.get();
    REQUIRE(response.contains("result"));
    CHECK(response["result"]["dirty"] == false);
    std::ofstream(path) << "external edit";
    const auto replay = request(fixture.instance, "scene.save-as", params);
    CHECK(replay == response);
    CHECK_FALSE(takeCommand(*fixture.server));
    std::ifstream stream(path);
    std::string content;
    std::getline(stream, content);
    CHECK(content == "external edit");
    auto conflict = params;
    conflict["overwrite"] = true;
    CHECK(request(fixture.instance, "scene.save-as", conflict)["error"]["code"] == -32006);
    CHECK(request(fixture.instance, "scene.open", params)["error"]["code"] == -32006);
}

TEST_CASE("lifecycle errors replay their original dirty state and actionable reason")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    woby::UiState state;
    woby::SceneDocument clean = woby::createSceneDocument(state);
    woby::setShowGrid(state, true);
    std::optional<std::filesystem::path> path;
    const Json params = {{"requestKey", "new-failed"}};
    auto future = std::async(std::launch::async, [&] { return request(fixture.instance, "scene.new", params); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    const auto error = woby::beginSceneLifecycle(std::get<woby::SceneLifecycleCommand>(command->payload), state, path, clean, false);
    REQUIRE(error);
    REQUIRE(completeCommand(*fixture.server, command->id,
        woby::AutomationCommandError{error->message, error->code, error, path, state.isDirty}));
    const auto response = future.get();
    CHECK(response["error"]["data"]["reason"] == "dirty_scene");
    CHECK(response["error"]["data"]["dirty"] == true);
    CHECK(response["error"]["data"]["path"].is_null());
    state = woby::prepareSceneReplacement(state, {}, {});
    CHECK(request(fixture.instance, "scene.new", params) == response);
    CHECK_FALSE(takeCommand(*fixture.server));
}

TEST_CASE("open timeout retains the FIFO slot and later completion can be recovered")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const Json params = {{"path", woby::pathToUtf8(fixture.directory / "scene.woby")},
        {"timeoutSeconds", 1}, {"requestKey", "open-slow"}};
    auto future = std::async(std::launch::async, [&] { return request(fixture.instance, "scene.open", params); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    const auto timeout = future.get();
    CHECK(timeout["error"]["code"] == -32003);
    CHECK(timeout["error"]["data"]["state"] == "running");
    CHECK(request(fixture.instance, "scene.open", params)["error"]["code"] == -32008);
    auto later = std::async(std::launch::async, [&] { return request(fixture.instance, "objects.list"); });
    REQUIRE(waitForQueued(fixture, 1));
    CHECK_FALSE(takeCommand(*fixture.server));
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationSceneResult{fixture.directory / "scene.woby"}));
    CHECK(request(fixture.instance, "scene.open", params)["result"]["state"] == "succeeded");
    auto conflict = params;
    conflict["onDirty"] = "discard";
    CHECK(request(fixture.instance, "scene.open", conflict)["error"]["code"] == -32006);
    const auto next = waitForCommand(*fixture.server);
    REQUIRE(next);
    REQUIRE(completeCommand(*fixture.server, next->id, woby::AutomationObjectsResult{}));
    CHECK(later.get().contains("result"));
}

TEST_CASE("quit acknowledges shutdown and prevents later queued commands from executing")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    const Json params = {{"onDirty", "discard"}, {"requestKey", "quit-once"}};
    auto future = std::async(std::launch::async, [&] { return request(fixture.instance, "quit", params); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    auto later = std::async(std::launch::async, [&] { return request(fixture.instance, "scene.new"); });
    REQUIRE(waitForQueued(fixture, 1));
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationSceneResult{{}, true, true}));
    const auto response = future.get();
    CHECK(response["result"]["quitAccepted"] == true);
    CHECK(later.get()["error"]["code"] == -32001);
    CHECK_FALSE(takeCommand(*fixture.server));
    CHECK(request(fixture.instance, "quit", params) == response);
    CHECK(request(fixture.instance, "scene.new")["error"]["code"] == -32001);
}

TEST_CASE("completed quit response drains even when the server is stopped immediately")
{
    AutomationFixture fixture;
    woby::setAutomationReady(*fixture.server);
    auto future = std::async(std::launch::async, [&] { return request(fixture.instance, "quit"); });
    const auto command = waitForCommand(*fixture.server);
    REQUIRE(command);
    REQUIRE(completeCommand(*fixture.server, command->id, woby::AutomationSceneResult{{}, false, true}));
    fixture.server.reset();
    CHECK(future.get()["result"]["quitAccepted"] == true);
}
