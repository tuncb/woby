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

void appendGroup(Mesh &result, WorldVertexRemap& remap, const UiFileState &file, size_t groupIndex, const double *parent, std::stop_token stop)
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
        if (i % 16384 == 0 && stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
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

Mesh uvLayoutMesh(const Mesh& source, bool separated, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
    UvExtent extent;
    for (const auto& node : source.nodes) {
        if (!node.hasTexcoords) { continue; }
        for (size_t i = node.indexOffset; i < size_t{node.indexOffset} + node.indexCount; ++i) {
            if (i % 16384 == 0 && stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
            includeUv(extent, source.vertices[source.indices[i]].texcoord);
        }
    }
    Mesh result;
    result.origin = source.origin;
    result.uvQuality = source.uvQuality;
    if (extent.min[0] > extent.max[0]) { return result; }
    const auto frame = uvLayoutFrame(source.bounds, extent);
    constexpr auto missing = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> remap(source.vertices.size(), missing);
    size_t patch = 0;
    const auto patchCount = std::count_if(source.nodes.begin(), source.nodes.end(), [](const auto& n) { return n.hasTexcoords; });
    for (const auto& node : source.nodes) {
        if (!node.hasTexcoords) { continue; }
        UvExtent local;
        for (size_t i = node.indexOffset; i < size_t{node.indexOffset}+node.indexCount; ++i) {
            if (i % 16384 == 0 && stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
            includeUv(local, source.vertices[source.indices[i]].texcoord);
        }
        const auto placement = uvPatchLayout(frame, extent, local, patch++, static_cast<size_t>(patchCount), separated);
        // Patches can share source vertices; display separation requires independent copies.
        const auto firstVertex = result.vertices.size();
        auto flattened = node;
        flattened.indexOffset = static_cast<uint32_t>(result.indices.size());
        result.nodes.push_back(std::move(flattened));
        for (size_t i = node.indexOffset; i < size_t{node.indexOffset} + node.indexCount; ++i) {
            if (i % 16384 == 0 && stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
            const auto sourceIndex = source.indices[i];
            auto& mapped = remap[sourceIndex];
            if (mapped == missing || (separated && mapped < firstVertex)) {
                auto vertex = source.vertices[sourceIndex];
                vertex.position = uvPatchPosition(placement, vertex.texcoord);
                vertex.normal = {0, -1, 0};
                mapped = static_cast<uint32_t>(result.vertices.size());
                result.vertices.push_back(vertex);
                result.precisePositions.push_back({vertex.position[0], vertex.position[1], vertex.position[2]});
            }
            result.indices.push_back(mapped);
        }
    }
    result.bounds = calculateBounds(result.vertices, [&](const ModelLoadProgress&) {
        if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
    });
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

static void validateComparisonInput(const UiState& state, ComparisonSide side, SceneObjectId id)
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
}

template <typename Visit>
static Mesh buildComparisonWorldMesh(const Visit& visit, const ComparisonSettings& settings,
    const Coordinate& origin, std::stop_token stop)
{
    if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
    Mesh result;
    // Check index counts before allocating; shared vertices do not need one
    // world-space copy for every triangle corner.
    size_t triangleCount = 0, vertexCapacity = 0;
    visit([&](const UiFileState& file, size_t index, const double*) {
        triangleCount += file.mesh.nodes[index].indexCount / 3;
        validateComparisonMeshSize(0, triangleCount);
        vertexCapacity += std::min(file.mesh.vertices.size(), size_t{file.mesh.nodes[index].indexCount});
    });
    result.vertices.reserve(vertexCapacity);
    result.precisePositions.reserve(vertexCapacity);
    result.origin = origin;
    result.indices.reserve(triangleCount * 3);
    WorldVertexRemap remap;
    auto duplicateInput = std::make_shared<DuplicateInput>();
    boost::unordered_flat_map<SceneObjectId, size_t> sourceIndices;
    duplicateInput->settings = settings.duplicates;
    visit([&](const UiFileState& file, size_t index, const double* parent) {
        appendGroup(result, remap, file, index, parent, stop);
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
    result.bounds = calculateBounds(result.vertices, [&](const ModelLoadProgress&) {
        if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
    });
    if (settings.type == AnalysisType::uvQuality) {
        result.uvQuality = std::make_shared<UvQuality>(analyzeUvQuality(result, settings.uvNormalization, settings.uvMetric, stop));
    }
    if (isUvAnalysis(settings.type) && settings.uvView == UvView::layout) {
        return uvLayoutMesh(result, settings.uvSeparated, stop);
    }
    return result;
}

Mesh comparisonWorldMesh(const UiState& state, ComparisonSide side, SceneObjectId id)
{
    validateComparisonInput(state, side, id);
    return buildComparisonWorldMesh([&](const auto& visitor) { visitParts(state, side, id, visitor); },
        comparisonSettings(state, id), state.coordinateOrigin.value_or(Coordinate{}), {});
}

ComparisonInputSnapshot snapshotComparisonInputs(const UiState& state, SceneObjectId id)
{
    ComparisonInputSnapshot snapshot;
    snapshot.settings = comparisonSettings(state, id);
    snapshot.origin = state.coordinateOrigin.value_or(Coordinate{});
    boost::unordered_flat_map<SceneObjectId, size_t> files;
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        validateComparisonInput(state, side, id);
        visitParts(state, side, id, [&](const UiFileState& file, size_t index, const double* parent) {
            const auto [entry, inserted] = files.try_emplace(file.objectId, snapshot.files.size());
            if (inserted) { snapshot.files.push_back(file); }
            ComparisonInputPart part;
            part.fileIndex = entry->second;
            part.groupIndex = index;
            std::copy_n(parent, 16, part.parent.begin());
            snapshot.parts[side == ComparisonSide::a ? 0 : 1].push_back(part);
        });
    }
    return snapshot;
}

Mesh comparisonWorldMesh(const ComparisonInputSnapshot& snapshot, ComparisonSide side, std::stop_token stop)
{
    const auto& parts = snapshot.parts[side == ComparisonSide::a ? 0 : 1];
    if (parts.empty()) { return {}; }
    return buildComparisonWorldMesh([&](const auto& visitor) {
        for (const auto& part : parts) {
            visitor(snapshot.files.at(part.fileIndex), part.groupIndex, part.parent.data());
        }
    }, snapshot.settings, snapshot.origin, stop);
}

PreparedComparisonInputs prepareUvComparisonInputs(const ComparisonInputSnapshot& snapshot, std::stop_token stop)
{
    PreparedComparisonInputs result;
    auto meshes = std::make_shared<std::array<Mesh, 2>>();
    const auto check = [&] {
        if (stop.stop_requested()) { throw std::runtime_error("Analysis canceled."); }
    };
    check();
    for (size_t side = 0; side < 2; ++side) {
        auto& mesh = (*meshes)[side];
        auto& buffers = result.buffers[side];
        mesh = comparisonWorldMesh(snapshot, side == 0 ? ComparisonSide::a : ComparisonSide::b, stop);
        if (mesh.indices.empty()) { continue; }
        // Match the previous upload path: quality uses the original normals,
        // while the indexed surface uses generated smooth normals.
        buffers.quality = uvQualityVertices(mesh, stop);
        generateSmoothNormals(mesh.vertices, mesh.indices, [&](const ModelLoadProgress&) { check(); });
        buffers.lines.reserve(mesh.indices.size() * 2);
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
            if (i % 12288 == 0) { check(); }
            for (size_t k = 0; k < 3; ++k) {
                buffers.lines.push_back(mesh.indices[i + k]);
                buffers.lines.push_back(mesh.indices[i + (k + 1) % 3]);
            }
        }
    }
    check();
    result.meshes = std::move(meshes);
    return result;
}

static uint64_t comparisonSignature(const UiState& state, SceneObjectId id, bool boundsOnly)
{
    if (!canInspectComparison(state, id)) { return 0; }
    uint64_t seed = 17;
    const auto settings = comparisonSettings(state, id);
    hashCombine(seed, boundsOnly ? static_cast<uint64_t>(isUvAnalysis(settings.type)) : static_cast<uint64_t>(settings.type));
    if (isUvAnalysis(settings.type)) {
        hashCombine(seed, static_cast<uint64_t>(settings.uvView));
        hashCombine(seed, settings.uvSeparated);
        if (!boundsOnly) {
            hashCombine(seed, static_cast<uint64_t>(settings.uvMetric));
            hashCombine(seed, static_cast<uint64_t>(settings.uvNormalization));
        }
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
            hashCombine(seed, reinterpret_cast<uintptr_t>(file.mesh.precisePositions.data()));
            hashCombine(seed, file.mesh.precisePositions.size());
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
uint64_t comparisonGeometrySignature(const UiState& state, SceneObjectId id)
{
    return comparisonSignature(state, id, false);
}

static std::optional<Bounds> calculateComparisonDisplayBounds(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison || !canInspectComparison(state, id)) { return std::nullopt; }
    if (isUvAnalysis(comparison->settings.type) && comparison->settings.uvView == UvView::layout) {
        // Accumulate without allocating another full mesh during camera framing.
        UvExtent uv;
        std::vector<UvExtent> patches;
        Bounds source;
        source.min.fill(std::numeric_limits<float>::infinity());
        source.max.fill(-std::numeric_limits<float>::infinity());
        visitParts(state, ComparisonSide::a, id, [&](const UiFileState& file, size_t index, const double* parent) {
            double local[16], model[16];
            groupTransformMatrix(file.groupSettings[index], local);
            coordinateMultiply(model, parent, local);
            const auto& node = file.mesh.nodes[index];
            UvExtent patch;
            for (size_t i = node.indexOffset; i < size_t{node.indexOffset} + node.indexCount; ++i) {
                const auto vertex = file.mesh.indices[i];
                const auto p = renderPosition(transformCoordinate(model, meshPosition(file.mesh, vertex)));
                for (size_t k = 0; k < 3; ++k) {
                    source.min[k] = std::min(source.min[k], p[k]);
                    source.max[k] = std::max(source.max[k], p[k]);
                }
                if (node.hasTexcoords) {
                    includeUv(uv, file.mesh.vertices[vertex].texcoord);
                    includeUv(patch, file.mesh.vertices[vertex].texcoord);
                }
            }
            if (node.hasTexcoords) { patches.push_back(patch); }
        });
        if (uv.min[0] > uv.max[0]) { return std::nullopt; }
        for (size_t k = 0; k < 3; ++k) { source.center[k] = (source.min[k] + source.max[k]) * .5f; }
        const auto frame = uvLayoutFrame(source, uv);
        std::vector<Vertex> corners;
        if (!comparison->settings.uvSeparated) {
            corners.resize(2);
            corners[0].position = uvLayoutPosition(frame, {static_cast<float>(uv.min[0]), static_cast<float>(uv.min[1])});
            corners[1].position = uvLayoutPosition(frame, {static_cast<float>(uv.max[0]), static_cast<float>(uv.max[1])});
            return calculateBounds(corners);
        }
        std::vector<UvPatchLayout> placements;
        for (size_t patch = 0; patch < patches.size(); ++patch) {
            const auto& extent = patches[patch];
            placements.push_back(uvPatchLayout(frame, uv, extent, patch, patches.size(), true));
            for (const auto& uvCorner : {extent.min, extent.max}) {
                Vertex corner;
                corner.position = uvPatchPosition(placements.back(), {static_cast<float>(uvCorner[0]), static_cast<float>(uvCorner[1])});
                corners.push_back(corner);
            }
        }
        auto bounds = calculateBounds(corners);
        // Preserve the mesh's exact framing sphere too. Extent corners can be
        // absent from irregular patches; they are sufficient only for the AABB.
        float radiusSquared = 0;
        size_t patch = 0;
        visitParts(state, ComparisonSide::a, id, [&](const UiFileState& file, size_t index, const double*) {
            const auto& node = file.mesh.nodes[index];
            if (!node.hasTexcoords) { return; }
            const auto& placement = placements[patch++];
            for (size_t i = node.indexOffset; i < size_t{node.indexOffset}+node.indexCount; ++i) {
                const auto position = uvPatchPosition(placement, file.mesh.vertices[file.mesh.indices[i]].texcoord);
                const float x = position[0]-bounds.center[0], y = position[1]-bounds.center[1], z = position[2]-bounds.center[2];
                radiusSquared = std::max(radiusSquared, x*x+y*y+z*z);
            }
        });
        bounds.radius = std::max(std::sqrt(radiusSquared), .001f);
        return bounds;
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
std::optional<Bounds> comparisonDisplayBounds(const UiState& state, SceneObjectId id)
{
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return std::nullopt; }
    if (!isUvAnalysis(comparison->settings.type) || comparison->settings.uvView != UvView::layout) {
        return calculateComparisonDisplayBounds(state, id);
    }
    const auto signature = comparisonSignature(state, id, true);
    if (!signature) { return std::nullopt; }
    if (!comparison->boundsCache || comparison->boundsCache->signature != signature) {
        comparison->boundsCache = std::make_shared<UiComparison::BoundsCache>(
            UiComparison::BoundsCache{signature, calculateComparisonDisplayBounds(state, id)});
    }
    auto bounds = comparison->boundsCache->bounds;
    if (bounds) {
        for (size_t k = 0; k < 3; ++k) {
            bounds->min[k] += comparison->translation[k];
            bounds->max[k] += comparison->translation[k];
            bounds->center[k] = comparison->settings.uvSeparated ? bounds->center[k] + comparison->translation[k]
                : (bounds->min[k] + bounds->max[k]) * .5f;
        }
        if (!comparison->settings.uvSeparated) {
            std::vector<Vertex> corners(2);
            corners[0].position = bounds->min;
            corners[1].position = bounds->max;
            *bounds = calculateBounds(corners);
        }
    }
    return bounds;
}
} // namespace woby
