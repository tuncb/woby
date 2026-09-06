#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "hash_utils.h"

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
    if (result.indices.size() + group.indexCount > comparisonTriangleLimit * 3)
    {
        throw std::runtime_error("Prototype comparison supports up to 50,000 triangles per comparison group.");
    }
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
void visitParts(const UiState& state, ComparisonSide side, const Visitor& visitor)
{
    std::set<std::pair<size_t, size_t>> visited;
    const auto part = [&](size_t fileIndex, size_t groupIndex, const float* parent) {
        if (fileIndex >= state.files.size()) { return; }
        const auto& file = state.files[fileIndex];
        if (groupIndex >= file.groupSettings.size() || groupIndex >= file.mesh.nodes.size()) { return; }
        if (!comparisonMember(file.groupSettings[groupIndex].comparison, side)
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

Mesh comparisonWorldMesh(const UiState &state, ComparisonSide side)
{
    Mesh result;
    visitParts(state, side, [&](const UiFileState& file, size_t index, const float* parent) {
        appendGroup(result, file, index, parent);
    });
    if (result.indices.empty()) {
        throw std::runtime_error("Each comparison group needs at least one mesh part with triangles.");
    }
    result.bounds = calculateBounds(result.vertices);
    result.nodes.push_back({"Comparison", 0, static_cast<uint32_t>(result.indices.size())});
    return result;
}

uint64_t comparisonGeometrySignature(const UiState &state)
{
    uint64_t seed = 17;
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        hashCombine(seed, static_cast<uint64_t>(side));
        size_t count = 0;
        visitParts(state, side, [&](const UiFileState& file, size_t index, const float* parent) {
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
        if (count == 0) { return 0; }
        hashCombine(seed, count);
    }
    return seed;
}
} // namespace woby
