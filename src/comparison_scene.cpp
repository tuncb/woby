#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "hash_utils.h"
#include "ui_operations.h"

#include <bx/math.h>

#include <stdexcept>
#include <set>
#include <utility>

namespace woby
{
namespace
{
void appendGroup(Mesh &result, const UiFileState &file, size_t groupIndex, const float *parent)
{
    if (groupIndex >= file.mesh.nodes.size() || groupIndex >= file.groupSettings.size())
    {
        throw std::runtime_error("Comparison encountered an invalid mesh group.");
    }
    float local[16], model[16];
    groupTransformMatrix(file.groupSettings[groupIndex], local);
    bx::mtxMul(model, parent, local);
    const auto &group = file.mesh.nodes[groupIndex];
    const size_t end = static_cast<size_t>(group.indexOffset) + group.indexCount;
    if (end > file.mesh.indices.size() || group.indexCount % 3 != 0)
    {
        throw std::runtime_error("Comparison encountered an invalid triangle range.");
    }
    validateComparisonMeshSize(result.vertices.size() + group.indexCount,
        (result.indices.size() + group.indexCount) / 3);
    for (size_t i = group.indexOffset; i < end; ++i)
    {
        const auto &p = file.mesh.vertices.at(file.mesh.indices[i]).position;
        Vertex vertex;
        for (size_t k = 0; k < 3; ++k)
        {
            vertex.position[k] = model[k] * p[0] + model[k + 4] * p[1] + model[k + 8] * p[2] + model[k + 12];
        }
        if (!finitePosition(vertex.position))
        {
            throw std::runtime_error("Comparison requires finite transformed coordinates.");
        }
        result.indices.push_back(static_cast<uint32_t>(result.vertices.size()));
        result.vertices.push_back(vertex);
    }
}
// One traversal supplies both geometry snapshots and their cache signatures.
// Each canonical mesh part is visited once, irrespective of overlapping selections.
template <typename Visitor>
void visitParts(const UiState& state, ComparisonSide side, SceneObjectId id, const Visitor& visitor)
{
    std::set<std::pair<size_t, size_t>> visited;
    const auto part = [&](size_t fileIndex, size_t groupIndex, const float* parent) {
        if (fileIndex >= state.files.size()) { return; }
        const auto& file = state.files[fileIndex];
        if (groupIndex >= file.groupSettings.size() || groupIndex >= file.mesh.nodes.size()) { return; }
        if (!comparisonContains(state, file.groupSettings[groupIndex].objectId, side, id)
            || file.mesh.nodes[groupIndex].indexCount == 0
            || !visited.emplace(fileIndex, groupIndex).second) { return; }
        visitor(file, groupIndex, parent);
    };
    const auto nodeVisitor = [&](auto&& self, const UiSceneNode& node, const float* parent) -> void {
        float local[16], model[16];
        if (node.kind == UiSceneNodeKind::group) {
            part(node.fileIndex, node.groupIndex, parent);
            return;
        }
        if (node.kind == UiSceneNodeKind::folder) {
            sceneNodeTransformMatrix(node.settings, local);
        } else {
            if (node.fileIndex >= state.files.size()) { return; }
            fileTransformMatrix(state.files[node.fileIndex].fileSettings, local);
        }
        bx::mtxMul(model, parent, local);
        if (node.kind == UiSceneNodeKind::file && node.children.empty()) {
            for (size_t i = 0; i < state.files[node.fileIndex].groupSettings.size(); ++i) {
                part(node.fileIndex, i, model);
            }
        }
        for (const auto& child : node.children) { self(self, child, model); }
    };
    float identity[16];
    bx::mtxIdentity(identity);
    if (state.sceneNodes.empty()) {
        for (size_t i = 0; i < state.files.size(); ++i) {
            float model[16];
            fileTransformMatrix(state.files[i].fileSettings, model);
            for (size_t j = 0; j < state.files[i].groupSettings.size(); ++j) { part(i, j, model); }
        }
    } else {
        for (const auto& node : state.sceneNodes) { nodeVisitor(nodeVisitor, node, identity); }
    }
}
} // namespace

std::vector<ComparisonTreeNode> comparisonTree(const UiState& state, ComparisonSide side, SceneObjectId id)
{
    std::set<std::pair<size_t, size_t>> visited;
    const auto build = [&](auto&& self, const UiSceneNode& source) -> ComparisonTreeNode {
        ComparisonTreeNode result;
        result.objectId = source.objectId;
        result.kind = source.kind;
        result.name = source.name;
        if (source.kind == UiSceneNodeKind::group) {
            if (source.fileIndex >= state.files.size()) { return result; }
            const auto& file = state.files[source.fileIndex];
            if (source.groupIndex >= file.groupSettings.size() || source.groupIndex >= file.mesh.nodes.size()) {
                return result;
            }
            const auto& part = file.groupSettings[source.groupIndex];
            const auto& meshNode = file.mesh.nodes[source.groupIndex];
            if (!comparisonContains(state, part.objectId, side, id) || meshNode.indexCount < 3 || meshNode.indexCount % 3 != 0
                || static_cast<size_t>(meshNode.indexOffset) + meshNode.indexCount > file.mesh.indices.size()
                || !visited.emplace(source.fileIndex, source.groupIndex).second) { return result; }
            result.objectId = part.objectId;
            result.partCount = 1;
            result.triangleCount = meshNode.indexCount / 3;
            return result;
        }
        const auto append = [&](const UiSceneNode& child) {
            auto branch = self(self, child);
            if (branch.partCount == 0) { return; }
            result.partCount += branch.partCount;
            result.triangleCount += branch.triangleCount;
            result.children.push_back(std::move(branch));
        };
        if (source.kind == UiSceneNodeKind::file && source.children.empty() && source.fileIndex < state.files.size()) {
            const auto fileNode = createFileSceneNode(state.files[source.fileIndex], source.fileIndex);
            for (const auto& child : fileNode.children) { append(child); }
        } else {
            for (const auto& child : source.children) { append(child); }
        }
        return result;
    };
    std::vector<ComparisonTreeNode> result;
    const auto appendRoot = [&](const UiSceneNode& source) {
        auto root = build(build, source);
        if (root.partCount != 0) { result.push_back(std::move(root)); }
    };
    if (state.sceneNodes.empty()) {
        for (size_t i = 0; i < state.files.size(); ++i) { appendRoot(createFileSceneNode(state.files[i], i)); }
    } else {
        for (const auto& source : state.sceneNodes) { appendRoot(source); }
    }
    return result;
}

ComparisonInputSummary comparisonInputSummary(const UiState& state, ComparisonSide side, SceneObjectId id)
{
    ComparisonInputSummary result;
    const auto appendNames = [&](auto&& self, const ComparisonTreeNode& node) -> void {
        if (node.kind == UiSceneNodeKind::folder) {
            for (const auto& child : node.children) { self(self, child); }
        } else {
            if (!result.sourceNames.empty()) { result.sourceNames += ", "; }
            result.sourceNames += node.name;
        }
    };
    for (const auto& root : comparisonTree(state, side, id)) {
        result.partCount += root.partCount;
        appendNames(appendNames, root);
    }
    const std::string label = side == ComparisonSide::a ? "A" : "B";
    if (const auto* comparison = findComparison(state, id)) {
        const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
        for (const auto& member : members) {
            if (!comparisonObjectParts(state, {member.objectId}).empty()) { continue; }
            if (result.issue.empty()) { result.issue = "Input " + label + " has missing or invalid references: "; }
            else { result.issue += ", "; }
            result.issue += member.name.empty() ? "Unnamed part" : member.name;
        }
    }
    if (!result.issue.empty()) { result.issue += ". Restore the source or remove missing references below."; }
    else if (result.partCount == 0) {
        result.issue = "Input " + label + " is empty. Use Comparison membership in the scene tree context menu.";
    }
    if (result.sourceNames.empty()) { result.sourceNames = "No available sources"; }
    return result;
}

Mesh comparisonWorldMesh(const UiState &state, ComparisonSide side, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { throw std::runtime_error("Comparison no longer exists."); }
    const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    if (members.size() != comparisonPartCount(state, side, id)) {
        throw std::runtime_error("Comparison has missing or invalid source parts.");
    }
    Mesh result;
    // Check the whole side before allocating any expanded world geometry.
    size_t triangleCount = 0;
    visitParts(state, side, id, [&](const UiFileState& file, size_t index, const float*) {
        triangleCount += file.mesh.nodes[index].indexCount / 3;
        validateComparisonMeshSize(triangleCount * 3, triangleCount);
    });
    result.vertices.reserve(triangleCount * 3);
    result.indices.reserve(triangleCount * 3);
    visitParts(state, side, id, [&](const UiFileState& file, size_t index, const float* parent) {
        appendGroup(result, file, index, parent);
    });
    if (result.indices.empty()) {
        throw std::runtime_error("Each comparison group needs at least one mesh part with triangles.");
    }
    result.bounds = calculateBounds(result.vertices);
    result.nodes.push_back({"Comparison", 0, static_cast<uint32_t>(result.indices.size())});
    return result;
}

uint64_t comparisonGeometrySignature(const UiState &state, SceneObjectId id)
{
    if (!canInspectComparison(state, id)) { return 0; }
    uint64_t seed = 17;
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        hashCombine(seed, static_cast<uint64_t>(side));
        size_t count = 0;
        visitParts(state, side, id, [&](const UiFileState& file, size_t index, const float* parent) {
            ++count;
            hashCombine(seed, file.objectId);
            hashCombine(seed, file.groupSettings[index].objectId);
            hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.vertices.data()));
            hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.indices.data()));
            hashCombine(seed, file.mesh.vertices.size());
            hashCombine(seed, file.mesh.indices.size());
            hashCombine(seed, file.mesh.nodes[index].indexOffset);
            hashCombine(seed, file.mesh.nodes[index].indexCount);
            float local[16], model[16];
            groupTransformMatrix(file.groupSettings[index], local);
            bx::mtxMul(model, parent, local);
            for (const auto value : model) { hashFloat(seed, value); }
        });
        hashCombine(seed, count);
    }
    return seed;
}
std::optional<Bounds> comparisonDisplayBounds(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison || !canInspectComparison(state, id)) { return std::nullopt; }
    std::vector<Vertex> corners;
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        visitParts(state, side, id, [&](const UiFileState& file, size_t index, const float* parent) {
            const auto& group = file.groupSettings[index];
            float local[16], model[16];
            groupTransformMatrix(group, local);
            bx::mtxMul(model, parent, local);
            const auto& bounds = group.localBoundsValid ? group.localBounds : file.mesh.bounds;
            for (unsigned mask = 0; mask < 8; ++mask) {
                Vertex corner;
                std::array<float, 3> p{};
                for (size_t k = 0; k < 3; ++k) { p[k] = (mask & (1u << k)) ? bounds.max[k] : bounds.min[k]; }
                for (size_t k = 0; k < 3; ++k) {
                    corner.position[k] = model[k] * p[0] + model[k + 4] * p[1] + model[k + 8] * p[2]
                        + model[k + 12] + comparison->translation[k];
                }
                if (finitePosition(corner.position)) { corners.push_back(corner); }
            }
        });
    }
    if (corners.empty()) { return std::nullopt; }
    return calculateBounds(corners);
}
} // namespace woby
