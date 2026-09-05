#include "control_protocol.h"
#include "command_line.h"
#include "utf8_path.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace woby {
using Json = nlohmann::json;

const std::vector<ControlMethod>& controlMethods()
{
    static const std::vector<ControlMethod> methods = {
        {ControlAction::status, "status", "status", {}, {}, {}},
        {ControlAction::capabilities, "capabilities", "capabilities", {}, {}, {}},
        {ControlAction::sceneInfo, "scene.info", "scene info", {}, {}, {}},
        {ControlAction::sceneTree, "scene.tree", "scene tree", {}, {}, {}},
        {ControlAction::sceneBounds, "scene.bounds", "scene bounds", {}, {}, {}},
        {ControlAction::visibility, "visibility.set", "visibility set", "target", {"visible"}, {"visible"}, false, true},
        {ControlAction::render, "render.set", "render set", "target", {"solid", "triangles", "vertices"}, {}, true, true},
        {ControlAction::transformGet, "transform.get", "transform get", "target", {}, {}},
        {ControlAction::transformSet, "transform.set", "transform set", "target", {"translation", "rotationDegrees", "scale"}, {}, true, true},
        {ControlAction::transformReset, "transform.reset", "transform reset", "target", {}, {}, false, true},
        {ControlAction::opacity, "opacity.set", "opacity set", "target", {"value"}, {"value"}, false, true},
        {ControlAction::colorSet, "color.set", "color set", "target", {"rgb"}, {"rgb"}, false, true},
        {ControlAction::colorReset, "color.reset", "color reset", "target", {}, {}, false, true},
        {ControlAction::vertexSize, "vertex-size.set", "vertex-size set", "target", {"pixels", "scale"}, {}, true, true},
        {ControlAction::grid, "grid.set", "grid set", {}, {"visible"}, {"visible"}, false, true},
        {ControlAction::origin, "origin.set", "origin set", {}, {"visible"}, {"visible"}, false, true},
        {ControlAction::upAxis, "up-axis.set", "up-axis set", "axis", {}, {}, false, true},
        {ControlAction::cameraGet, "camera.get", "camera get", {}, {}, {}},
        {ControlAction::cameraFrame, "camera.frame", "camera frame", {}, {}, {}, false, true},
        {ControlAction::cameraOrbit, "camera.orbit", "camera orbit", {}, {"yawDegrees", "pitchDegrees"}, {}, true, true},
        {ControlAction::cameraPan, "camera.pan", "camera pan", {}, {"right", "up"}, {}, true, true},
        {ControlAction::cameraRoll, "camera.roll", "camera roll", {}, {"rollDegrees"}, {"rollDegrees"}, false, true},
        {ControlAction::cameraDolly, "camera.dolly", "camera dolly", {}, {"factor"}, {"factor"}, false, true},
        {ControlAction::cameraMove, "camera.move", "camera move", {}, {"right", "up", "forward"}, {}, true, true},
        {ControlAction::modelAdd, "model.add", "model add", "path", {}, {}, false, true},
        {ControlAction::modelRemove, "model.remove", "model remove", "target", {}, {}, false, true},
        {ControlAction::folderAdd, "folder.add", "folder add", "path", {"tree"}, {}, false, true},
        {ControlAction::importersList, "importers.list", "importers list", {}, {}, {}},
        {ControlAction::importersAdd, "importers.add", "importers add", "path", {"remember"}, {}, false, true},
        {ControlAction::importersScan, "importers.scan", "importers scan", "path", {"remember"}, {}, false, true},
        {ControlAction::importersForget, "importers.forget", "importers forget", "path", {}, {}, false, true},
        {ControlAction::stats, "stats", "stats", {}, {}, {}},
        {ControlAction::performance, "performance.get", "performance get", {}, {}, {}},
        {ControlAction::pane, "pane.set", "pane set", {}, {"visible", "width"}, {}, true, true},
    };
    return methods;
}

const ControlMethod* findControlMethod(const std::string& method)
{
    for (const auto& entry : controlMethods()) { if (entry.method == method) { return &entry; } }
    return nullptr;
}

const ControlMethod& controlMethod(ControlAction action)
{
    for (const auto& entry : controlMethods()) { if (entry.action == action) { return entry; } }
    throw std::invalid_argument("Unknown control action.");
}

namespace {
bool booleanOption(const std::string& name)
{
    return name == "visible" || name == "solid" || name == "triangles" || name == "vertices"
        || name == "tree" || name == "remember";
}
bool vectorOption(const std::string& name)
{
    return name == "translation" || name == "rotationDegrees" || name == "rgb";
}
std::string cliOption(const std::string& name)
{
    if (name == "rotationDegrees") { return "--rotation-degrees"; }
    if (name == "yawDegrees") { return "--yaw-degrees"; }
    if (name == "pitchDegrees") { return "--pitch-degrees"; }
    if (name == "rollDegrees") { return "--roll-degrees"; }
    return "--" + name;
}
float number(const Json& value)
{
    if (!value.is_number()) { throw std::invalid_argument("Expected a finite number."); }
    const double result = value.get<double>();
    if (!std::isfinite(result) || std::abs(result) > static_cast<double>(std::numeric_limits<float>::max())) {
        throw std::invalid_argument("Expected a finite float-range number.");
    }
    return static_cast<float>(result);
}
double cliNumber(const std::string& value)
{
    size_t used = 0;
    const double result = std::stod(value, &used);
    if (used != value.size() || !std::isfinite(result)) { throw std::invalid_argument("Expected a finite number."); }
    return result;
}
}

ControlOperation parseControlOperation(const ControlMethod& method, const Json& params)
{
    if (!params.is_object()) { throw std::invalid_argument("params must be an object."); }
    for (const auto& item : params.items()) {
        if (item.key() != method.positional
            && std::find(method.options.begin(), method.options.end(), item.key()) == method.options.end()) {
            throw std::invalid_argument("Unknown " + method.method + " parameter: " + item.key());
        }
    }
    auto required = method.required;
    if (!method.positional.empty()) { required.push_back(method.positional); }
    for (const auto& name : required) {
        if (!params.contains(name)) { throw std::invalid_argument("Missing parameter: " + name); }
    }
    if (method.requiresValues && std::none_of(method.options.begin(), method.options.end(),
        [&](const auto& name) { return params.contains(name); })) {
        throw std::invalid_argument("Supply at least one value to change.");
    }
    ControlOperation command;
    command.action = method.action;
    if (params.contains("target")) {
        if (!params["target"].is_string() || params["target"].get<std::string>().empty()) {
            throw std::invalid_argument("target must be scene or an object ID.");
        }
        command.target = params["target"].get<std::string>();
        const bool sceneAllowed = method.action == ControlAction::visibility || method.action == ControlAction::render
            || method.action == ControlAction::vertexSize;
        if (command.target == "scene" && !sceneAllowed) { throw std::invalid_argument("This command requires an object ID."); }
    }
    if (params.contains("path")) {
        if (!params["path"].is_string()) { throw std::invalid_argument("path must be an absolute path."); }
        const auto value = params["path"].get<std::string>();
        if (value.empty() || value.size() > 8192 || value.find('\0') != std::string::npos) {
            throw std::invalid_argument("Invalid path.");
        }
        command.path = pathFromUtf8(value).lexically_normal();
        if (!command.path.is_absolute()) { throw std::invalid_argument("path must be absolute."); }
    }
    if (params.contains("axis")) {
        if (params["axis"] != "y" && params["axis"] != "z") { throw std::invalid_argument("axis must be y or z."); }
        command.axis = params["axis"].get<std::string>();
    }
    for (const auto& name : method.options) {
        if (!params.contains(name)) { continue; }
        const auto& value = params[name];
        if (booleanOption(name) && !value.is_boolean()) { throw std::invalid_argument(name + " must be a boolean."); }
        if (vectorOption(name)) {
            if (!value.is_array() || value.size() != 3) { throw std::invalid_argument(name + " must contain three numbers."); }
            for (const auto& component : value) { (void)number(component); }
        }
    }
#define BOOL_FIELD(field) if (params.contains(#field)) { command.field = params[#field].get<bool>(); }
    BOOL_FIELD(visible) BOOL_FIELD(solid) BOOL_FIELD(triangles) BOOL_FIELD(vertices) BOOL_FIELD(tree) BOOL_FIELD(remember)
#undef BOOL_FIELD
#define NUMBER_FIELD(field) if (params.contains(#field)) { command.field = number(params[#field]); }
    NUMBER_FIELD(scale) NUMBER_FIELD(value) NUMBER_FIELD(pixels) NUMBER_FIELD(width)
    NUMBER_FIELD(yawDegrees) NUMBER_FIELD(pitchDegrees) NUMBER_FIELD(rollDegrees)
    NUMBER_FIELD(right) NUMBER_FIELD(up) NUMBER_FIELD(forward) NUMBER_FIELD(factor)
#undef NUMBER_FIELD
#define VECTOR_FIELD(field) if (params.contains(#field)) { command.field = params[#field].get<std::array<float, 3>>(); }
    VECTOR_FIELD(translation) VECTOR_FIELD(rotationDegrees) VECTOR_FIELD(rgb)
#undef VECTOR_FIELD
    if (command.factor && *command.factor <= 0) { throw std::invalid_argument("factor must be positive."); }
    if (command.action == ControlAction::vertexSize
        && (command.target == "scene" ? (!command.pixels || command.scale.has_value()) : (!command.scale || command.pixels.has_value()))) {
        throw std::invalid_argument("Use pixels for scene, or scale for a file/group.");
    }
    return command;
}

std::string controlMethodUsage(const ControlMethod& method)
{
    std::string result = method.cli;
    if (!method.positional.empty()) { result += " " + method.positional; }
    for (const auto& name : method.options) {
        const bool required = std::find(method.required.begin(), method.required.end(), name) != method.required.end();
        result += required ? " " : " [";
        result += cliOption(name);
        if (name != "tree" && name != "remember") {
            result += booleanOption(name) ? " true|false" : vectorOption(name) ? " X Y Z" : " N";
        }
        if (!required) { result += "]"; }
    }
    return result;
}

Json controlOperationParams(const ControlOperation& command)
{
    Json result = Json::object();
    if (!command.target.empty()) { result["target"] = command.target; }
    if (!command.path.empty()) { result["path"] = pathToUtf8(command.path); }
    if (!command.axis.empty()) { result["axis"] = command.axis; }
    if (command.action == ControlAction::folderAdd) { result["tree"] = command.tree; }
    if (command.action == ControlAction::importersAdd || command.action == ControlAction::importersScan) { result["remember"] = command.remember; }
#define FIELD(field) if (command.field) { result[#field] = *command.field; }
    FIELD(visible) FIELD(solid) FIELD(triangles) FIELD(vertices) FIELD(translation) FIELD(rotationDegrees) FIELD(rgb)
    FIELD(scale) FIELD(value) FIELD(pixels) FIELD(width) FIELD(yawDegrees) FIELD(pitchDegrees) FIELD(rollDegrees)
    FIELD(right) FIELD(up) FIELD(forward) FIELD(factor)
#undef FIELD
    return result;
}

Json controlCapabilities()
{
    Json methods = Json::array();
    for (const auto* name : {"instance.info", "objects.list", "object.get", "screenshot.capture", "scene.save",
        "scene.save-as", "scene.open", "scene.new", "quit", "command.get"}) { methods.push_back({{"method", name}}); }
    for (const auto& method : controlMethods()) {
        Json parameters = Json::object();
        if (!method.positional.empty()) { parameters[method.positional] = {{"type", "string"}, {"required", true}}; }
        for (const auto& name : method.options) {
            parameters[name] = {{"type", booleanOption(name) ? "boolean" : vectorOption(name) ? "number[3]" : "number"},
                {"required", std::find(method.required.begin(), method.required.end(), name) != method.required.end()},
                {"cliOption", cliOption(name)}};
        }
        methods.push_back({{"method", method.method}, {"cli", method.cli}, {"usage", controlMethodUsage(method)},
            {"positional", method.positional}, {"parameters", parameters}, {"options", method.options},
            {"requiredOptions", method.required}, {"requiresValues", method.requiresValues}, {"mutating", method.mutating}});
    }
    return {{"apiVersion", 1}, {"methods", methods}, {"objectKinds", {"folder", "file", "group"}},
        {"cameraPersistent", false}, {"screenshot", {{"width", 1920}, {"height", 1800}, {"overwrite", true}}},
        {"scopes", {{"visibility.set", {"scene", "folder", "file", "group"}},
            {"render.set", {"scene", "folder", "file", "group"}}, {"transform", {"folder", "file", "group"}},
            {"opacity.set", {"folder", "file", "group"}}, {"color", {"group"}},
            {"vertex-size.set", {"scene", "file", "group"}}, {"model.remove", {"file"}}}}};
}

bool parseExtendedControlArguments(int argc, char** argv, ControlArguments& arguments)
{
    std::vector<std::string> words;
    Json common = Json::object();
    for (int index = 2; index < argc; ++index) {
        const std::string word = argv[index];
        const bool global = word == "--instance" || word == "--timeout" || word == "--request-key" || word == "--json" || word == "--wait";
        if (!global) { words.push_back(word); continue; }
        if (common.contains(word)) { throw std::runtime_error("Repeated option: " + word); }
        if (word == "--json" || word == "--wait") { common[word] = true; }
        else {
            if (++index >= argc) { throw std::runtime_error("Missing value for " + word); }
            common[word] = argv[index];
        }
    }
    const ControlMethod* selected = nullptr;
    size_t commandWords = 0;
    for (const auto& method : controlMethods()) {
        if (!words.empty() && words[0] == method.cli) { selected = &method; commandWords = 1; break; }
        if (words.size() >= 2 && words[0] + " " + words[1] == method.cli) { selected = &method; commandWords = 2; break; }
    }
    if (!selected) { return false; }
    if (!common.contains("--instance") || !validInstanceId(common["--instance"].get<std::string>())) {
        throw std::runtime_error("Control commands require --instance ID with a valid instance ID.");
    }
    arguments.instanceId = common["--instance"].get<std::string>();
    arguments.json = common.contains("--json");
    if (common.contains("--request-key")) {
        arguments.requestKey = common["--request-key"].get<std::string>();
        if (!validAutomationRequestKey(*arguments.requestKey)) { throw std::runtime_error("Invalid request key."); }
    }
    if (common.contains("--timeout")) {
        const auto text = common["--timeout"].get<std::string>();
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) { throw std::runtime_error("timeout must be an integer from 1 to 3600."); }
        const double timeout = cliNumber(text);
        if (timeout < 1 || timeout > 3600) { throw std::runtime_error("timeout must be from 1 to 3600."); }
        arguments.timeoutSeconds = static_cast<int>(timeout);
    }
    Json params = Json::object();
    for (size_t index = commandWords; index < words.size(); ++index) {
        const auto word = words[index];
        if (word.rfind("--", 0) != 0 && !selected->positional.empty() && !params.contains(selected->positional)) {
            params[selected->positional] = selected->positional == "path"
                ? pathToUtf8(std::filesystem::absolute(pathFromUtf8(word)).lexically_normal()) : word;
            continue;
        }
        auto option = std::find_if(selected->options.begin(), selected->options.end(), [&](const auto& name) { return cliOption(name) == word; });
        if (option == selected->options.end() || params.contains(*option)) { throw std::runtime_error("Unexpected or repeated option: " + word); }
        const auto& name = *option;
        if (name == "tree" || name == "remember") { params[name] = true; continue; }
        const size_t count = vectorOption(name) ? 3u : 1u;
        if (words.size() - index - 1 < count) { throw std::runtime_error("Missing value for " + word); }
        if (booleanOption(name)) {
            const auto value = words[++index];
            if (value != "true" && value != "false") { throw std::runtime_error(word + " expects true or false."); }
            params[name] = value == "true";
        } else if (count == 3) {
            params[name] = Json::array();
            for (size_t component = 0; component < 3; ++component) { params[name].push_back(cliNumber(words[++index])); }
        } else { params[name] = cliNumber(words[++index]); }
    }
    arguments.operation = parseControlOperation(*selected, params);
    arguments.command = ControlCommand::operation;
    return true;
}

} // namespace woby
