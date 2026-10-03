#include "workflow.h"
#include "utf8_path.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace mesh_lab {
namespace {
using Json = nlohmann::json;
std::string string(const Json& object, const char* key, size_t limit, bool required = true)
{
    if (!required && !object.contains(key)) { return {}; }
    const auto& value = object.at(key);
    if (!value.is_string()) { throw std::runtime_error(std::string(key) + " must be a string."); }
    auto result = value.get<std::string>();
    if ((required && result.find_first_not_of(" \t\r\n") == std::string::npos)
        || result.size() > limit || result.find('\0') != std::string::npos) {
        throw std::runtime_error(std::string(key) + " is empty, too long, or contains a null character.");
    }
    return result;
}
void keys(const Json& object, std::initializer_list<std::string_view> allowed)
{
    if (!object.is_object()) { throw std::runtime_error("Expected a JSON object."); }
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error("Unknown workflow field: " + key);
        }
    }
}
size_t nodeIndex(const std::map<std::string, size_t>& ids, const std::string& id)
{
    const auto found = ids.find(id);
    if (found == ids.end()) { throw std::runtime_error("Unknown node ID: " + id); }
    return found->second;
}
std::string readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw std::runtime_error("Cannot open workflow file."); }
    std::string text(maxWorkflowBytes + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) { throw std::runtime_error("Cannot read workflow file."); }
    text.resize(static_cast<size_t>(file.gcount()));
    return text;
}
} // namespace

Workflow parseWorkflow(std::string_view text)
{
    if (text.size() > maxWorkflowBytes) { throw std::runtime_error("Workflow exceeds the 2 MiB limit."); }
    const auto json = Json::parse(text);
    keys(json, {"format", "version", "title", "description", "source", "initial_node", "nodes", "edges"});
    if (string(json, "format", 64) != "mesh-memory-lab.workflow"
        || !json.at("version").is_number_integer() || json.at("version") != 1) {
        throw std::runtime_error("Unsupported workflow format or version; expected mesh-memory-lab.workflow version 1.");
    }
    Workflow result;
    result.title = string(json, "title", 120);
    result.description = string(json, "description", 8000, false);
    if (json.contains("source")) {
        keys(json["source"], {"format", "text"});
        if (string(json["source"], "format", 16) != "obj") { throw std::runtime_error("Only OBJ live sources are supported."); }
        result.objSource = string(json["source"], "text", maxSourceBytes);
    }
    const auto& nodes = json.at("nodes");
    const auto& edges = json.at("edges");
    if (!nodes.is_array() || nodes.empty() || nodes.size() > 64 || !edges.is_array() || edges.size() > 128) {
        throw std::runtime_error("Expected 1..64 nodes and 0..128 edges.");
    }
    std::map<std::string, size_t> ids;
    for (const auto& value : nodes) {
        keys(value, {"id", "title", "kind", "summary", "description", "position", "inspector"});
        WorkflowNode node;
        node.id = string(value, "id", 64);
        if (node.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos
            || !ids.emplace(node.id, result.nodes.size()).second) { throw std::runtime_error("Invalid or duplicate node ID: " + node.id); }
        node.title = string(value, "title", 120);
        node.summary = string(value, "summary", 240, false);
        node.description = string(value, "description", 16000, false);
        const auto kind = string(value, "kind", 16);
        if (kind != "data" && kind != "transform") { throw std::runtime_error("Node kind must be data or transform."); }
        node.transformer = kind == "transform";
        const auto& position = value.at("position");
        if (!position.is_array() || position.size() != 2) { throw std::runtime_error("Node position must be [x, y]."); }
        for (size_t axis = 0; axis < 2; ++axis) {
            if (!position[axis].is_number()) { throw std::runtime_error("Node coordinates must be numbers."); }
            const auto coordinate = position[axis].get<double>();
            if (!std::isfinite(coordinate) || coordinate < 0 || coordinate > 20000) { throw std::runtime_error("Node coordinates must be finite and within 0..20000."); }
            node.position[axis] = static_cast<float>(coordinate);
            result.extent[axis] = std::max(result.extent[axis], node.position[axis] + (axis == 0 ? workflowCardWidth : workflowCardHeight));
        }
        if (value.contains("inspector")) {
            const auto binding = string(value, "inspector", 32);
            const auto found = std::find_if(pipeline.begin(), pipeline.end(), [&](const auto& p) { return binding == p.key; });
            if (found == pipeline.end() || result.objSource.empty()) { throw std::runtime_error("Inspector requires a known pipeline binding and an embedded OBJ source."); }
            if (found->transformer != node.transformer) { throw std::runtime_error("Inspector binding does not match the node kind."); }
            node.inspector = found->id;
        }
        result.nodes.push_back(std::move(node));
    }
    std::set<std::pair<size_t, size_t>> connections;
    for (const auto& value : edges) {
        keys(value, {"from", "to", "label"});
        WorkflowEdge edge;
        edge.from = nodeIndex(ids, string(value, "from", 64));
        edge.to = nodeIndex(ids, string(value, "to", 64));
        edge.label = string(value, "label", 120, false);
        if (edge.from == edge.to || !connections.emplace(edge.from, edge.to).second) { throw std::runtime_error("Duplicate or self-referencing edge."); }
        result.edges.push_back(std::move(edge));
    }
    if (json.contains("initial_node")) { result.initialNode = nodeIndex(ids, string(json, "initial_node", 64)); }
    return result;
}

std::string serializeWorkflow(const Workflow& workflow)
{
    Json json{{"format", "mesh-memory-lab.workflow"}, {"version", 1}, {"title", workflow.title},
        {"description", workflow.description}, {"initial_node", workflow.nodes.at(workflow.initialNode).id},
        {"nodes", Json::array()}, {"edges", Json::array()}};
    if (!workflow.objSource.empty()) { json["source"] = {{"format", "obj"}, {"text", workflow.objSource}}; }
    for (const auto& node : workflow.nodes) {
        Json value{{"id", node.id}, {"title", node.title}, {"kind", node.transformer ? "transform" : "data"},
            {"summary", node.summary}, {"description", node.description}, {"position", node.position}};
        if (node.inspector) { value["inspector"] = pipeline.at(static_cast<size_t>(*node.inspector)).key; }
        json["nodes"].push_back(std::move(value));
    }
    for (const auto& edge : workflow.edges) {
        json["edges"].push_back({{"from", workflow.nodes.at(edge.from).id}, {"to", workflow.nodes.at(edge.to).id}, {"label", edge.label}});
    }
    auto text = json.dump(2) + "\n";
    (void)parseWorkflow(text); // Validate public structs at the save boundary as well.
    return text;
}
Workflow loadWorkflow(const std::filesystem::path& path) { return parseWorkflow(readFile(path)); }

void saveWorkflow(const std::filesystem::path& path, const Workflow& workflow)
{
    const auto text = serializeWorkflow(workflow);
    const auto absolute = std::filesystem::absolute(path);
    const auto prefix = ".meshflow-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::path temporary;
    for (size_t i = 0;; ++i) {
        temporary = absolute.parent_path() / (prefix + "-" + std::to_string(i));
        if (std::filesystem::create_directory(temporary)) { break; }
    }
    try {
        const auto payload = temporary / "workflow.meshflow";
        std::ofstream file(payload, std::ios::binary);
        file.write(text.data(), static_cast<std::streamsize>(text.size())); file.close();
        if (!file) { throw std::runtime_error("Cannot write workflow file."); }
        std::filesystem::copy_file(payload, absolute); // Default options refuse an existing file.
    } catch (...) {
        std::error_code error; std::filesystem::remove_all(temporary, error); throw;
    }
    std::error_code error; std::filesystem::remove_all(temporary, error);
}

WorkflowLibrary loadWorkflowLibrary(const std::filesystem::path& directory)
{
    WorkflowLibrary result;
    result.directory = std::filesystem::absolute(directory).lexically_normal();
    std::vector<std::filesystem::path> paths;
    // Directory symlinks are deliberately not followed, so snapshots cannot
    // cause recursive cycles or scan unrelated directory trees.
    for (const auto& file : std::filesystem::recursive_directory_iterator(result.directory)) {
        auto extension = woby::pathToUtf8(file.path().extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (file.is_regular_file() && extension == ".meshflow") {
            paths.push_back(file.path());
            if (paths.size() > 256) { throw std::runtime_error("A workflow library may contain at most 256 .meshflow files."); }
        }
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        WorkflowEntry entry; entry.path = path; entry.relativePath = path.lexically_relative(result.directory);
        const auto folder = entry.relativePath.has_parent_path() ? *entry.relativePath.begin() : std::filesystem::path{};
        auto group = std::find_if(result.groups.begin(), result.groups.end(), [&](const auto& g) { return g.folder == folder; });
        if (group == result.groups.end()) { result.groups.push_back({folder,{}}); group = std::prev(result.groups.end()); }
        group->entries.push_back(result.entries.size());
        try {
            auto document = loadWorkflow(path);
            if (!document.objSource.empty()) { entry.trace = std::make_shared<const Trace>(traceObjSource(document.objSource, document.title)); }
            entry.document = std::move(document);
        } catch (const std::exception& error) { entry.error = error.what(); }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

std::filesystem::path saveWorkflowCopy(const WorkflowEntry& entry)
{
    if (!entry.document) { throw std::runtime_error("Cannot copy an invalid workflow."); }
    const auto stem = woby::pathToUtf8(entry.path.stem());
    for (size_t i = 1;; ++i) {
        const auto destination = entry.path.parent_path() / woby::pathFromUtf8(stem + "-copy-" + std::to_string(i) + ".meshflow");
        if (!std::filesystem::exists(destination)) {
            auto document = *entry.document;
            if (document.title.size() <= 113) { document.title += " (copy)"; }
            saveWorkflow(destination, document);
            return destination;
        }
    }
}

size_t findWorkflow(const WorkflowLibrary& library, std::string_view name)
{
    const auto requested = woby::pathFromUtf8(name).lexically_normal();
    size_t found = noWorkflow;
    for (size_t i = 0; i < library.entries.size(); ++i) {
        const auto& entry = library.entries[i];
        if (name.empty()) { if (entry.document) { return i; } continue; }
        if (entry.relativePath == requested) {
            if (!entry.document) { throw std::runtime_error(entry.error); }
            return i;
        }
    }
    if (name.empty()) { return noWorkflow; }
    if (!requested.has_parent_path()) {
        for (size_t i = 0; i < library.entries.size(); ++i) {
            if (library.entries[i].path.filename() != requested) { continue; }
            if (found != noWorkflow) { throw std::runtime_error("Ambiguous workflow name; use commit-folder/file.meshflow: " + std::string(name)); }
            found = i;
        }
    }
    if (found == noWorkflow) { throw std::runtime_error("Workflow not found: " + std::string(name)); }
    if (!library.entries[found].document) { throw std::runtime_error(library.entries[found].error); }
    return found;
}
} // namespace mesh_lab
