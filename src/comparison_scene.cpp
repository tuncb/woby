#include "comparison_scene.h"
#include "mesh_comparison.h"
#include "hash_utils.h"
#include "utf8_path.h"
#include "ui_operations.h"
#include "uv_analysis.h"
#include "uv_quality.h"

#include <bx/math.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <utility>

namespace woby
{
namespace
{
struct WorldVertexRemap {
    const Mesh* mesh = nullptr;
    std::vector<uint32_t> vertices;
};

void appendGroup(Mesh &result, WorldVertexRemap& remap, const UiFileState &file, size_t groupIndex, const double *parent)
{
    if (groupIndex >= file.mesh.nodes.size() || groupIndex >= file.groupSettings.size())
    {
        throw std::runtime_error("Analysis encountered an invalid mesh group.");
    }
    double local[16], model[16];
    groupTransformMatrix(file.groupSettings[groupIndex], local);
    coordinateMultiply(model, parent, local);
    const auto &group = file.mesh.nodes[groupIndex];
    const size_t end = static_cast<size_t>(group.indexOffset) + group.indexCount;
    if (end > file.mesh.indices.size() || group.indexCount % 3 != 0)
    {
        throw std::runtime_error("Analysis encountered an invalid triangle range.");
    }
    validateComparisonMeshSize(result.vertices.size(),
        (result.indices.size() + group.indexCount) / 3);
    constexpr auto missing = std::numeric_limits<uint32_t>::max();
    if (remap.mesh != &file.mesh) {
        remap.mesh = &file.mesh;
        remap.vertices.assign(file.mesh.vertices.size(), missing);
    }
    // Reuse vertices within a part, preserving source splits and triangle order.
    // A new part gets its own vertices because its transform can differ. Earlier
    // mappings precede this offset, so no whole-file clear is needed per part.
    const auto firstVertex = result.vertices.size();
    auto node = group;
    node.indexOffset = static_cast<uint32_t>(result.indices.size());
    node.sourceObjectId = file.groupSettings[groupIndex].objectId;
    node.uvQualityOffset = result.indices.size()/3;
    result.nodes.push_back(std::move(node));
    for (size_t i = group.indexOffset; i < end; ++i)
    {
        const auto sourceIndex = file.mesh.indices[i];
        auto& mapped = remap.vertices.at(sourceIndex);
        if (mapped != missing && mapped >= firstVertex) {
            result.indices.push_back(mapped);
            continue;
        }
        const auto point = transformCoordinate(model, meshPosition(file.mesh, sourceIndex));
        Vertex vertex;
        vertex.position = renderPosition(point);
        vertex.texcoord = file.mesh.vertices[sourceIndex].texcoord;
        result.precisePositions.push_back(point);
        validateComparisonMeshSize(result.vertices.size() + 1, 0);
        mapped = static_cast<uint32_t>(result.vertices.size());
        result.indices.push_back(mapped);
        result.vertices.push_back(vertex);
    }
}
// One traversal supplies both geometry snapshots and their cache signatures.
// Each canonical mesh part is visited once, irrespective of overlapping selections.
template <typename Visitor>
void visitParts(const UiState& state, ComparisonSide side, SceneObjectId id, const Visitor& visitor)
{
    const auto members = comparisonMemberIds(state, side, id);
    if (members.empty()) { return; }
    boost::unordered_flat_set<std::pair<size_t, size_t>> visited;
    visited.reserve(members.size());
    const auto part = [&](size_t fileIndex, size_t groupIndex, const double* parent) {
        if (fileIndex >= state.files.size()) { return; }
        const auto& file = state.files[fileIndex];
        if (groupIndex >= file.groupSettings.size() || groupIndex >= file.mesh.nodes.size()) { return; }
        if (!std::binary_search(members.begin(), members.end(), file.groupSettings[groupIndex].objectId)
            || file.mesh.nodes[groupIndex].indexCount == 0
            || !visited.emplace(fileIndex, groupIndex).second) { return; }
        visitor(file, groupIndex, parent);
    };
    const auto nodeVisitor = [&](auto&& self, const UiSceneNode& node, const double* parent) -> void {
        double local[16], model[16];
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
        coordinateMultiply(model, parent, local);
        if (node.kind == UiSceneNodeKind::file && node.children.empty()) {
            for (size_t i = 0; i < state.files[node.fileIndex].groupSettings.size(); ++i) {
                part(node.fileIndex, i, model);
            }
        }
        for (const auto& child : node.children) { self(self, child, model); }
    };
    double identity[16];
    coordinateIdentity(identity);
    if (state.sceneNodes.empty()) {
        for (size_t i = 0; i < state.files.size(); ++i) {
            double model[16];
            fileTransformMatrix(state.files[i].fileSettings, model);
            for (size_t j = 0; j < state.files[i].groupSettings.size(); ++j) { part(i, j, model); }
        }
    } else {
        for (const auto& node : state.sceneNodes) { nodeVisitor(nodeVisitor, node, identity); }
    }
}
} // namespace

Mesh uvLayoutMesh(const Mesh& source, SceneUpAxis upAxis, bool separated)
{
    UvExtent extent;
    for (const auto& node : source.nodes) {
        if (!node.hasTexcoords) { continue; }
        for (size_t i = node.indexOffset; i < size_t{node.indexOffset} + node.indexCount; ++i) {
            includeUv(extent, source.vertices[source.indices[i]].texcoord);
        }
    }
    Mesh result;
    result.origin = source.origin;
    result.uvQuality = source.uvQuality;
    if (extent.min[0] > extent.max[0]) { return result; }
    const auto frame = uvLayoutFrame(source.bounds, extent, upAxis);
    constexpr auto missing = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> remap(source.vertices.size(), missing);
    size_t patch = 0;
    const auto patchCount = std::count_if(source.nodes.begin(), source.nodes.end(), [](const auto& n) { return n.hasTexcoords; });
    const auto columns = static_cast<size_t>(std::ceil(std::sqrt(static_cast<double>(patchCount))));
    const double cellWidth = (extent.max[0]-extent.min[0])*frame.scale;
    const double cellHeight = (extent.max[1]-extent.min[1])*frame.scale;
    const double gap = std::max({cellWidth,cellHeight,1e-6})*.15;
    for (const auto& node : source.nodes) {
        if (!node.hasTexcoords) { continue; }
        UvExtent local;
        for (size_t i = node.indexOffset; i < size_t{node.indexOffset}+node.indexCount; ++i) { includeUv(local,source.vertices[source.indices[i]].texcoord); }
        const double offsetU = separated ? (static_cast<double>(patch%columns)-static_cast<double>(columns-1)*.5)*(cellWidth+gap) : 0;
        const auto rows = (static_cast<size_t>(patchCount)+columns-1)/columns;
        const double offsetV = separated ? (static_cast<double>(patch/columns)-static_cast<double>(rows-1)*.5)*(cellHeight+gap) : 0;
        auto patchFrame = frame;
        if (separated) { patchFrame.uvCenter = {(local.min[0]+local.max[0])*.5,(local.min[1]+local.max[1])*.5}; }
        ++patch;
        // Patches can share source vertices; display separation requires independent copies.
        const auto firstVertex = result.vertices.size();
        auto flattened = node;
        flattened.indexOffset = static_cast<uint32_t>(result.indices.size());
        result.nodes.push_back(std::move(flattened));
        for (size_t i = node.indexOffset; i < size_t{node.indexOffset} + node.indexCount; ++i) {
            const auto sourceIndex = source.indices[i];
            auto& mapped = remap[sourceIndex];
            if (mapped == missing || (separated && mapped < firstVertex)) {
                auto vertex = source.vertices[sourceIndex];
                vertex.position = uvLayoutPosition(patchFrame, vertex.texcoord);
                vertex.position[0] += static_cast<float>(offsetU);
                vertex.position[upAxis == SceneUpAxis::y ? 1 : 2] += static_cast<float>(offsetV);
                vertex.normal = upAxis == SceneUpAxis::y ? std::array<float, 3>{0, 0, 1}
                    : std::array<float, 3>{0, -1, 0};
                mapped = static_cast<uint32_t>(result.vertices.size());
                result.vertices.push_back(vertex);
                result.precisePositions.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});
            }
            result.indices.push_back(mapped);
        }
    }
    result.bounds = calculateBounds(result.vertices);
    return result;
}

std::vector<ComparisonTreeNode> comparisonTree(const UiState& state, ComparisonSide side, SceneObjectId id)
{
    const auto members = comparisonMemberIds(state, side, id, false);
    const auto enabled = comparisonMemberIds(state, side, id);
    boost::unordered_flat_set<std::pair<size_t, size_t>> visited;
    visited.reserve(members.size());
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
            if (!std::binary_search(members.begin(), members.end(), part.objectId) || meshNode.indexCount < 3 || meshNode.indexCount % 3 != 0
                || static_cast<size_t>(meshNode.indexOffset) + meshNode.indexCount > file.mesh.indices.size()
                || !visited.emplace(source.fileIndex, source.groupIndex).second) { return result; }
            result.objectId = part.objectId;
            result.partCount = 1;
            result.enabledPartCount = std::binary_search(enabled.begin(), enabled.end(), part.objectId) ? 1 : 0;
            result.triangleCount = meshNode.indexCount / 3;
            return result;
        }
        const auto append = [&](const UiSceneNode& child) {
            auto branch = self(self, child);
            if (branch.partCount == 0) { return; }
            result.partCount += branch.partCount;
            result.enabledPartCount += branch.enabledPartCount;
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
        if (node.enabledPartCount == 0) { return; }
        if (node.kind == UiSceneNodeKind::folder) {
            for (const auto& child : node.children) { self(self, child); }
        } else {
            if (!result.sourceNames.empty()) { result.sourceNames += ", "; }
            result.sourceNames += node.name;
        }
    };
    for (const auto& root : comparisonTree(state, side, id)) {
        result.partCount += root.partCount;
        result.enabledPartCount += root.enabledPartCount;
        appendNames(appendNames, root);
    }
    const std::string label = side == ComparisonSide::a ? "A" : "B";
    if (const auto* comparison = findComparison(state, id)) {
        const auto available = comparableScenePartIds(state);
        const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
        for (const auto& member : members) {
            if (!member.enabled || std::binary_search(available.begin(), available.end(), member.objectId)
                || !comparisonObjectParts(state, {member.objectId}).empty()) { continue; }
            if (result.issue.empty()) { result.issue = "Input " + label + " has missing or invalid references: "; }
            else { result.issue += ", "; }
            result.issue += member.name.empty() ? "Unnamed part" : member.name;
        }
    }
    if (!result.issue.empty()) { result.issue += ". Restore the source or remove missing references below."; }
    else if (result.partCount == 0) {
        result.issue = "Input " + label + " is empty. Use Analysis membership in the scene tree context menu.";
    }
    else if (result.enabledPartCount == 0) {
        result.issue = "Input " + label + " is turned off. Check an item to include it in the analysis.";
    }
    if (result.sourceNames.empty()) { result.sourceNames = "No available sources"; }
    return result;
}

Mesh comparisonWorldMesh(const UiState &state, ComparisonSide side, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { throw std::runtime_error("Analysis no longer exists."); }
    const auto& members = side == ComparisonSide::a ? comparison->a : comparison->b;
    const auto available = comparableScenePartIds(state);
    for (const auto& member : members) {
        if (member.enabled && !std::binary_search(available.begin(), available.end(), member.objectId)
            && comparisonObjectParts(state, {member.objectId}).empty()) {
            throw std::runtime_error("Analysis has missing or invalid source parts.");
        }
    }
    Mesh result;
    // Check index counts before allocating; shared vertices do not need one
    // world-space copy for every triangle corner.
    size_t triangleCount = 0, vertexCapacity = 0;
    visitParts(state, side, id, [&](const UiFileState& file, size_t index, const double*) {
        triangleCount += file.mesh.nodes[index].indexCount / 3;
        validateComparisonMeshSize(0, triangleCount);
        vertexCapacity += std::min(file.mesh.vertices.size(), size_t{file.mesh.nodes[index].indexCount});
    });
    result.vertices.reserve(vertexCapacity);
    result.precisePositions.reserve(vertexCapacity);
    result.origin = state.coordinateOrigin.value_or(Coordinate{});
    result.indices.reserve(triangleCount * 3);
    WorldVertexRemap remap;
    auto duplicateInput = std::make_shared<DuplicateInput>();
    boost::unordered_flat_map<SceneObjectId, size_t> sourceIndices;
    sourceIndices.reserve(state.files.size());
    duplicateInput->settings = comparison->settings.duplicates;
    visitParts(state, side, id, [&](const UiFileState& file, size_t index, const double* parent) {
        appendGroup(result, remap, file, index, parent);
        const auto [entry, inserted] = sourceIndices.try_emplace(file.objectId, duplicateInput->sources.size());
        if (inserted) {
            DuplicateSource source;
            source.fileId = file.objectId;
            source.name = pathToUtf8(file.path.filename());
            source.data = file.mesh.sourceData;
            std::copy_n(parent, 16, source.unusedPointTransform.begin());
            duplicateInput->sources.push_back(std::move(source));
        }
        auto& source = duplicateInput->sources[entry->second];
        SourcePartInstance part;
        part.partId = file.groupSettings[index].objectId;
        part.firstIndex = file.mesh.nodes[index].indexOffset;
        part.indexCount = file.mesh.nodes[index].indexCount;
        double local[16];
        groupTransformMatrix(file.groupSettings[index], local);
        coordinateMultiply(part.transform.data(), parent, local);
        source.parts.push_back(part);
        source.wholeFile = source.parts.size() == file.mesh.nodes.size();
    });
    result.duplicateInput = std::move(duplicateInput);
    if (result.indices.empty()) {
        throw std::runtime_error("Each analysis group needs at least one mesh part with triangles.");
    }
    result.bounds = calculateBounds(result.vertices);
    if (comparison->settings.type == AnalysisType::uvQuality) {
        result.uvQuality = std::make_shared<UvQuality>(analyzeUvQuality(result, comparison->settings.uvNormalization, comparison->settings.uvMetric));
    }
    if (isUvAnalysis(comparison->settings.type) && comparison->settings.uvView == UvView::layout) {
        return uvLayoutMesh(result, state.upAxis, comparison->settings.uvSeparated);
    }
    return result;
}

uint64_t comparisonGeometrySignature(const UiState &state, SceneObjectId id)
{
    if (!canInspectComparison(state, id)) { return 0; }
    uint64_t seed = 17;
    const auto settings = comparisonSettings(state, id);
    hashCombine(seed, static_cast<uint64_t>(settings.type));
    if (isUvAnalysis(settings.type)) {
        hashCombine(seed, static_cast<uint64_t>(settings.uvView));
        hashCombine(seed, settings.uvSeparated);
        hashCombine(seed, static_cast<uint64_t>(settings.uvMetric));
        hashCombine(seed, static_cast<uint64_t>(settings.uvNormalization));
        if (settings.uvView == UvView::layout) { hashCombine(seed, static_cast<uint64_t>(state.upAxis)); }
    }
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        hashCombine(seed, static_cast<uint64_t>(side));
        size_t count = 0;
        visitParts(state, side, id, [&](const UiFileState& file, size_t index, const double* parent) {
            ++count;
            hashCombine(seed, file.objectId);
            hashCombine(seed, file.groupSettings[index].objectId);
            hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.vertices.data()));
            hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.indices.data()));
            hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.sourceData.get()));
            hashCombine(seed, file.mesh.vertices.size());
            hashCombine(seed, file.mesh.indices.size());
            hashCombine(seed, file.mesh.nodes[index].indexOffset);
            hashCombine(seed, file.mesh.nodes[index].indexCount);
            hashCombine(seed, file.mesh.nodes[index].hasTexcoords);
            double local[16], model[16];
            groupTransformMatrix(file.groupSettings[index], local);
            coordinateMultiply(model, parent, local);
            for (const auto value : model) { hashDouble(seed, value); }
        });
        hashCombine(seed, count);
    }
    return seed;
}
std::optional<Bounds> comparisonDisplayBounds(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison || !canInspectComparison(state, id)) { return std::nullopt; }
    if (isUvAnalysis(comparison->settings.type) && comparison->settings.uvView == UvView::layout && comparison->settings.uvSeparated) {
        auto mesh = comparisonWorldMesh(state, ComparisonSide::a, id);
        if (mesh.vertices.empty()) { return std::nullopt; }
        auto bounds = mesh.bounds;
        for (size_t k = 0; k < 3; ++k) { bounds.min[k] += comparison->translation[k]; bounds.max[k] += comparison->translation[k]; bounds.center[k] += comparison->translation[k]; }
        return bounds;
    }
    if (isUvAnalysis(comparison->settings.type) && comparison->settings.uvView == UvView::layout) {
        // Accumulate without allocating another full mesh during camera framing.
        UvExtent uv;
        Bounds source;
        source.min.fill(std::numeric_limits<float>::infinity());
        source.max.fill(-std::numeric_limits<float>::infinity());
        visitParts(state, ComparisonSide::a, id, [&](const UiFileState& file, size_t index, const double* parent) {
            double local[16], model[16];
            groupTransformMatrix(file.groupSettings[index], local);
            coordinateMultiply(model, parent, local);
            const auto& node = file.mesh.nodes[index];
            for (size_t i = node.indexOffset; i < size_t{node.indexOffset} + node.indexCount; ++i) {
                const auto vertex = file.mesh.indices[i];
                const auto p = renderPosition(transformCoordinate(model, meshPosition(file.mesh, vertex)));
                for (size_t k = 0; k < 3; ++k) {
                    source.min[k] = std::min(source.min[k], p[k]);
                    source.max[k] = std::max(source.max[k], p[k]);
                }
                if (node.hasTexcoords) { includeUv(uv, file.mesh.vertices[vertex].texcoord); }
            }
        });
        if (uv.min[0] > uv.max[0]) { return std::nullopt; }
        for (size_t k = 0; k < 3; ++k) { source.center[k] = (source.min[k] + source.max[k]) * .5f; }
        const auto frame = uvLayoutFrame(source, uv, state.upAxis);
        std::vector<Vertex> corners(2);
        corners[0].position = uvLayoutPosition(frame, {static_cast<float>(uv.min[0]), static_cast<float>(uv.min[1])});
        corners[1].position = uvLayoutPosition(frame, {static_cast<float>(uv.max[0]), static_cast<float>(uv.max[1])});
        for (auto& corner : corners) {
            for (size_t k = 0; k < 3; ++k) { corner.position[k] += comparison->translation[k]; }
        }
        return calculateBounds(corners);
    }
    std::vector<Vertex> corners;
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        visitParts(state, side, id, [&](const UiFileState& file, size_t index, const double* parent) {
            const auto& group = file.groupSettings[index];
            double local[16], model[16];
            groupTransformMatrix(group, local);
            coordinateMultiply(model, parent, local);
            const auto& bounds = group.localBoundsValid ? group.localBounds : file.mesh.bounds;
            for (unsigned mask = 0; mask < 8; ++mask) {
                Vertex corner;
                std::array<float, 3> p{};
                for (size_t k = 0; k < 3; ++k) { p[k] = (mask & (1u << k)) ? bounds.max[k] : bounds.min[k]; }
                for (size_t k = 0; k < 3; ++k) {
                    corner.position[k] = static_cast<float>(model[k] * p[0] + model[k + 4] * p[1] + model[k + 8] * p[2]
                        + model[k + 12] + comparison->translation[k]);
                }
                if (finitePosition(corner.position)) { corners.push_back(corner); }
            }
        });
    }
    if (corners.empty()) { return std::nullopt; }
    return calculateBounds(corners);
}
} // namespace woby
