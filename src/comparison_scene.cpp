#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "hash_utils.h"

#include <bx/math.h>

#include <stdexcept>

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
        throw std::runtime_error("Prototype comparison supports up to 50,000 triangles per file.");
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
void appendNode(Mesh &result, const UiState &state, const UiSceneNode &node, size_t fileIndex, const float *parent)
{
    float model[16], local[16];
    if (node.kind == UiSceneNodeKind::folder)
    {
        sceneNodeTransformMatrix(node.settings, local);
        bx::mtxMul(model, parent, local);
    }
    else if (node.kind == UiSceneNodeKind::file)
    {
        if (node.fileIndex != fileIndex)
        {
            return;
        }
        const auto &file = state.files[fileIndex];
        fileTransformMatrix(file.fileSettings, local);
        bx::mtxMul(model, parent, local);
        if (node.children.empty())
        {
            for (size_t i = 0; i < file.mesh.nodes.size(); ++i)
            {
                appendGroup(result, file, i, model);
            }
        }
    }
    else
    {
        if (node.fileIndex == fileIndex)
        {
            appendGroup(result, state.files[fileIndex], node.groupIndex, parent);
        }
        return;
    }
    for (const auto &child : node.children)
    {
        appendNode(result, state, child, fileIndex, model);
    }
}
void hashTransform(uint64_t &seed, const auto &settings)
{
    hashFloat(seed, settings.scale);
    hashArray3(seed, settings.translation);
    hashArray3(seed, settings.rotationDegrees);
    hashArray3(seed, settings.center);
}
void hashNode(uint64_t &seed, const UiSceneNode &node)
{
    hashCombine(seed, static_cast<uint64_t>(node.kind));
    hashCombine(seed, node.fileIndex);
    hashCombine(seed, node.groupIndex);
    hashTransform(seed, node.settings);
    hashCombine(seed, node.children.size());
    for (const auto &child : node.children)
    {
        hashNode(seed, child);
    }
}
} // namespace

Mesh comparisonWorldMesh(const UiState &state, size_t fileIndex)
{
    const auto &file = state.files.at(fileIndex);
    if (file.mesh.indices.size() / 3 > comparisonTriangleLimit)
    {
        throw std::runtime_error("Prototype comparison supports up to 50,000 triangles per file.");
    }
    Mesh result;
    float identity[16];
    bx::mtxIdentity(identity);
    if (state.sceneNodes.empty())
    {
        float model[16];
        fileTransformMatrix(file.fileSettings, model);
        for (size_t i = 0; i < file.mesh.nodes.size(); ++i)
        {
            appendGroup(result, file, i, model);
        }
    }
    else
    {
        for (const auto &node : state.sceneNodes)
        {
            appendNode(result, state, node, fileIndex, identity);
        }
    }
    if (result.indices.empty())
    {
        throw std::runtime_error("The selected file has no triangles in the scene.");
    }
    result.bounds = calculateBounds(result.vertices);
    result.nodes.push_back({"Comparison", 0, static_cast<uint32_t>(result.indices.size())});
    return result;
}

uint64_t comparisonGeometrySignature(const UiState &state)
{
    uint64_t seed = 17;
    for (int index : {state.comparison.originalFile, state.comparison.repairedFile})
    {
        if (index < 0 || static_cast<size_t>(index) >= state.files.size())
        {
            return 0;
        }
        const auto &file = state.files[static_cast<size_t>(index)];
        hashCombine(seed, file.objectId);
        hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.vertices.data()));
        hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.indices.data()));
        hashCombine(seed, file.mesh.vertices.size());
        hashCombine(seed, file.mesh.indices.size());
        hashTransform(seed, file.fileSettings);
        for (const auto &group : file.groupSettings)
        {
            hashTransform(seed, group);
        }
    }
    for (const auto &node : state.sceneNodes)
    {
        hashNode(seed, node);
    }
    return seed;
}
} // namespace woby
