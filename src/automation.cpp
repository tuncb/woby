#include "automation.h"
#include "automation_registry.h"
#include "utf8_path.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
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
    std::string requestKey;
    size_t responseBytes = 0;
    nlohmann::json response;
};

struct AutomationKeyRecord {
    AutomationCommandId id = 0;
    std::string fingerprint;
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
    std::string objectIdPrefix;
    std::string commandIdPrefix;
    std::deque<std::shared_ptr<AutomationRequest>> pending;
    std::shared_ptr<AutomationRequest> request;
    std::deque<std::shared_ptr<AutomationRequest>> history;
    size_t historyBytes = 0;
    // Never forget used keys within a session, even after their result is evicted.
    std::map<std::string, AutomationKeyRecord> requestKeys;
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

Json instanceInfo(AutomationRuntime& runtime)
{
    std::lock_guard lock(runtime.mutex);
    const auto& instance = runtime.registration.instance;
    return {{"id", instance.id}, {"pid", instance.pid}, {"apiVersion", 1},
        {"url", "http://127.0.0.1:" + std::to_string(instance.port) + "/rpc"},
        {"ready", runtime.ready.load()}, {"queuedCommands", runtime.pending.size()},
        {"activeSequence", runtime.request ? Json(std::to_string(runtime.request->command.id)) : Json(nullptr)}};
}

struct AutomationCommandMessages {
    const char* timedOut;
    const char* expired;
};

AutomationCommandMessages commandMessages(const AutomationScreenshotCommand&)
{
    return {
        "Screenshot timed out; a capture already in progress may still be saved.",
        "Screenshot timed out before capture.",
    };
}

AutomationCommandMessages commandMessages(const AutomationObjectsCommand&)
{
    return {"Object query timed out.", "Object query timed out before execution."};
}

AutomationCommandMessages commandMessages(const AutomationObjectCommand&)
{
    return commandMessages(AutomationObjectsCommand{});
}

AutomationCommandMessages commandMessages(const SceneLifecycleCommand&)
{
    return {"Scene command timed out; started work may still complete.", "Scene command timed out before execution."};
}

AutomationCommandMessages commandMessages(const ControlOperation&)
{
    return {"Control command timed out; started work may still complete.", "Control command timed out before execution."};
}

std::string publicObjectId(const AutomationRuntime& runtime, SceneObjectId id)
{
    char suffix[17];
    std::snprintf(suffix, sizeof(suffix), "%016llx", static_cast<unsigned long long>(id));
    return runtime.objectIdPrefix + suffix;
}

Json objectInfo(const AutomationRuntime& runtime, const SceneObjectInfo& object)
{
    const char* kind = "folder";
    switch (object.kind) {
    case SceneObjectKind::folder: break;
    case SceneObjectKind::file: kind = "file"; break;
    case SceneObjectKind::group: kind = "group"; break;
    case SceneObjectKind::comparison: kind = "comparison"; break;
    }
    Json result = {{"id", publicObjectId(runtime, object.id)}, {"kind", kind}, {"name", object.name}};
    if (object.kind == SceneObjectKind::file) {
        result["path"] = pathToUtf8(object.path);
    } else if (object.kind == SceneObjectKind::group) {
        result["fileId"] = publicObjectId(runtime, object.fileId);
    }
    return result;
}

Json commandResponse(const AutomationRuntime& runtime, const AutomationScreenshotResult& result)
{
    return rpcResult(nullptr, {{"instance", runtime.registration.instance.id}, {"path", pathToUtf8(result.savedPath)}});
}

Json commandResponse(const AutomationRuntime& runtime, const AutomationObjectsResult& result)
{
    Json objects = Json::array();
    for (const auto& object : result.objects) {
        objects.push_back(objectInfo(runtime, object));
    }
    return rpcResult(nullptr, {{"instance", runtime.registration.instance.id}, {"objects", std::move(objects)}});
}

Json commandResponse(const AutomationRuntime& runtime, const AutomationObjectResult& result)
{
    auto object = objectInfo(runtime, result.object);
    object.update(result.details);
    return rpcResult(nullptr, {{"instance", runtime.registration.instance.id}, {"object", object}});
}

Json commandResponse(const AutomationRuntime& runtime, const AutomationControlResult& result)
{
    auto value = result.value;
    value["instance"] = runtime.registration.instance.id;
    return rpcResult(nullptr, value);
}

Json commandResponse(const AutomationRuntime& runtime, const AutomationSceneResult& result)
{
    Json value = {{"instance", runtime.registration.instance.id},
        {"path", result.path ? Json(pathToUtf8(*result.path)) : Json(nullptr)}, {"dirty", result.dirty}};
    if (result.quitAccepted) { value["quitAccepted"] = true; }
    return rpcResult(nullptr, value);
}

Json commandResponse(const AutomationRuntime&, const AutomationCommandError& error)
{
    auto response = rpcError(nullptr, error.code, error.message);
    if (error.lifecycle) {
        response["error"]["data"] = {{"reason", error.lifecycle->reason},
            {"path", error.scenePath ? Json(pathToUtf8(*error.scenePath)) : Json(nullptr)},
            {"dirty", error.dirty}};
    }
    return response;
}

Json completedResponse(const AutomationRuntime& runtime, const AutomationCommand& command,
    const AutomationCommandResult& result)
{
    auto response = std::visit([&](const auto& value) { return commandResponse(runtime, value); }, result);
    auto& metadata = response.contains("error") ? response["error"]["data"] : response["result"];
    metadata["sequence"] = std::to_string(command.id);
    metadata["commandId"] = runtime.commandIdPrefix + std::to_string(command.id);
    return response;
}

const char* commandState(const AutomationRequest& request)
{
    if (!request.finished) {
        return request.started ? "running" : "queued";
    }
    if (!request.started) {
        return "expired-before-start";
    }
    return request.response.contains("error") ? "failed" : "succeeded";
}

Json commandMetadata(const AutomationRuntime& runtime, const AutomationRequest& request)
{
    Json result = {{"sequence", std::to_string(request.command.id)},
        {"commandId", runtime.commandIdPrefix + std::to_string(request.command.id)},
        {"state", commandState(request)}};
    if (!request.requestKey.empty()) {
        result["requestKey"] = request.requestKey;
    }
    return result;
}

// All request/history helpers run under the runtime mutex.
void finishRequest(AutomationRuntime& runtime, const std::shared_ptr<AutomationRequest>& request, Json response)
{
    request->response = std::move(response);
    request->finished = true;
    auto& metadata = request->response.contains("error") ? request->response["error"]["data"] : request->response["result"];
    metadata.update(commandMetadata(runtime, *request));
    request->responseBytes = request->response.dump().size();
    if (request->responseBytes > maxAutomationHistoryBytes) {
        runtime.changed.notify_all();
        return;
    }
    runtime.history.push_back(request);
    runtime.historyBytes += request->responseBytes;
    while (runtime.history.size() > maxAutomationHistory || runtime.historyBytes > maxAutomationHistoryBytes) {
        runtime.historyBytes -= runtime.history.front()->responseBytes;
        runtime.history.pop_front();
    }
    runtime.changed.notify_all();
}

void expireQueuedRequests(AutomationRuntime& runtime)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = runtime.pending.begin(); it != runtime.pending.end();) {
        const auto request = *it;
        if (now < request->deadline) {
            ++it;
            continue;
        }
        it = runtime.pending.erase(it);
        const auto messages = std::visit([](const auto& command) { return commandMessages(command); }, request->command.payload);
        finishRequest(runtime, request, completedResponse(runtime, request->command, AutomationCommandError{messages.expired, -32003}));
    }
}

std::shared_ptr<AutomationRequest> findRequest(const AutomationRuntime& runtime, AutomationCommandId id)
{
    if (runtime.request && runtime.request->command.id == id) {
        return runtime.request;
    }
    for (const auto& request : runtime.pending) {
        if (request->command.id == id) {
            return request;
        }
    }
    for (const auto& request : runtime.history) {
        if (request->command.id == id) {
            return request;
        }
    }
    return {};
}

Json unavailableCommand(const AutomationRuntime& runtime, const Json& id, AutomationCommandId commandId)
{
    auto response = rpcError(id, -32009, "Command result is no longer retained; its outcome is unknown. Do not blindly repeat it.");
    response["error"]["data"] = {{"sequence", std::to_string(commandId)},
        {"commandId", runtime.commandIdPrefix + std::to_string(commandId)}, {"state", "unavailable"}};
    return response;
}

Json queryCommand(AutomationRuntime& runtime, const Json& id, const Json& params)
{
    if (params.size() != 1 || (!params.contains("id") && !params.contains("requestKey"))) {
        return rpcError(id, -32602, "command.get requires exactly one of id or requestKey.");
    }
    std::lock_guard lock(runtime.mutex);
    expireQueuedRequests(runtime);
    AutomationCommandId commandId = 0;
    if (params.contains("requestKey")) {
        if (!params["requestKey"].is_string() || !validAutomationRequestKey(params["requestKey"].get<std::string>())) {
            return rpcError(id, -32602, "Invalid requestKey.");
        }
        const auto key = runtime.requestKeys.find(params["requestKey"].get<std::string>());
        if (key == runtime.requestKeys.end()) {
            return rpcError(id, -32007, "Unknown request key in this viewer session.");
        }
        commandId = key->second.id;
    } else {
        if (!params["id"].is_string()) {
            return rpcError(id, -32602, "Command id must be a string returned by a scene command.");
        }
        const auto text = params["id"].get<std::string>();
        // Command IDs include a random launch prefix; a restart cannot alias old IDs.
        if (!text.starts_with(runtime.commandIdPrefix)) {
            return rpcError(id, -32007, "Unknown command ID in this viewer session.");
        }
        const auto* begin = text.data() + runtime.commandIdPrefix.size();
        const auto* end = text.data() + text.size();
        const auto parsed = std::from_chars(begin, end, commandId);
        if (parsed.ec != std::errc{} || parsed.ptr != end || commandId == 0
            || text != runtime.commandIdPrefix + std::to_string(commandId)) {
            return rpcError(id, -32602, "Invalid command ID.");
        }
        if (runtime.nextCommandId != 0 && commandId >= runtime.nextCommandId) {
            return rpcError(id, -32007, "Unknown command ID in this viewer session.");
        }
    }
    const auto request = findRequest(runtime, commandId);
    if (!request) {
        return unavailableCommand(runtime, id, commandId);
    }
    auto result = commandMetadata(runtime, *request);
    result["instance"] = runtime.registration.instance.id;
    if (request->finished) {
        const char* field = request->response.contains("error") ? "error" : "result";
        result[field] = request->response[field];
    }
    return rpcResult(id, result);
}

bool resultMatches(const AutomationScreenshotCommand&, const AutomationCommandResult& result)
{
    return std::holds_alternative<AutomationScreenshotResult>(result);
}

bool resultMatches(const AutomationObjectsCommand&, const AutomationCommandResult& result)
{
    return std::holds_alternative<AutomationObjectsResult>(result);
}

bool resultMatches(const AutomationObjectCommand&, const AutomationCommandResult& result)
{
    return std::holds_alternative<AutomationObjectResult>(result);
}

Json commandFingerprint(const AutomationScreenshotCommand& command)
{
    return Json::array({"screenshot.capture", pathToUtf8(command.outputPath)});
}

Json commandFingerprint(const AutomationObjectsCommand&)
{
    return Json::array({"objects.list"});
}

Json commandFingerprint(const AutomationObjectCommand& command)
{
    return Json::array({"object.get", command.objectId});
}

bool resultMatches(const SceneLifecycleCommand& command, const AutomationCommandResult& result)
{
    const auto* scene = std::get_if<AutomationSceneResult>(&result);
    return scene && scene->quitAccepted == (command.action == SceneAction::quit);
}

Json commandFingerprint(const SceneLifecycleCommand& command)
{
    return Json::array({"scene-lifecycle", static_cast<int>(command.action), pathToUtf8(command.path),
        static_cast<int>(command.onDirty), pathToUtf8(command.savePath), command.overwrite});
}

bool resultMatches(const ControlOperation&, const AutomationCommandResult& result)
{
    return std::holds_alternative<AutomationControlResult>(result);
}

Json commandFingerprint(const ControlOperation& command)
{
    return Json::array({controlMethod(command.action).method, controlOperationParams(command)});
}

Json submitAutomationCommand(
    AutomationRuntime& runtime,
    const Json& id,
    AutomationCommandPayload payload,
    int timeoutSeconds,
    const Json& params)
{
    std::string requestKey;
    if (params.contains("requestKey")) {
        if (!params["requestKey"].is_string() || !validAutomationRequestKey(params["requestKey"].get<std::string>())) {
            return rpcError(id, -32602, "requestKey must be 1-128 ASCII letters, digits, '-', '_', '.', or ':'.");
        }
        requestKey = params["requestKey"].get<std::string>();
    }
    const auto fingerprint = std::visit([](const auto& command) { return commandFingerprint(command).dump(); }, payload);
    std::unique_lock lock(runtime.mutex);
    expireQueuedRequests(runtime);
    if (!requestKey.empty()) {
        const auto key = runtime.requestKeys.find(requestKey);
        if (key != runtime.requestKeys.end()) {
            if (key->second.fingerprint != fingerprint) {
                auto response = rpcError(id, -32006, "requestKey was already used for different command parameters.");
                response["error"]["data"] = {{"requestKey", requestKey},
                    {"commandId", runtime.commandIdPrefix + std::to_string(key->second.id)},
                    {"sequence", std::to_string(key->second.id)}};
                return response;
            }
            const auto existing = findRequest(runtime, key->second.id);
            if (!existing) {
                return unavailableCommand(runtime, id, key->second.id);
            }
            if (existing->finished) {
                auto response = existing->response;
                response["id"] = id;
                return response;
            }
            // Duplicate waiters must not exhaust the HTTP pool needed for status.
            auto response = rpcError(id, -32008, "Command is already in progress; use command.get to inspect it.");
            response["error"]["data"] = commandMetadata(runtime, *existing);
            return response;
        }
        if (runtime.requestKeys.size() >= maxAutomationRequestKeys) {
            return rpcError(id, -32010, "Viewer session request-key capacity reached; no command was admitted.");
        }
    }
    if (runtime.stopping || !runtime.ready.load()) {
        return rpcError(id, -32001, "Instance is not ready.");
    }
    if (runtime.pending.size() + (runtime.request ? 1u : 0u) >= maxAutomationCommands) {
        return rpcError(id, -32002, "Automation command queue is full.");
    }
    if (runtime.nextCommandId == 0) {
        return rpcError(id, -32603, "Automation command sequences exhausted.");
    }
    auto request = std::make_shared<AutomationRequest>();
    request->command = {runtime.nextCommandId++, std::move(payload)};
    request->requestKey = requestKey;
    request->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    const auto messages = std::visit([](const auto& command) { return commandMessages(command); }, request->command.payload);
    runtime.pending.push_back(request);
    if (!requestKey.empty()) {
        runtime.requestKeys.emplace(requestKey, AutomationKeyRecord{request->command.id, fingerprint});
    }
    if (!runtime.changed.wait_until(lock, request->deadline, [&] { return request->finished || runtime.stopping; })) {
        // Started work may still finish after the HTTP deadline. Reserve its slot
        // until the main thread completes that command, so results cannot cross requests.
        if (!request->started) {
            expireQueuedRequests(runtime);
            auto response = request->response;
            response["id"] = id;
            return response;
        }
        auto response = rpcError(id, -32003, messages.timedOut);
        response["error"]["data"] = commandMetadata(runtime, *request);
        return response;
    }
    if (runtime.stopping && !request->finished) {
        auto response = rpcError(id, -32001, "Instance is shutting down; command outcome may be unknown.");
        response["error"]["data"] = commandMetadata(runtime, *request);
        return response;
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
        if (item.key() != "path" && item.key() != "timeoutSeconds" && item.key() != "requestKey") {
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
    return submitAutomationCommand(runtime, id, AutomationScreenshotCommand{path.lexically_normal()}, timeoutSeconds, params);
}

Json queryScene(AutomationRuntime& runtime, const Json& id, const Json& params, const std::string& method)
{
    const bool singleObject = method == "object.get";
    for (const auto& item : params.items()) {
        if (item.key() != "timeoutSeconds" && item.key() != "requestKey" && !(singleObject && item.key() == "id")) {
            return rpcError(id, -32602, "Unknown object query parameter: " + item.key());
        }
    }
    int timeoutSeconds = 60;
    if (params.contains("timeoutSeconds")) {
        const auto& timeout = params["timeoutSeconds"];
        if (!timeout.is_number_integer() || timeout < 1 || timeout > 3600) {
            return rpcError(id, -32602, "timeoutSeconds must be an integer from 1 to 3600.");
        }
        timeoutSeconds = timeout.get<int>();
    }
    if (!singleObject) {
        return submitAutomationCommand(runtime, id, AutomationObjectsCommand{}, timeoutSeconds, params);
    }
    if (!params.contains("id") || !params["id"].is_string()) {
        return rpcError(id, -32602, "object.get requires an object ID from objects.list.");
    }
    const auto text = params["id"].get<std::string>();
    constexpr size_t prefixLength = 37; // obj-<32 hex digits>-
    const auto isHex = [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); };
    if (text.size() != prefixLength + 16 || text.substr(0, 4) != "obj-" || text[36] != '-'
        || !std::all_of(text.begin() + 4, text.begin() + 36, isHex)
        || !std::all_of(text.begin() + prefixLength, text.end(), isHex)) {
        return rpcError(id, -32602, "Invalid object ID; use an ID returned by objects.list.");
    }
    SceneObjectId objectId = invalidSceneObjectId;
    const auto parsed = std::from_chars(text.data() + prefixLength, text.data() + text.size(), objectId, 16);
    if (parsed.ec != std::errc{} || objectId == invalidSceneObjectId) {
        return rpcError(id, -32602, "Invalid object ID; use an ID returned by objects.list.");
    }
    if (text.compare(0, prefixLength, runtime.objectIdPrefix) != 0) {
        return rpcError(id, -32005, "Unknown or stale object ID.");
    }
    return submitAutomationCommand(runtime, id, AutomationObjectCommand{objectId}, timeoutSeconds, params);
}

Json sceneLifecycle(AutomationRuntime& runtime, const Json& id, const Json& params, const std::string& method)
{
    SceneLifecycleCommand command;
    if (method == "scene.save-as") { command.action = SceneAction::saveAs; }
    else if (method == "scene.open") { command.action = SceneAction::open; }
    else if (method == "scene.new") { command.action = SceneAction::newScene; }
    else if (method == "quit") { command.action = SceneAction::quit; }
    const bool hasPath = command.action == SceneAction::saveAs || command.action == SceneAction::open;
    const bool destructive = command.action == SceneAction::open || command.action == SceneAction::newScene
        || command.action == SceneAction::quit;
    for (const auto& item : params.items()) {
        if (item.key() == "timeoutSeconds" || item.key() == "requestKey"
            || (hasPath && item.key() == "path")
            || (destructive && (item.key() == "onDirty" || item.key() == "savePath"))
            || ((destructive || command.action == SceneAction::saveAs) && item.key() == "overwrite")) { continue; }
        return rpcError(id, -32602, "Unknown scene command parameter: " + item.key());
    }
    auto parsePath = [&](const char* name, std::filesystem::path& path) {
        if (!params.contains(name) || !params[name].is_string()) { return false; }
        const auto value = params[name].get<std::string>();
        if (value.empty() || value.size() > 8192u || value.find('\0') != std::string::npos) { return false; }
        path = pathFromUtf8(value).lexically_normal();
        return path.is_absolute() && !path.filename().empty() && path.filename() != "." && path.filename() != "..";
    };
    if ((hasPath && !parsePath("path", command.path))
        || (params.contains("savePath") && !parsePath("savePath", command.savePath))) {
        return rpcError(id, -32602, "Scene paths must be absolute filenames.");
    }
    if (params.contains("onDirty")) {
        if (params["onDirty"] == "save") { command.onDirty = DirtyPolicy::save; }
        else if (params["onDirty"] == "discard") { command.onDirty = DirtyPolicy::discard; }
        else if (params["onDirty"] != "error") { return rpcError(id, -32602, "onDirty must be error, save, or discard."); }
    }
    if (!command.savePath.empty() && command.onDirty != DirtyPolicy::save) {
        return rpcError(id, -32602, "savePath requires onDirty: save.");
    }
    if (params.contains("overwrite")) {
        if (!params["overwrite"].is_boolean()
            || (command.action != SceneAction::saveAs && command.savePath.empty())) {
            return rpcError(id, -32602, "overwrite must be a boolean accompanying an explicit save destination.");
        }
        command.overwrite = params["overwrite"].get<bool>();
    }
    int timeoutSeconds = 60;
    if (params.contains("timeoutSeconds")) {
        const auto& timeout = params["timeoutSeconds"];
        if (!timeout.is_number_integer() || timeout < 1 || timeout > 3600) {
            return rpcError(id, -32602, "timeoutSeconds must be an integer from 1 to 3600.");
        }
        timeoutSeconds = timeout.get<int>();
    }
    return submitAutomationCommand(runtime, id, command, timeoutSeconds, params);
}

Json executeControlOperation(AutomationRuntime& runtime, const Json& id, Json params, const ControlMethod& method)
{
    const auto originalParams = params;
    int timeout = 60;
    if (params.contains("timeoutSeconds")) {
        if (!params["timeoutSeconds"].is_number_integer() || params["timeoutSeconds"] < 1 || params["timeoutSeconds"] > 3600) {
            return rpcError(id, -32602, "timeoutSeconds must be an integer from 1 to 3600.");
        }
        timeout = params["timeoutSeconds"].get<int>();
        params.erase("timeoutSeconds");
    }
    params.erase("requestKey");
    ControlOperation command;
    try { command = parseControlOperation(method, params); }
    catch (const std::exception& error) { return rpcError(id, -32602, error.what()); }
    for (const auto& [text, resolved] : {std::pair{command.target, &command.objectId},
        std::pair{command.a.value_or(""), &command.aId}, std::pair{command.b.value_or(""), &command.bId},
        std::pair{command.object.value_or(""), &command.memberId}}) {
        if (text.empty() || (resolved == &command.objectId && text == "scene")) { continue; }
        const auto isHex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
        if (text.size() != 53 || text.substr(0, 4) != "obj-" || text[36] != '-'
            || !std::all_of(text.begin() + 4, text.begin() + 36, isHex)
            || !std::all_of(text.begin() + 37, text.end(), isHex)) {
            return rpcError(id, -32602, "Invalid target; use scene or an ID returned by objects.list.");
        }
        const auto parsed = std::from_chars(text.data() + 37, text.data() + text.size(), *resolved, 16);
        if (parsed.ec != std::errc{} || *resolved == invalidSceneObjectId) { return rpcError(id, -32602, "Invalid target ID."); }
        if (text.compare(0, 37, runtime.objectIdPrefix) != 0) { return rpcError(id, -32005, "Unknown or stale object ID."); }
    }
    return submitAutomationCommand(runtime, id, command, timeout, originalParams);
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
            } else if (method == "scene.save" || method == "scene.save-as" || method == "scene.open"
                || method == "scene.new" || method == "quit") {
                result = sceneLifecycle(runtime, id, params, method);
            } else if (method == "command.get") {
                result = queryCommand(runtime, id, params);
            } else if (method == "objects.list" || method == "object.get") {
                result = queryScene(runtime, id, params, method);
            } else if (const auto* entry = findControlMethod(method)) {
                result = executeControlOperation(runtime, id, params, *entry);
            } else {
                result = rpcError(id, -32601, "Unknown method: " + method);
            }
        }
    } catch (const std::exception& error) {
        result = rpcError(id, -32603, error.what());
    }
    response.set_content(result.dump(), "application/json");
}

Json callInstance(const AutomationInstance& instance, const std::string& method, const Json& params,
    int timeoutSeconds, Json* rpcFailure = nullptr)
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
        if (rpcFailure) {
            *rpcFailure = body["error"];
        }
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

std::string automationObjectId(const AutomationRuntime& runtime, SceneObjectId id)
{
    return publicObjectId(runtime, id);
}

nlohmann::json automationInstanceInfo(AutomationRuntime& runtime)
{
    return instanceInfo(runtime);
}

AutomationOwner startAutomation(const std::optional<std::string>& instanceId, const std::filesystem::path& registryDirectory)
{
    AutomationOwner runtime(new AutomationRuntime, &stopAutomation);
    runtime->objectIdPrefix = "obj-" + automationRandomHex(16u) + "-";
    runtime->commandIdPrefix = "cmd-" + automationRandomHex(16u) + "-";
    const auto directory = registryDirectory.empty() ? automationRegistryDirectory() : registryDirectory;
    reserveAutomationInstance(runtime->registration, directory, instanceId.value_or("woby-" + automationRandomHex(8u)));
    auto* pointer = runtime.get();
    // Waiting scene commands must leave HTTP workers available for discovery and
    // rejecting excess work. Admission is bounded independently of the HTTP pool.
    runtime->server.new_task_queue = [] { return new httplib::ThreadPool(maxAutomationCommands + 4, maxAutomationCommands + 4, 32); };
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
    if (runtime.request || runtime.stopping) {
        return {};
    }
    expireQueuedRequests(runtime);
    if (!runtime.pending.empty()) {
        const auto request = runtime.pending.front();
        auto command = request->command;
        request->started = true;
        runtime.request = request;
        runtime.pending.pop_front();
        return command;
    }
    return {};
}

bool completeAutomationCommand(AutomationRuntime& runtime, AutomationCommandId id,
    const AutomationCommandResult& result)
{
    std::lock_guard lock(runtime.mutex);
    if (runtime.stopping || !runtime.request || !runtime.request->started || runtime.request->command.id != id) {
        return false;
    }
    if (!std::holds_alternative<AutomationCommandError>(result)
        && !std::visit([&](const auto& command) { return resultMatches(command, result); }, runtime.request->command.payload)) {
        return false;
    }
    finishRequest(runtime, runtime.request, completedResponse(runtime, runtime.request->command, result));
    runtime.request.reset();
    if (const auto* scene = std::get_if<AutomationSceneResult>(&result); scene && scene->quitAccepted) {
        runtime.stopping = true;
        runtime.ready.store(false);
    }
    runtime.changed.notify_all();
    return true;
}

int runAutomationCommand(const ControlArguments& arguments, const std::filesystem::path& registryDirectory)
{
    Json rpcFailure;
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
        if (arguments.command == ControlCommand::none || !arguments.instanceId) {
            throw std::runtime_error("Expected a control command.");
        }
        const auto instance = readAutomationInstance(directory, *arguments.instanceId);
        verifyInstance(instance);
        if (arguments.command == ControlCommand::command) {
            const Json lookup = arguments.requestKey ? Json{{"requestKey", *arguments.requestKey}}
                                                     : Json{{"id", arguments.commandId}};
            const auto result = callInstance(instance, "command.get", lookup, 5, &rpcFailure);
            std::printf("%s\n", result.dump(arguments.json ? -1 : 2).c_str());
            return 0;
        }
        Json params = {{"timeoutSeconds", arguments.timeoutSeconds}};
        if (arguments.requestKey) {
            params["requestKey"] = *arguments.requestKey;
        }
        if (arguments.command == ControlCommand::operation) {
            params.update(controlOperationParams(arguments.operation));
            const auto result = callInstance(instance, controlMethod(arguments.operation.action).method,
                params, arguments.timeoutSeconds + 5, &rpcFailure);
            std::printf("%s\n", result.dump(arguments.json ? -1 : 2).c_str());
            return 0;
        }
        if (arguments.command == ControlCommand::objects || arguments.command == ControlCommand::object) {
            if (arguments.command == ControlCommand::object) {
                params["id"] = arguments.objectId;
            }
            const auto result = callInstance(instance,
                arguments.command == ControlCommand::objects ? "objects.list"
                    : "object.get",
                params, arguments.timeoutSeconds + 5, &rpcFailure);
            std::printf("%s\n", result.dump(arguments.json ? -1 : 2).c_str());
            return 0;
        }
        if (arguments.command == ControlCommand::scene || arguments.command == ControlCommand::quit) {
            const auto& command = arguments.lifecycle;
            std::string method;
            switch (command.action) {
            case SceneAction::save: method = "scene.save"; break;
            case SceneAction::saveAs: method = "scene.save-as"; break;
            case SceneAction::open: method = "scene.open"; break;
            case SceneAction::newScene: method = "scene.new"; break;
            case SceneAction::quit: method = "quit"; break;
            }
            if (!command.path.empty()) {
                params["path"] = pathToUtf8(std::filesystem::absolute(command.path).lexically_normal());
            }
            if (command.action == SceneAction::open || command.action == SceneAction::newScene || command.action == SceneAction::quit) {
                params["onDirty"] = command.onDirty == DirtyPolicy::save ? "save"
                    : command.onDirty == DirtyPolicy::discard ? "discard" : "error";
            }
            if (!command.savePath.empty()) {
                params["savePath"] = pathToUtf8(std::filesystem::absolute(command.savePath).lexically_normal());
            }
            if (command.action == SceneAction::saveAs || !command.savePath.empty()) {
                params["overwrite"] = command.overwrite;
            }
            const auto result = callInstance(instance, method, params, arguments.timeoutSeconds + 5, &rpcFailure);
            std::printf("%s\n", result.dump(arguments.json ? -1 : 2).c_str());
            return 0;
        }
        const auto outputPath = std::filesystem::absolute(arguments.outputPath).lexically_normal();
        params["path"] = pathToUtf8(outputPath);
        const auto result = callInstance(instance, "screenshot.capture", params, arguments.timeoutSeconds + 5, &rpcFailure);
        if (arguments.json) {
            std::printf("%s\n", result.dump().c_str());
        } else {
            std::printf("Saved screenshot %s (instance %s)\n", result.at("path").get<std::string>().c_str(), instance.id.c_str());
        }
        return 0;
    } catch (const std::exception& error) {
        if (arguments.json) {
            Json result = {{"error", error.what()}};
            if (rpcFailure.contains("code")) {
                result["code"] = rpcFailure["code"];
            }
            if (rpcFailure.contains("data")) {
                result["data"] = rpcFailure["data"];
            }
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
        "  woby [STARTUP_OPTIONS]\n"
        "  woby ctl --instance ID COMMAND [OPTIONS]\n"
        "  woby update [--check | --status] [--json]\n"
        "    Update a writable portable deployment from GitHub; close its viewers first.\n"
        "    --check only checks versions. --status reports the last update.\n"
        "    Exit 0: complete/check succeeded; 1: failed; 2: helper still pending.\n"
        "  woby help | --help | -h\n"
        "  woby ctl help | --help | -h\n"
        "\nStartup options:\n"
        "  --instance ID              Choose the viewer's instance ID.\n"
        "  --scene PATH | --woby PATH Open one saved .woby scene.\n"
        "  --file PATH                Add a model; repeat for multiple files.\n"
        "  --folder PATH              Recursively add models from a folder.\n"
        "  --folder-tree PATH         Recursively add models and retain folder structure.\n"
        "  --plugin PATH              Load an importer library; may be repeated.\n"
        "  --plugin-folder PATH       Scan a plugin folder non-recursively; may be repeated.\n"
        "  --log-level off|trace|debug|info|warn|error|critical  Default: off.\n"
        "  --log-file PATH            Required with a non-off log level; otherwise invalid.\n"
        "  --log-performance          Enable frame logs; requires trace, debug, or info.\n"
        "  --log-frame-interval N     Positive frame count; default: 120. Requires --log-performance.\n"
        "  --log-slow-frame-ms N      Positive threshold; requires --log-performance.\n"
        "  --version                  Print the application version.\n"
        "  --help | -h                Print this help.\n"
        "Folder inputs may be repeated and combined with --file and --scene.\n"
        "\nDiscovery, lifecycle, capture, and recovery:\n"
        "  woby ctl instances [--json]\n"
        "  woby ctl --instance ID screenshot PATH [--timeout SECONDS] [--json]\n"
        "  woby ctl --instance ID scene save [--json]\n"
        "  woby ctl --instance ID scene save-as PATH [--overwrite] [--json]\n"
        "  woby ctl --instance ID scene open PATH [--on-dirty error|save|discard] [--save-path PATH] [--overwrite]\n"
        "  woby ctl --instance ID scene new [--on-dirty error|save|discard] [--save-path PATH] [--overwrite]\n"
        "  woby ctl --instance ID quit [--on-dirty error|save|discard] [--save-path PATH] [--overwrite]\n"
        "  woby ctl --instance ID objects [--timeout SECONDS] [--json]\n"
        "  woby ctl --instance ID object OBJECT_ID [--timeout SECONDS] [--json]\n"
        "  woby ctl --instance ID command COMMAND_ID [--json]\n"
        "  woby ctl --instance ID command --request-key KEY [--json]\n"
        "\nScene controls (prefix each command with woby ctl --instance ID):\n");
    for (const auto& method : controlMethods()) {
        std::printf("  %s\n", controlMethodUsage(method).c_str());
    }
    std::printf(
        "\nCommon control options:\n"
        "  --json             Emit one JSON value; exit code 0 on success, 1 on failure.\n"
        "  --timeout SECONDS  Integer from 1 to 3600; default: 60.\n"
        "  --wait             Accepted for compatibility; commands always wait.\n"
        "  --request-key KEY  Safe retries in this viewer launch; 1-128 ASCII letters,\n"
        "                     digits, '-', '_', '.', or ':'. Reuse only for the same request.\n"
        "These options apply to scene controls, lifecycle, capture, and object queries.\n"
        "Exceptions: instances only accepts --json; command lookup accepts --json and\n"
        "either COMMAND_ID or --request-key KEY, with no --timeout or --wait.\n"
        "Duplicates in progress return RPC code -32008; inspect them with ctl command.\n"
        "\nTargets and values:\n"
        "Every viewer starts a local HTTP API and displays its instance ID in the title.\n"
        "ctl connects to a running viewer; it never starts one. Discover it with ctl instances.\n"
        "Instance IDs: 1-64 lowercase letters, digits, '-' or '_'; start with a letter or digit.\n"
        "OBJECT_ID, FILE_ID, GROUP_ID, and COMPARISON_ID come from objects, not names or paths.\n"
        "Object IDs expire on removal, scene replacement, or viewer restart.\n"
        "TARGET is scene or a supported object ID. Use capabilities for target scopes.\n"
        "Booleans require true|false. --tree, --remember, and --overwrite are flags.\n"
        "Omitted setter values are preserved; setters require at least one value.\n"
        "Vectors contain three finite numbers. Quote names and paths containing spaces.\n"
        "Comparison transforms support display translation only.\n"
        "\nComparisons:\n"
        "create returns target (the new COMPARISON_ID); omitted A/B inputs leave empty sides.\n"
        "--a, --b, and --object accept file, folder, or triangular mesh group IDs.\n"
        "add/remove edit one side's current parts; clear removes all references on that side.\n"
        "enable sets --enabled true|false on one side's existing members; omit --object\n"
        "to set the whole side. object COMPARISON_ID reports each member's enabled state.\n"
        "delete removes only the comparison; source models remain loaded.\n"
        "results waits for a fresh geometry/tolerance snapshot calculation, even when hidden.\n"
        "It returns aToB and bToA: sampled maximum, area-weighted mean/P95, percentage\n"
        "above tolerance, and mesh diagnostics.\n"
        "Incomplete inputs fail. Reusing a results request key returns the original snapshot.\n"
        "\nSaving and capture:\n"
        "scene open/new and quit default to --on-dirty error. --save-path requires\n"
        "--on-dirty save; --overwrite requires an explicit save destination.\n"
        "Screenshot waits for visible comparisons and PNG writing; incomplete/failed\n"
        "comparisons fail capture. Existing PNGs are overwritten. CLI paths may be relative.\n"
        "\nSee README.md, doc/ctl-commands.md, and doc/automation.md for examples and details.\n");
}

} // namespace woby
