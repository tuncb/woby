#include "comparison_placement.h"
#include "ui_operations.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <boost/unordered/unordered_flat_map.hpp>

namespace woby {
namespace {
template <typename Visitor>
void visitNodes(const std::vector<UiSceneNode>& nodes, const Visitor& visitor)
{
    for (const auto& node : nodes) {
        visitor(node);
        visitNodes(node.children, visitor);
    }
}

void translate(CoordinateMatrix& matrix, const Coordinate& offset)
{
    for (size_t k = 0; k < 3; ++k) { matrix[12 + k] += offset[k]; }
}
}

std::vector<ComparisonSourceTransform> comparisonSourceTransforms(const UiState& state)
{
    std::vector<ComparisonSourceTransform> result;
    CoordinateMatrix identity;
    coordinateIdentity(identity.data());
    const auto append = [&](SceneObjectId id, size_t fileIndex, size_t groupIndex,
                            const CoordinateMatrix& parent, const CoordinateMatrix& local, const Coordinate& center) {
        ComparisonSourceTransform transform;
        transform.objectId = id; transform.fileIndex = fileIndex; transform.groupIndex = groupIndex;
        transform.parent = parent;
        coordinateMultiply(transform.model.data(), local.data(), parent.data());
        transform.origin = transformCoordinate(transform.model.data(), center);
        result.push_back(transform);
        return transform.model;
    };
    const auto group = [&](size_t f, size_t g, const CoordinateMatrix& parent) {
        if (f >= state.files.size()) { return; }
        const auto& file = state.files[f];
        if (g >= file.groupSettings.size() || g >= meshGeometry(file.mesh).nodes.size()) { return; }
        const auto& settings = file.groupSettings[g];
        CoordinateMatrix local;
        groupTransformMatrix(settings, local.data());
        append(settings.objectId, f, g, parent, local, {settings.center[0], settings.center[1], settings.center[2]});
    };
    const auto file = [&](size_t f, const CoordinateMatrix& parent) {
        const auto& source = state.files[f];
        const auto& settings = source.fileSettings;
        CoordinateMatrix local;
        fileTransformMatrix(settings, local.data());
        return append(source.objectId, f, invalidSceneNodeIndex, parent, local,
            {settings.center[0], settings.center[1], settings.center[2]});
    };
    const auto visit = [&](auto&& self, const UiSceneNode& node, const CoordinateMatrix& parent) -> void {
        if (node.kind == UiSceneNodeKind::group) { group(node.fileIndex, node.groupIndex, parent); return; }
        CoordinateMatrix model;
        if (node.kind == UiSceneNodeKind::folder) {
            CoordinateMatrix local;
            sceneNodeTransformMatrix(node.settings, local.data());
            model = append(node.objectId, invalidSceneNodeIndex, invalidSceneNodeIndex, parent, local,
                {node.settings.center[0], node.settings.center[1], node.settings.center[2]});
        } else {
            if (node.fileIndex >= state.files.size()) { return; }
            model = file(node.fileIndex, parent);
            if (node.children.empty()) {
                for (size_t g = 0; g < state.files[node.fileIndex].groupSettings.size(); ++g) { group(node.fileIndex, g, model); }
            }
        }
        for (const auto& child : node.children) { self(self, child, model); }
    };
    if (state.sceneNodes.empty()) {
        for (size_t f = 0; f < state.files.size(); ++f) {
            const auto model = file(f, identity);
            for (size_t g = 0; g < state.files[f].groupSettings.size(); ++g) { group(f, g, model); }
        }
    } else {
        for (const auto& node : state.sceneNodes) { visit(visit, node, identity); }
    }
    return result;
}

Coordinate comparisonAlignmentOffset(const UiComparison& comparison, ComparisonSide side,
    const std::vector<ComparisonSourceTransform>& transforms)
{
    Coordinate offset{};
    if (!comparison.settings.alignOrigins || isUvAnalysis(comparison.settings.type)) { return offset; }
    const auto moving = comparison.settings.distanceOnOriginal ? ComparisonSide::a : ComparisonSide::b;
    if (side != moving) { return offset; }
    const auto origin = [&](size_t index) -> const ComparisonSourceTransform* {
        const auto found = std::find_if(transforms.begin(), transforms.end(), [&](const auto& transform) {
            return transform.objectId == comparison.origins[index];
        });
        return found == transforms.end() ? nullptr : &*found;
    };
    const auto* a = origin(0);
    const auto* b = origin(1);
    if (a && b) {
        for (size_t k = 0; k < 3; ++k) {
            offset[k] = comparison.settings.distanceOnOriginal ? b->origin[k] - a->origin[k] : a->origin[k] - b->origin[k];
        }
    }
    return offset;
}

Coordinate enabledPartsAlignmentOffset(const UiState& state, const UiComparison& comparison,
    const std::vector<ComparisonSourceTransform>& transforms, bool captured)
{
    std::array<Coordinate, 2> centers{};
    boost::unordered_flat_map<SceneObjectId, const ComparisonSourceTransform*> byId;
    for (const auto& transform : transforms) { byId.emplace(transform.objectId, &transform); }
    for (size_t side = 0; side < 2; ++side) {
        Coordinate minimum, maximum;
        minimum.fill(std::numeric_limits<double>::infinity());
        maximum.fill(-std::numeric_limits<double>::infinity());
        bool any = false;
        for (const auto& part : side == 0 ? comparison.a : comparison.b) {
            if (!part.enabled) { continue; }
            const auto found = byId.find(part.objectId);
            if (found == byId.end() || found->second->groupIndex == invalidSceneNodeIndex) { return {}; }
            const auto& transform = *found->second;
            const auto& file = state.files[transform.fileIndex];
            const auto& node = meshGeometry(file.mesh).nodes[transform.groupIndex];
            if (node.indexCount < 3) { continue; }
            auto parent = transform.parent, model = transform.model;
            if (captured && part.placement) { resolveComparisonPlacement(state, file, *part.placement, parent, model); }
            const auto& group = file.groupSettings[transform.groupIndex];
            std::array<Coordinate, 2> bounds;
            const auto& cache = file.mesh.annotationCache;
            if (cache && transform.groupIndex < cache->localBounds.size() && cache->localBounds[transform.groupIndex]) {
                bounds = *cache->localBounds[transform.groupIndex];
            } else {
                const auto& local = group.localBoundsValid ? group.localBounds : file.mesh.bounds;
                for (size_t k = 0; k < 3; ++k) { bounds[0][k] = local.min[k]; bounds[1][k] = local.max[k]; }
            }
            for (unsigned corner = 0; corner < 8; ++corner) {
                Coordinate point;
                for (size_t k = 0; k < 3; ++k) { point[k] = bounds[(corner >> k) & 1][k]; }
                point = transformCoordinate(model.data(), point);
                for (size_t k = 0; k < 3; ++k) {
                    minimum[k] = std::min(minimum[k], point[k]);
                    maximum[k] = std::max(maximum[k], point[k]);
                }
                any = true;
            }
        }
        if (!any) { return {}; }
        for (size_t k = 0; k < 3; ++k) { centers[side][k] = minimum[k] + (maximum[k] - minimum[k]) * .5; }
    }
    Coordinate offset;
    for (size_t k = 0; k < 3; ++k) { offset[k] = centers[1][k] - centers[0][k]; }
    return offset;
}

bool comparisonHasSourcePlacements(const UiComparison& comparison)
{
    const auto known = [](const auto& members) {
        return std::all_of(members.begin(), members.end(), [](const auto& part) {
            return !part.placement || part.placement->alignmentOffset.has_value();
        });
    };
    return known(comparison.a) && known(comparison.b);
}

void clearComparisonAlignment(UiComparison& comparison)
{
    for (auto* members : {&comparison.a, &comparison.b}) {
        for (auto& part : *members) {
            if (!part.placement || !part.placement->alignmentOffset) { continue; }
            auto& placement = *part.placement;
            Coordinate offset;
            for (size_t k = 0; k < 3; ++k) { offset[k] = -(*placement.alignmentOffset)[k]; }
            translate(placement.parent, offset); translate(placement.model, offset);
            placement.alignmentOffset = Coordinate{};
        }
    }
}

void realignComparisonInputs(UiState& state, UiComparison& comparison)
{
    if (comparison.settings.type != AnalysisType::surfaceComparison || !comparison.settings.alignOrigins) { return; }
    comparison.settings.alignEnabledParts = true;
    if (!comparison.settings.lockPlacement) { return; }
    const auto offset = enabledPartsAlignmentOffset(state, comparison, comparisonSourceTransforms(state), true);
    // Keep B fixed. A receives one translation, including its disabled parts.
    for (auto& part : comparison.a) {
        if (!part.placement) { continue; }
        auto& placement = *part.placement;
        translate(placement.parent, offset); translate(placement.model, offset);
        if (placement.alignmentOffset) {
            for (size_t k = 0; k < 3; ++k) { (*placement.alignmentOffset)[k] += offset[k]; }
        }
    }
}

SceneComparisonPartRecord::Placement comparisonPartPlacement(const UiState& state,
    const ComparisonSourceTransform& transform, const Coordinate& offset)
{
    SceneComparisonPartRecord::Placement placement;
    placement.parent = transform.parent;
    placement.model = transform.model;
    placement.alignmentOffset = offset;
    translate(placement.parent, offset);
    translate(placement.model, offset);
    placement.sourceOrigin = state.files.at(transform.fileIndex).mesh.origin;
    placement.sceneOrigin = state.coordinateOrigin.value_or(Coordinate{});
    return placement;
}

void resolveComparisonPlacement(const UiState& state, const UiFileState& file,
    const SceneComparisonPartRecord::Placement& placement, CoordinateMatrix& parent, CoordinateMatrix& model)
{
    CoordinateMatrix rebase;
    coordinateIdentity(rebase.data());
    for (size_t k = 0; k < 3; ++k) { rebase[12 + k] = file.mesh.origin[k] - placement.sourceOrigin[k]; }
    coordinateMultiply(parent.data(), rebase.data(), placement.parent.data());
    coordinateMultiply(model.data(), rebase.data(), placement.model.data());
    const auto sceneOrigin = state.coordinateOrigin.value_or(Coordinate{});
    Coordinate offset;
    for (size_t k = 0; k < 3; ++k) { offset[k] = placement.sceneOrigin[k] - sceneOrigin[k]; }
    translate(parent, offset);
    translate(model, offset);
}

bool validComparisonPlacement(const SceneComparisonPartRecord::Placement& placement)
{
    const auto finite = [](const auto& values) {
        return std::all_of(values.begin(), values.end(), [](double v) { return std::isfinite(v); });
    };
    const auto affine = [&](const auto& m) {
        return finite(m) && m[3] == 0 && m[7] == 0 && m[11] == 0 && m[15] == 1;
    };
    return affine(placement.parent) && affine(placement.model) && finite(placement.sourceOrigin) && finite(placement.sceneOrigin)
        && (!placement.alignmentOffset || finite(*placement.alignmentOffset));
}

bool comparisonOriginCandidate(const UiState& state, const UiComparison& comparison, ComparisonSide side, SceneObjectId id)
{
    if (!id) { return false; }
    const auto parts = comparisonObjectParts(state, {id});
    const auto& members = side == ComparisonSide::a ? comparison.a : comparison.b;
    return std::any_of(members.begin(), members.end(), [&](const auto& part) {
        return std::binary_search(parts.begin(), parts.end(), part.objectId);
    });
}

void synchronizeComparisonPlacement(UiState& state, UiComparison& comparison)
{
    if (!comparison.settings.alignOrigins && !comparison.settings.lockPlacement
        && comparison.settings.type != AnalysisType::surfaceComparison
        && !comparison.origins[0] && !comparison.origins[1]) { return; }
    for (size_t side = 0; side < 2; ++side) {
        auto& members = side == 0 ? comparison.a : comparison.b;
        if (!comparisonOriginCandidate(state, comparison, side == 0 ? ComparisonSide::a : ComparisonSide::b, comparison.origins[side])) {
            comparison.origins[side] = invalidSceneObjectId;
            for (const auto& part : members) {
                if (const auto object = findSceneObject(state, part.objectId)) {
                    comparison.origins[side] = object->fileId ? object->fileId : part.objectId;
                    break;
                }
            }
        }
        if (!comparison.settings.lockPlacement) {
            for (auto& part : members) { part.placement.reset(); }
        }
    }
    if (!comparison.settings.lockPlacement || isUvAnalysis(comparison.settings.type)
        || comparison.a.empty() || comparison.b.empty()) { return; }
    const auto captured = [](const auto& members) {
        return std::all_of(members.begin(), members.end(), [](const auto& part) { return part.placement.has_value(); });
    };
    if (captured(comparison.a) && captured(comparison.b)) { return; }
    const auto transforms = comparisonSourceTransforms(state);
    boost::unordered_flat_map<SceneObjectId, const ComparisonSourceTransform*> byId;
    for (const auto& transform : transforms) { byId.emplace(transform.objectId, &transform); }
    for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
        auto offset = comparison.settings.alignEnabledParts ? Coordinate{} : comparisonAlignmentOffset(comparison, side, transforms);
        if (comparison.settings.alignEnabledParts) {
            const auto& members = side == ComparisonSide::a ? comparison.a : comparison.b;
            for (const auto& part : members) {
                if (part.placement && part.placement->alignmentOffset) { offset = *part.placement->alignmentOffset; break; }
            }
        }
        for (auto& part : side == ComparisonSide::a ? comparison.a : comparison.b) {
            if (part.placement) { continue; }
            if (const auto found = byId.find(part.objectId); found != byId.end()
                && found->second->groupIndex != invalidSceneNodeIndex) {
                part.placement = comparisonPartPlacement(state, *found->second, offset);
            }
        }
    }
    if (comparison.settings.alignEnabledParts) { realignComparisonInputs(state, comparison); }
}

SceneComparisonOriginRecord comparisonOriginReference(const UiState& state, SceneObjectId id)
{
    SceneComparisonOriginRecord reference;
    if (!id) { return reference; }
    for (size_t f = 0; f < state.files.size(); ++f) {
        const auto& file = state.files[f];
        if (file.objectId == id) { reference.fileIndex = static_cast<int>(f); return reference; }
        for (size_t g = 0; g < file.groupSettings.size(); ++g) {
            if (file.groupSettings[g].objectId == id) {
                reference.fileIndex = static_cast<int>(f); reference.groupIndex = static_cast<int>(g); return reference;
            }
        }
    }
    int index = 0;
    visitNodes(state.sceneNodes, [&](const auto& node) {
        if (node.kind == UiSceneNodeKind::folder && node.objectId == id) { reference.nodeIndex = index; }
        ++index;
    });
    return reference;
}

SceneObjectId comparisonOriginObject(const UiState& state, const SceneComparisonOriginRecord& reference)
{
    if (reference.fileIndex >= 0 && static_cast<size_t>(reference.fileIndex) < state.files.size()) {
        const auto& file = state.files[reference.fileIndex];
        if (reference.groupIndex == -1) { return file.objectId; }
        if (reference.groupIndex >= 0 && static_cast<size_t>(reference.groupIndex) < file.groupSettings.size()) {
            return file.groupSettings[reference.groupIndex].objectId;
        }
    }
    SceneObjectId id = invalidSceneObjectId;
    int index = 0;
    visitNodes(state.sceneNodes, [&](const auto& node) {
        if (index == reference.nodeIndex && node.kind == UiSceneNodeKind::folder) { id = node.objectId; }
        ++index;
    });
    return id;
}
} // namespace woby
