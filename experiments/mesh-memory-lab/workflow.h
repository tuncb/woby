#pragma once
#include "pipeline.h"
#include <memory>
#include <optional>

namespace mesh_lab {
inline constexpr size_t maxWorkflowBytes = 2 * 1024 * 1024;
inline constexpr float workflowCardWidth = 196, workflowCardHeight = 108;
inline constexpr size_t noWorkflow = static_cast<size_t>(-1);
struct WorkflowNode {
    std::string id, title, summary, description;
    bool transformer = false;
    std::array<float, 2> position{};
    std::optional<Node> inspector;
};
struct WorkflowEdge {
    size_t from = 0, to = 0;
    std::string label;
};
struct Workflow {
    std::string title, description, objSource;
    std::vector<WorkflowNode> nodes;
    std::vector<WorkflowEdge> edges;
    size_t initialNode = 0;
    std::array<float, 2> extent{}; // Cached at the validation boundary.
};
struct WorkflowEntry {
    std::filesystem::path path, relativePath;
    std::optional<Workflow> document;
    std::shared_ptr<const Trace> trace;
    std::string error;
};
struct WorkflowGroup {
    std::filesystem::path folder; // Empty for files directly in the library root.
    std::vector<size_t> entries;
};
struct WorkflowLibrary {
    std::filesystem::path directory;
    std::vector<WorkflowEntry> entries;
    std::vector<WorkflowGroup> groups;
};
[[nodiscard]] Workflow parseWorkflow(std::string_view text);
[[nodiscard]] std::string serializeWorkflow(const Workflow& workflow);
[[nodiscard]] Workflow loadWorkflow(const std::filesystem::path& path);
// New files only: an existing destination is never overwritten.
void saveWorkflow(const std::filesystem::path& path, const Workflow& workflow);
[[nodiscard]] std::filesystem::path saveWorkflowCopy(const WorkflowEntry& entry);
[[nodiscard]] WorkflowLibrary loadWorkflowLibrary(const std::filesystem::path& directory);
// Relative paths distinguish snapshots with identical filenames. Bare names
// are accepted only for a root file or an unambiguous match.
[[nodiscard]] size_t findWorkflow(const WorkflowLibrary& library, std::string_view name);
} // namespace mesh_lab
