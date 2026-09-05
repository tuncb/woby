#include "automation.h"
#include "automation_registry.h"
#include "utf8_path.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace woby {

struct AutomationRequest {
    AutomationCommand command;
    std::chrono::steady_clock::time_point deadline;
    bool started = false;
    bool finished = false;
    nlohmann::json response;
};

struct AutomationRuntime {
    AutomationRegistration registration;
    httplib::Server server;
    std::thread listener;
    std::atomic<bool> ready = false;
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    AutomationCommandId nextCommandId = 1;
    std::shared_ptr<AutomationRequest> request;
};

namespace {

using Json = nlohmann::json;

Json rpcError(const Json& id, int code, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

Json rpcResult(const Json& id, const Json& result)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

Json instanceInfo(const AutomationRuntime& runtime)
{
    const auto& instance = runtime.registration.instance;
    return {{"id", instance.id}, {"pid", instance.pid}, {"apiVersion", 1},
        {"url", "http://127.0.0.1:" + std::to_string(instance.port) + "/rpc"},
        {"ready", runtime.ready.load()}};
}

struct AutomationCommandMessages {
    const char* busy;
    const char* timedOut;
    const char* expired;
};

AutomationCommandMessages commandMessages(const AutomationScreenshotCommand&)
{
    return {
        "A screenshot is already pending.",
        "Screenshot timed out; a capture already in progress may still be saved.",
        "Screenshot timed out before capture.",
    };
}

Json commandResponse(const AutomationRuntime& runtime, const AutomationScreenshotResult& result)
{
    return rpcResult(nullptr, {{"instance", runtime.registration.instance.id}, {"path", pathToUtf8(result.savedPath)}});
}

Json commandResponse(const AutomationRuntime&, const AutomationCommandError& error)
{
    return rpcError(nullptr, -32004, error.message);
}

Json submitAutomationCommand(
    AutomationRuntime& runtime,
    const Json& id,
    AutomationCommandPayload payload,
    int timeoutSeconds)
{
    std::unique_lock lock(runtime.mutex);
    if (runtime.stopping || !runtime.ready.load()) {
        return rpcError(id, -32001, "Instance is not ready.");
    }
    if (runtime.request) {
        const auto messages = std::visit([](const auto& command) { return commandMessages(command); }, runtime.request->command.payload);
        return rpcError(id, -32002, messages.busy);
    }
    auto request = std::make_shared<AutomationRequest>();
    request->command = {runtime.nextCommandId++, std::move(payload)};
    request->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    const auto messages = std::visit([](const auto& command) { return commandMessages(command); }, request->command.payload);
    runtime.request = request;
    if (!runtime.changed.wait_until(lock, request->deadline, [&] { return request->finished || runtime.stopping; })) {
        // Started work may still finish after the HTTP deadline. Reserve its slot
        // until the main thread completes that command, so results cannot cross requests.
        if (!request->started && runtime.request == request) {
            runtime.request.reset();
        }
        return rpcError(id, -32003, messages.timedOut);
    }
    if (runtime.stopping) {
        return rpcError(id, -32001, "Instance is shutting down.");
    }
    Json response = request->response;
    response["id"] = id;
    return response;
}

Json captureScreenshot(AutomationRuntime& runtime, const Json& id, const Json& params)
{
    if (!params.contains("path") || !params["path"].is_string()) {
        return rpcError(id, -32602, "Screenshot requires an absolute output path.");
    }
    for (const auto& item : params.items()) {
        if (item.key() != "path" && item.key() != "timeoutSeconds") {
            return rpcError(id, -32602, "Unknown screenshot parameter: " + item.key());
        }
    }
    const auto pathText = params["path"].get<std::string>();
    if (pathText.empty() || pathText.size() > 8192u || pathText.find('\0') != std::string::npos) {
        return rpcError(id, -32602, "Invalid screenshot output path.");
    }
    const auto path = pathFromUtf8(pathText);
    if (!path.is_absolute() || path.filename().empty()) {
        return rpcError(id, -32602, "Screenshot requires an absolute output filename.");
    }
    int timeoutSeconds = 60;
    if (params.contains("timeoutSeconds")) {
        const auto& timeout = params["timeoutSeconds"];
        if (!timeout.is_number_integer() || timeout < 1 || timeout > 3600) {
            return rpcError(id, -32602, "timeoutSeconds must be an integer from 1 to 3600.");
        }
        timeoutSeconds = timeout.get<int>();
    }
    return submitAutomationCommand(runtime, id, AutomationScreenshotCommand{path.lexically_normal()}, timeoutSeconds);
}

void handleRpc(AutomationRuntime& runtime, const httplib::Request& request, httplib::Response& response)
{
    response.set_header("Cache-Control", "no-store");
    const auto& instance = runtime.registration.instance;
    const std::string expectedHost = "127.0.0.1:" + std::to_string(instance.port);
    // Browsers are not clients of this first API. Do not accept browser origins or CORS.
    if (request.has_header("Origin") || request.get_header_value("Host") != expectedHost) {
        response.status = 403;
        response.set_content("Forbidden", "text/plain");
        return;
    }
    if (request.get_header_value("Authorization") != "Bearer " + instance.token) {
        response.status = 401;
        response.set_content("Unauthorized", "text/plain");
        return;
    }
    const auto contentType = request.get_header_value("Content-Type");
    if (contentType != "application/json" && contentType.rfind("application/json;", 0u) != 0u) {
        response.status = 415;
        response.set_content("Expected application/json", "text/plain");
        return;
    }
    Json id = nullptr;
    Json result;
    try {
        const auto rpc = Json::parse(request.body, nullptr, false);
        if (rpc.is_discarded()) {
            result = rpcError(nullptr, -32700, "Invalid JSON.");
        } else if (!rpc.is_object() || !rpc.contains("jsonrpc") || rpc["jsonrpc"] != "2.0"
                   || !rpc.contains("method") || !rpc["method"].is_string()
                   || (rpc.contains("id") && !rpc["id"].is_string() && !rpc["id"].is_number_integer() && !rpc["id"].is_null())) {
            result = rpcError(nullptr, -32600, "Expected one JSON-RPC 2.0 request.");
        } else if (!rpc.contains("id")) {
            // This API requires acknowledged commands. Notifications perform no work.
            response.status = 204;
            return;
        } else {
            id = rpc["id"];
            const auto params = rpc.value("params", Json::object());
            const auto method = rpc["method"].get<std::string>();
            if (!params.is_object()) {
                result = rpcError(id, -32602, "params must be an object.");
            } else if (method == "instance.info") {
                result = params.empty() ? rpcResult(id, instanceInfo(runtime))
                                        : rpcError(id, -32602, "instance.info takes no parameters.");
            } else if (method == "screenshot.capture") {
                result = captureScreenshot(runtime, id, params);
            } else {
                result = rpcError(id, -32601, "Unknown method: " + method);
            }
        }
    } catch (const std::exception& error) {
        result = rpcError(id, -32603, error.what());
    }
    response.set_content(result.dump(), "application/json");
}

Json callInstance(const AutomationInstance& instance, const std::string& method, const Json& params, int timeoutSeconds)
{
    httplib::Client client("127.0.0.1", instance.port);
    client.set_connection_timeout(1, 0);
    client.set_read_timeout(timeoutSeconds, 0);
    client.set_write_timeout(5, 0);
    const Json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
    const httplib::Headers headers = {{"Authorization", "Bearer " + instance.token}};
    const auto response = client.Post("/rpc", headers, request.dump(), "application/json");
    if (!response) {
        throw std::runtime_error("Cannot contact instance '" + instance.id + "': " + httplib::to_string(response.error()));
    }
    if (response->status != 200) {
        throw std::runtime_error("Instance '" + instance.id + "' returned HTTP " + std::to_string(response->status) + ".");
    }
    const auto body = Json::parse(response->body);
    if (!body.is_object() || body.value("jsonrpc", "") != "2.0" || !body.contains("id") || body["id"] != 1) {
        throw std::runtime_error("Invalid response from instance '" + instance.id + "'.");
    }
    if (body.contains("error")) {
        throw std::runtime_error(body["error"].value("message", "Automation command failed."));
    }
    return body.at("result");
}

Json verifyInstance(const AutomationInstance& instance)
{
    const auto info = callInstance(instance, "instance.info", Json::object(), 1);
    if (info.at("id") != instance.id || info.at("pid") != instance.pid || info.at("apiVersion") != 1) {
        throw std::runtime_error("Instance identity changed; run 'woby ctl instances' again.");
    }
    return info;
}

} // namespace

AutomationOwner startAutomation(const std::optional<std::string>& instanceId, const std::filesystem::path& registryDirectory)
{
    AutomationOwner runtime(new AutomationRuntime, &stopAutomation);
    const auto directory = registryDirectory.empty() ? automationRegistryDirectory() : registryDirectory;
    reserveAutomationInstance(runtime->registration, directory, instanceId.value_or("woby-" + automationRandomHex(8u)));
    auto* pointer = runtime.get();
    runtime->server.new_task_queue = [] { return new httplib::ThreadPool(4, 4, 16); };
    runtime->server.set_payload_max_length(16384u);
    runtime->server.set_read_timeout(5, 0);
    runtime->server.set_write_timeout(5, 0);
    runtime->server.set_keep_alive_max_count(1);
    runtime->server.Post("/rpc", [pointer](const auto& request, auto& response) {
        handleRpc(*pointer, request, response);
    });
    const int port = runtime->server.bind_to_any_port("127.0.0.1");
    if (port < 1) {
        throw std::runtime_error("Cannot bind Woby's local automation server.");
    }
    runtime->registration.instance.port = port;
    runtime->listener = std::thread([pointer] { pointer->server.listen_after_bind(); });
    runtime->server.wait_until_ready();
    if (!runtime->server.is_running()) {
        throw std::runtime_error("Cannot start Woby's local automation server.");
    }
    publishAutomationInstance(runtime->registration);
    return runtime;
}

void stopAutomation(AutomationRuntime* runtime)
{
    if (!runtime) {
        return;
    }
    {
        std::lock_guard lock(runtime->mutex);
        runtime->stopping = true;
        runtime->ready.store(false);
    }
    runtime->changed.notify_all();
    runtime->server.stop();
    if (runtime->listener.joinable()) {
        runtime->listener.join();
    }
    releaseAutomationInstance(runtime->registration);
    delete runtime;
}

const std::string& automationInstanceId(const AutomationRuntime& runtime)
{
    return runtime.registration.instance.id;
}

void setAutomationReady(AutomationRuntime& runtime)
{
    runtime.ready.store(true);
}

std::optional<AutomationCommand> takeAutomationCommand(AutomationRuntime& runtime)
{
    std::lock_guard lock(runtime.mutex);
    if (!runtime.request || runtime.request->started || runtime.stopping) {
        return {};
    }
    if (std::chrono::steady_clock::now() >= runtime.request->deadline) {
        const auto messages = std::visit([](const auto& command) { return commandMessages(command); }, runtime.request->command.payload);
        runtime.request->response = rpcError(nullptr, -32003, messages.expired);
        runtime.request->finished = true;
        runtime.request.reset();
        runtime.changed.notify_all();
        return {};
    }
    const auto command = runtime.request->command;
    runtime.request->started = true;
    return command;
}

bool completeAutomationCommand(AutomationRuntime& runtime, AutomationCommandId id, const AutomationCommandResult& result)
{
    std::lock_guard lock(runtime.mutex);
    if (runtime.stopping || !runtime.request || !runtime.request->started || runtime.request->command.id != id) {
        return false;
    }
    runtime.request->response = std::visit([&](const auto& value) { return commandResponse(runtime, value); }, result);
    runtime.request->finished = true;
    runtime.request.reset();
    runtime.changed.notify_all();
    return true;
}

int runAutomationCommand(const ControlArguments& arguments, const std::filesystem::path& registryDirectory)
{
    try {
        const auto directory = registryDirectory.empty() ? automationRegistryDirectory() : registryDirectory;
        if (arguments.command == ControlCommand::instances) {
            Json instances = Json::array();
            for (const auto& instance : readAutomationInstances(directory)) {
                try {
                    instances.push_back(verifyInstance(instance));
                } catch (const std::exception&) {
                    // Ignore crashed instances and ports reused by unrelated processes.
                }
            }
            if (arguments.json) {
                std::printf("%s\n", instances.dump().c_str());
            } else if (instances.empty()) {
                std::printf("No running Woby instances.\n");
            } else {
                for (const auto& instance : instances) {
                    std::printf("%s  pid=%llu  %s  %s\n", instance.at("id").get<std::string>().c_str(),
                        static_cast<unsigned long long>(instance.at("pid").get<uint64_t>()),
                        instance.at("ready").get<bool>() ? "ready" : "starting",
                        instance.at("url").get<std::string>().c_str());
                }
            }
            return 0;
        }
        if (arguments.command != ControlCommand::screenshot || !arguments.instanceId) {
            throw std::runtime_error("Expected a control command.");
        }
        const auto instance = readAutomationInstance(directory, *arguments.instanceId);
        verifyInstance(instance);
        const auto outputPath = std::filesystem::absolute(arguments.outputPath).lexically_normal();
        const auto result = callInstance(instance, "screenshot.capture",
            {{"path", pathToUtf8(outputPath)}, {"timeoutSeconds", arguments.timeoutSeconds}}, arguments.timeoutSeconds + 5);
        if (arguments.json) {
            std::printf("%s\n", result.dump().c_str());
        } else {
            std::printf("Saved screenshot %s (instance %s)\n", result.at("path").get<std::string>().c_str(), instance.id.c_str());
        }
        return 0;
    } catch (const std::exception& error) {
        if (arguments.json) {
            const Json result = {{"error", error.what()}};
            std::printf("%s\n", result.dump().c_str());
        } else {
            std::fprintf(stderr, "%s\n", error.what());
        }
        return 1;
    }
}

void printCommandLineHelp()
{
    std::printf(
        "Usage:\n"
        "  woby [--instance ID] [--scene PATH] [--file PATH ...]\n"
        "  woby ctl instances [--json]\n"
        "  woby ctl --instance ID screenshot PATH [--timeout SECONDS] [--json]\n"
        "\nEvery viewer instance starts a local HTTP API and displays its ID in the title.\n"
        "IDs: 1-64 lowercase letters, digits, '-' or '_'; start with a letter or digit.\n"
        "Screenshot waits for the PNG to be saved (default timeout: 60 seconds).\n"
        "--wait is also accepted; waiting is always enabled. Existing PNGs are overwritten.\n"
        "Other startup options: --folder, --folder-tree, --woby, --plugin, --plugin-folder,\n"
        "--log-level, --log-file, --log-performance, --log-frame-interval, --log-slow-frame-ms,\n"
        "--version, --help. See README.md for details.\n");
}

} // namespace woby
