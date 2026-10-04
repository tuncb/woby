#include "scene_queries.h"
#include "comparison_runtime.h"
#include "hash_utils.h"
#include "ui_operations.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace woby {
SceneQueryStamp sceneQueryStamp(const UiState& state)
{
    return {&state, state.sceneGeneration, state.revisions.geometry, state.revisions.visibility};
}

void updateSceneTreeQueries(SceneTreeQueries& cache, const UiState& state)
{
    const auto stamp = sceneQueryStamp(state);
    const bool ownerChanged = cache.stamp.owner != &state || cache.stamp.generation != stamp.generation;
    if (cache.stamp != stamp) {
        cache.nodes.clear();
        const auto visit = [&](auto&& self, const UiSceneNode& node) -> SceneTreeSummary {
            SceneTreeSummary result;
            bool visible = true;
            if (node.kind == UiSceneNodeKind::folder) { visible = node.settings.visible; }
            else if (node.fileIndex < state.files.size()) {
                const auto& file = state.files[node.fileIndex];
                visible = file.fileSettings.visible;
                if (node.kind == UiSceneNodeKind::group) {
                    if (node.groupIndex < file.groupSettings.size()) {
                        result = {1, visible && file.groupSettings[node.groupIndex].visible ? 1u : 0u};
                    }
                } else if (node.children.empty()) {
                    result = {file.groupSettings.size(), countVisibleFileGroups(file)};
                }
            } else { visible = false; }
            for (const auto& child : node.children) {
                const auto summary = self(self, child);
                result.parts += summary.parts; result.visible += summary.visible;
            }
            if (!visible) { result.visible = 0; }
            cache.nodes[node.objectId] = result;
            return result;
        };
        for (const auto& node : state.sceneNodes) { visit(visit, node); }
        cache.stamp = stamp;
        ++cache.builds;
    }
    const auto* comparison = findComparison(state);
    const auto id = comparison ? comparison->objectId : invalidSceneObjectId;
    if (ownerChanged || !cache.membershipValid || cache.comparison != id || cache.analysis != state.revisions.analysis) {
        for (auto& members : cache.members) { members.clear(); }
        if (comparison) {
            for (const auto& member : comparison->a) { cache.members[0].insert(member.objectId); }
            for (const auto& member : comparison->b) { cache.members[1].insert(member.objectId); }
        }
        cache.comparison = id; cache.analysis = state.revisions.analysis;
        cache.membershipValid = true; ++cache.membershipBuilds;
    }
}
SceneTreeSummary sceneTreeSummary(const SceneTreeQueries& cache, SceneObjectId id)
{
    const auto it = cache.nodes.find(id);
    return it == cache.nodes.end() ? SceneTreeSummary{} : it->second;
}
bool sceneTreeMember(const SceneTreeQueries& cache, SceneObjectId id, ComparisonSide side)
{
    return id && cache.members[side == ComparisonSide::a ? 0 : 1].contains(id);
}

void updateScenePartQueries(ScenePartQueries& cache, const UiState& state)
{
    const auto stamp = sceneQueryStamp(state);
    const bool rebuild = cache.stamp != stamp;
    if (rebuild) {
        cache.records.clear();
        std::vector<SceneObjectId> ancestors;
        const auto group = [&](size_t f, size_t g, const PickMatrix& parent, float opacity, bool visible) {
            if (f >= state.files.size()) { return; }
            const auto& file = state.files[f];
            if (g >= file.groupSettings.size() || g >= file.mesh.nodes.size()) { return; }
            const auto& settings = file.groupSettings[g]; const auto& node = file.mesh.nodes[g];
            ScenePartRecord record;
            record.ancestors = ancestors;
            record.ancestors.push_back(settings.objectId);
            record.fileId = file.objectId; record.contentRevision = file.mesh.contentRevision;
            auto& part = record.value;
            part.objectId = settings.objectId; part.fileIndex = f; part.groupIndex = g; part.sourceMesh = true;
            part.indexOffset = node.indexOffset; part.indexCount = node.indexCount;
            part.lineIndexOffset = node.lineIndexOffset; part.lineIndexCount = node.lineIndexCount;
            part.pointIndexOffset = node.pointIndexOffset; part.pointIndexCount = node.pointIndexCount;
            PickMatrix local;
            groupTransformMatrix(settings, local.data());
            bx::mtxMul(part.model.data(), parent.data(), local.data());
            if (settings.localBoundsValid) { part.bounds = settings.localBounds; }
            part.solid = node.indexCount && settings.showSolidMesh;
            part.edges = node.lineIndexCount || (node.indexCount && settings.showTriangles);
            part.vertices = settings.showVertices;
            part.opacity = opacity * settings.opacity;
            record.framingVisible = visible && settings.visible && !file.mesh.vertices.empty();
            record.visible = visible && settings.visible && part.opacity > 0 && (node.pointIndexCount
                ? settings.showVertices : node.lineIndexCount || settings.showSolidMesh || settings.showTriangles || settings.showVertices);
            cache.records.push_back(std::move(record));
        };
        const auto visit = [&](auto&& self, const UiSceneNode& node, const PickMatrix& parent, float opacity, bool visible) -> void {
            ancestors.push_back(node.objectId);
            if (node.kind == UiSceneNodeKind::group) { group(node.fileIndex, node.groupIndex, parent, opacity, visible); }
            else if (node.kind == UiSceneNodeKind::folder || node.fileIndex < state.files.size()) {
                PickMatrix local, model;
                if (node.kind == UiSceneNodeKind::folder) {
                    sceneNodeTransformMatrix(node.settings, local.data());
                    opacity *= node.settings.opacity; visible = visible && node.settings.visible;
                } else {
                    const auto& file = state.files[node.fileIndex];
                    fileTransformMatrix(file.fileSettings, local.data());
                    opacity *= file.fileSettings.opacity; visible = visible && file.fileSettings.visible;
                }
                bx::mtxMul(model.data(), parent.data(), local.data());
                if (node.kind == UiSceneNodeKind::file && node.children.empty()) {
                    for (size_t g = 0; g < state.files[node.fileIndex].groupSettings.size(); ++g) { group(node.fileIndex, g, model, opacity, visible); }
                } else { for (const auto& child : node.children) { self(self, child, model, opacity, visible); } }
            }
            ancestors.pop_back();
        };
        PickMatrix identity; bx::mtxIdentity(identity.data());
        if (!state.sceneNodes.empty()) { for (const auto& node : state.sceneNodes) { visit(visit, node, identity, 1, true); } }
        else {
            for (size_t f = 0; f < state.files.size(); ++f) {
                const auto& file = state.files[f];
                PickMatrix model; fileTransformMatrix(file.fileSettings, model.data());
                ancestors = {file.objectId};
                for (size_t g = 0; g < file.groupSettings.size(); ++g) { group(f, g, model, file.fileSettings.opacity, file.fileSettings.visible); }
            }
        }
        cache.stamp = stamp; ++cache.builds;
    }
    if (rebuild || cache.picking != state.revisions.picking) {
        for (auto& record : cache.records) {
            auto& part = record.value;
            const auto& file = state.files[part.fileIndex]; const auto& group = file.groupSettings[part.groupIndex];
            part.lineWidth = part.lineIndexCount ? group.lines.width : 0;
            part.edgeXray = part.lineIndexCount ? !group.lines.depthTest : state.triangleEdgeXray;
            part.pointSize = std::round(std::clamp(state.masterVertexPointSize * file.vertexSizeScale * group.vertexSizeScale,
                minVertexPointSize, maxVertexPointSize));
        }
        cache.picking = state.revisions.picking; ++cache.displayBuilds;
    }
    if (rebuild || cache.selection != state.selectedSceneObjects) {
        const boost::unordered_flat_set<SceneObjectId> selected(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end());
        for (auto& record : cache.records) {
            record.value.selected = std::any_of(record.ancestors.begin(), record.ancestors.end(), [&](auto id) { return selected.contains(id); });
        }
        cache.selection = state.selectedSceneObjects; ++cache.selectionBuilds;
    }
}

void resolveSceneParts(SceneQueryRuntime& runtime, const UiState& state, std::vector<ScenePickPart>& parts, bool includeHidden)
{
    updateScenePartQueries(runtime.parts, state);
    parts.clear();
    for (const auto& record : runtime.parts.records) {
        if (!includeHidden && !record.visible) { continue; }
        auto part = record.value;
        if (part.fileIndex >= state.files.size()) { continue; }
        const auto& file = state.files[part.fileIndex];
        if (file.objectId != record.fileId || file.mesh.contentRevision != record.contentRevision
            || part.groupIndex >= file.groupSettings.size() || file.groupSettings[part.groupIndex].objectId != part.objectId) { continue; }
        part.mesh = &file.mesh;
        parts.push_back(part);
    }
}

void updateSceneAnnotationQueries(SceneQueryRuntime& runtime, const UiState& state)
{
    auto& cache = runtime.annotations;
    const auto stamp = sceneQueryStamp(state);
    if (cache.stamp == stamp && cache.revision == state.revisions.annotations) { return; }
    const bool rebuild = cache.stamp != stamp;
    if (cache.stamp.owner != &state || cache.stamp.generation != state.sceneGeneration) { cache.objects.clear(); }
    std::vector<ScenePickPart> parts;
    if (!state.annotations.empty()) { resolveSceneParts(runtime, state, parts); }
    std::vector<const ScenePickPart*> sources;
    boost::unordered_flat_set<SceneObjectId> live;
    for (const auto& item : state.annotations) {
        live.insert(item.objectId);
        auto& entry = cache.objects[item.objectId];
        const bool visible = item.settings.visible && item.settings.color[3] > 0;
        if (rebuild || !entry.builds || entry.revision != item.geometryRevision || entry.visible != visible || entry.valid != item.targetValid) {
            entry.id = item.objectId;
            annotationWorldLines(item, parts, entry.lines, sources);
            entry.revision = item.geometryRevision; entry.visible = visible; entry.valid = item.targetValid;
            ++entry.builds; ++cache.builds;
        }
    }
    boost::unordered::erase_if(cache.objects, [&](const auto& entry) { return !live.contains(entry.first); });
    cache.stamp = stamp; cache.revision = state.revisions.annotations;
}

void updateAnnotationProjection(AnnotationQuery& cache, const UiAnnotation& item,
    std::span<const ScenePickPart> parts, const ScenePickView& view)
{
    const auto& settings = item.settings;
    if (cache.projectedBuild == cache.builds && cache.width == settings.width
        && cache.view.view == view.view && cache.view.renderProjection == view.renderProjection
        && cache.view.width == view.width && cache.view.height == view.height && cache.view.pixelScale == view.pixelScale) { return; }
    cache.view = view; cache.width = settings.width; cache.projectedBuild = cache.builds; ++cache.projectionBuilds;
    auto& vertices = cache.projected;
    vertices.clear();
    const auto& lines = cache.lines;
    if (lines.empty() || view.width == 0 || view.height == 0) { return; }
    auto& sources = cache.sources;
    sources.clear();
    const auto vp = annotationCompose(view.view, view.renderProjection);
    for (size_t i = 0; i < std::max(size_t{1}, item.targetIds.size()); ++i) {
        const auto* source = annotationSourcePart(item, parts, static_cast<uint32_t>(i));
        if (!source) { return; }
        sources.push_back({static_cast<size_t>(source - parts.data()), annotationCompose(source->model, vp)});
    }
    const auto& geometry = item.geometry;
    vertices.reserve(lines.size() * 6);
    size_t segmentIndex = 0;
    for (const auto& line : lines) {
        const auto& segment = geometry.segments[segmentIndex++];
        const auto* target = &parts[sources[segment.source].partIndex];
        double slopeX = 0, slopeY = 0;
        if (target && target->mesh && !segment.endTriangle) {
            const auto& transform = sources[segment.source].transform;
            std::array<std::array<float, 4>, 3> corners;
            for (size_t k = 0; k < 3; ++k) {
                const auto index = target->mesh->indices[target->indexOffset + static_cast<size_t>(segment.triangle) * 3 + k];
                const auto& p = target->mesh->vertices[index].position;
                corners[k] = annotationTransform(transform, {p[0], p[1], p[2], 1});
            }
            // NDC depth is affine over the projected face. Extend the stroke in
            // that plane, preventing its uphill half from sinking into the mesh.
            if (std::all_of(corners.begin(), corners.end(), [](const auto& p) { return std::abs(p[3]) > 1e-12f; })) {
                const double x0 = corners[0][0] / corners[0][3], y0 = corners[0][1] / corners[0][3], z0 = corners[0][2] / corners[0][3];
                const double dx1 = corners[1][0] / corners[1][3] - x0, dy1 = corners[1][1] / corners[1][3] - y0, dz1 = corners[1][2] / corners[1][3] - z0;
                const double dx2 = corners[2][0] / corners[2][3] - x0, dy2 = corners[2][1] / corners[2][3] - y0, dz2 = corners[2][2] / corners[2][3] - z0;
                const double determinant = dx1 * dy2 - dx2 * dy1;
                if (std::abs(determinant) > 1e-15) {
                    slopeX = (dz1 * dy2 - dz2 * dy1) / determinant;
                    slopeY = (dx1 * dz2 - dx2 * dz1) / determinant;
                }
            }
        }
        auto a = annotationTransform(vp, {line.a[0], line.a[1], line.a[2], 1});
        auto b = annotationTransform(vp, {line.b[0], line.b[1], line.b[2], 1});
        // Clip line centers before expanding, including lines crossing the near plane.
        bool visible = true;
        for (size_t plane = 0; plane < 6; ++plane) {
            const auto distance = [&](const auto& p) {
                if (plane < 4) { return p[3] + (plane % 2 == 0 ? p[plane / 2] : -p[plane / 2]); }
                return plane == 4 ? p[2] : p[3] - p[2];
            };
            const float da = distance(a), db = distance(b);
            if (da < 0 && db < 0) { visible = false; break; }
            if ((da < 0) != (db < 0)) {
                const float t = da / (da - db);
                std::array<float, 4> p;
                for (size_t k = 0; k < 4; ++k) { p[k] = a[k] + t * (b[k] - a[k]); }
                (da < 0 ? a : b) = p;
            }
        }
        if (!visible || a[3] <= 0 || b[3] <= 0) { continue; }
        const float dx = (b[0] / b[3] - a[0] / a[3]) * static_cast<float>(view.width);
        const float dy = (b[1] / b[3] - a[1] / a[3]) * static_cast<float>(view.height);
        const float length = std::hypot(dx, dy);
        if (length < 1e-6f) { continue; }
        const float ox = -dy / length * settings.width * view.pixelScale / static_cast<float>(view.width);
        const float oy = dx / length * settings.width * view.pixelScale / static_cast<float>(view.height);
        std::array<std::array<float, 3>, 4> corners;
        for (size_t i = 0; i < 4; ++i) {
            auto p = i < 2 ? a : b;
            const float sign = i % 2 == 0 ? 1.0f : -1.0f;
            p[0] += sign * ox * p[3]; p[1] += sign * oy * p[3];
            // Reversed floating-point depth needs a relative bias toward the
            // eye. A fixed NDC offset would pull distant strokes through occluders.
            p[2] += static_cast<float>(sign * (slopeX * ox + slopeY * oy)) * p[3]
                + std::abs(p[2]) * 1e-6f;
            // Keep the expanded stroke in NDC. Inverting the combined view and
            // projection loses precision on large scenes with small near planes,
            // shifting the rendered stroke away from its handles and pick edges.
            corners[i] = {p[0] / p[3], p[1] / p[3], p[2] / p[3]};
        }
        for (size_t i : {0u, 1u, 2u, 2u, 1u, 3u}) { vertices.push_back(corners[i]); }
    }
}

void appendSceneAnnotationParts(SceneQueryRuntime& runtime, const UiState& state, std::vector<ScenePickPart>& parts)
{
    updateSceneAnnotationQueries(runtime, state);
    for (const auto& item : state.annotations) {
        const auto& entry = runtime.annotations.objects.at(item.objectId);
        ScenePickPart part;
        part.objectId = item.objectId; part.edgeXray = false; part.annotationOverlay = true;
        bx::mtxIdentity(part.model.data()); part.diagnosticEdges = entry.lines;
        parts.push_back(part);
    }
}

static void synchronizeComparisonQueries(SceneQueryRuntime& runtime, const UiState& state)
{
    if (runtime.comparisonOwner != &state || runtime.comparisonGeneration != state.sceneGeneration) { runtime.comparisons.clear(); }
    if (runtime.comparisonAnalysis != state.revisions.analysis) {
        boost::unordered::erase_if(runtime.comparisons, [&](const auto& entry) { return !findComparison(state, entry.first); });
    }
    runtime.comparisonOwner = &state; runtime.comparisonGeneration = state.sceneGeneration;
    runtime.comparisonAnalysis = state.revisions.analysis;
}

ComparisonQuery& updateSceneComparisonQuery(SceneQueryRuntime& runtime, const UiState& state, SceneObjectId id)
{
    synchronizeComparisonQueries(runtime, state);
    auto& query = runtime.comparisons[id];
    updateComparisonInspectorCache(query.inputs, state, id);
    return query;
}

uint64_t sceneComparisonRevision(const UiState& state, const ComparisonRuntimes& comparisons)
{
    uint64_t key = reinterpret_cast<uintptr_t>(&comparisons);
    hashCombine(key, comparisons.generation);
    for (const auto& comparison : state.comparisons) {
        hashCombine(key, comparison.objectId);
        hashCombine(key, comparison.diagnosticFocus.has_value());
        if (comparison.diagnosticFocus) {
            hashCombine(key, comparison.diagnosticFocus->signature); hashCombine(key, comparison.diagnosticFocus->index);
            hashCombine(key, static_cast<uint64_t>(comparison.diagnosticFocus->side));
            hashCombine(key, static_cast<uint64_t>(comparison.diagnosticFocus->category));
        }
        hashCombine(key, comparison.uvFindingFocus.has_value());
        if (comparison.uvFindingFocus) {
            hashCombine(key, comparison.uvFindingFocus->signature); hashCombine(key, comparison.uvFindingFocus->index);
        }
    }
    for (const auto& [id, runtime] : comparisons.objects) {
        hashCombine(key, id); hashCombine(key, runtime.results.revision);
        hashCombine(key, runtime.results.signature); hashCombine(key, runtime.results.cache.signature);
        hashCombine(key, runtime.results.cache.completed); hashCombine(key, runtime.gpu.uploadedStages);
        hashCombine(key, runtime.gpu.ready);
        hashCombine(key, runtime.results.value.original.source.contentRevision);
        hashCombine(key, runtime.results.value.repaired.source.contentRevision);
        for (const auto value : runtime.results.stageRevisions) { hashCombine(key, value); }
        for (const auto value : runtime.gpu.stageRevisions) { hashCombine(key, value); }
    }
    return key;
}

void updateSceneBoundsQuery(SceneQueryRuntime& runtime, UiState& state)
{
    synchronizeComparisonQueries(runtime, state);
    auto& cache = runtime.bounds;
    const auto stamp = sceneQueryStamp(state);
    if (cache.stamp == stamp && cache.analysis == state.revisions.analysis && cache.presentation == state.revisions.presentation) { return; }
    recalculateSceneBounds(state);
    cache.stamp = stamp; cache.analysis = state.revisions.analysis; cache.presentation = state.revisions.presentation;
    ++cache.builds;
}

void updateSceneSelectionQueries(SceneQueryRuntime& runtime, const UiState& state, const ComparisonRuntimes* comparisons)
{
    synchronizeComparisonQueries(runtime, state);
    auto& cache = runtime.selection;
    const auto stamp = sceneQueryStamp(state);
    const bool sourceChanged = cache.stamp != stamp || cache.selection != state.selectedSceneObjects;
    const bool boundsChanged = sourceChanged || cache.annotations != state.revisions.annotations
        || cache.analysis != state.revisions.analysis || cache.presentation != state.revisions.presentation;
    // Result publication/readiness is independent of logical scene revisions.
    const uint64_t comparisonKey = comparisons ? sceneComparisonRevision(state, *comparisons) : 0;
    const bool dimensionsChanged = sourceChanged || cache.analysis != state.revisions.analysis
        || cache.presentation != state.revisions.presentation || cache.comparisonKey != comparisonKey;
    if (!boundsChanged && !dimensionsChanged) { return; }
    updateScenePartQueries(runtime.parts, state);
    if (boundsChanged) {
        // Framing includes visible local boxes even when primitive display is off;
        // exact dimensions below deliberately use only rendered contributors.
        Bounds bounds;
        bounds.min.fill(std::numeric_limits<float>::infinity());
        bounds.max.fill(-std::numeric_limits<float>::infinity());
        const auto include = [&](const std::array<float, 3>& point) {
            if (!finitePosition(point)) { return; }
            for (size_t axis = 0; axis < 3; ++axis) {
                bounds.min[axis] = std::min(bounds.min[axis], point[axis]);
                bounds.max[axis] = std::max(bounds.max[axis], point[axis]);
            }
        };
        for (const auto& record : runtime.parts.records) {
            const auto& part = record.value;
            if (!part.selected || !record.framingVisible) { continue; }
            const auto& mesh = state.files[part.fileIndex].mesh;
            auto local = part.bounds;
            if (!local) {
                Bounds measured;
                measured.min.fill(std::numeric_limits<float>::infinity());
                measured.max.fill(-std::numeric_limits<float>::infinity());
                for (const auto index : meshNodeIndices(mesh, mesh.nodes[part.groupIndex])) {
                    if (index >= mesh.vertices.size() || !finitePosition(mesh.vertices[index].position)) { continue; }
                    for (size_t axis = 0; axis < 3; ++axis) {
                        measured.min[axis] = std::min(measured.min[axis], mesh.vertices[index].position[axis]);
                        measured.max[axis] = std::max(measured.max[axis], mesh.vertices[index].position[axis]);
                    }
                }
                local = finitePosition(measured.min) ? measured : mesh.bounds;
            }
            for (size_t corner = 0; corner < 8; ++corner) {
                std::array<float, 3> point{}, world{};
                for (size_t axis = 0; axis < 3; ++axis) { point[axis] = corner & (size_t{1} << axis) ? local->max[axis] : local->min[axis]; }
                // Match the existing framing calculation's float arithmetic.
                for (size_t axis = 0; axis < 3; ++axis) {
                    world[axis] = part.model[axis] * point[0] + part.model[4 + axis] * point[1]
                        + part.model[8 + axis] * point[2] + part.model[12 + axis];
                }
                include(world);
            }
        }
        for (const auto& comparison : state.comparisons) {
            if (!comparison.settings.enabled || !sceneObjectSelected(state, comparison.objectId)) { continue; }
            auto& query = updateSceneComparisonQuery(runtime, state, comparison.objectId);
            uint64_t key = query.inputs.boundsSignature;
            for (const auto value : comparison.translation) { hashFloat(key, value); }
            if (!query.boundsValid || query.boundsSignature != key) {
                query.bounds = comparisonDisplayBounds(state, comparison.objectId, query.inputs.boundsSignature);
                query.boundsSignature = key; query.boundsValid = true; ++query.boundsBuilds;
            }
            if (query.bounds) { include(query.bounds->min); include(query.bounds->max); }
        }
        updateSceneAnnotationQueries(runtime, state);
        for (const auto& item : state.annotations) {
            if (!sceneObjectSelected(state, item.objectId)) { continue; }
            for (const auto& line : runtime.annotations.objects.at(item.objectId).lines) { include(line.a); include(line.b); }
        }
        cache.bounds.reset();
        if (finitePosition(bounds.min)) {
            float radiusSquared = 0;
            for (size_t axis = 0; axis < 3; ++axis) {
                bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * .5f;
                const float extent = std::max(bounds.max[axis] - bounds.center[axis], bounds.center[axis] - bounds.min[axis]);
                radiusSquared += extent * extent;
            }
            bounds.radius = std::max(.001f, std::sqrt(radiusSquared));
            cache.bounds = bounds;
        }
        ++cache.boundsBuilds;
    }
    if (dimensionsChanged) {
        std::vector<ScenePickPart> parts;
        resolveSceneParts(runtime, state, parts);
        if (comparisons) {
            for (const auto& comparison : state.comparisons) {
                const auto it = comparisons->objects.find(comparison.objectId);
                if (!comparison.settings.enabled || it == comparisons->objects.end()
                    || !comparisonStagesReady(it->second, state, comparison.objectId, comparisonSource, true)) { continue; }
                const auto first = parts.size();
                appendComparisonPickParts(parts, comparison, readyComparisonSettings(it->second, state, comparison.objectId),
                    it->second.results.value, sceneObjectSelected(state, comparison.objectId));
                for (size_t i = first; i < parts.size(); ++i) {
                    if (parts[i].objectId != comparison.objectId) { parts[i].selected = sceneObjectSelected(state, parts[i].objectId); }
                }
            }
        }
        cache.dimensions = sceneDimensions(parts); ++cache.dimensionBuilds;
    }
    cache.stamp = stamp; cache.selection = state.selectedSceneObjects;
    cache.annotations = state.revisions.annotations; cache.analysis = state.revisions.analysis;
    cache.presentation = state.revisions.presentation; cache.comparisonKey = comparisonKey;
}
} // namespace woby
