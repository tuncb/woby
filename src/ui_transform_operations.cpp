#include "ui_operations.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

namespace woby {
namespace {

constexpr size_t noParent = static_cast<size_t>(-1);
using TransformBounds = std::array<Coordinate, 2>;

struct TransformObject {
    SceneObjectId id = 0;
    size_t parent = noParent, end = 0;
    CoordinateMatrix local{}, world{};
    Coordinate pivot{};
    std::array<float, 3> translation{};
    TransformBounds bounds{};
    bool hasBounds = false, hasGeometry = false;
};

void includePoint(TransformObject& object, const Coordinate& point)
{
    if (!finiteCoordinate(point)) { return; }
    if (!object.hasBounds) { object.bounds = {point, point}; object.hasBounds = true; }
    else {
        for (size_t k = 0; k < 3; ++k) {
            object.bounds[0][k] = std::min(object.bounds[0][k], point[k]);
            object.bounds[1][k] = std::max(object.bounds[1][k], point[k]);
        }
    }
}

std::vector<TransformObject> transformObjects(const UiState& state)
{
    std::vector<TransformObject> objects;
    const auto append = [&](SceneObjectId id, size_t parent, const CoordinateMatrix& local,
                            const auto& settings) {
        TransformObject object;
        object.id = id; object.parent = parent; object.local = local;
        object.translation = settings.translation;
        if (parent == noParent) { object.world = local; }
        else { coordinateMultiply(object.world.data(), local.data(), objects[parent].world.data()); }
        object.pivot = transformCoordinate(object.world.data(),
            {settings.center[0], settings.center[1], settings.center[2]});
        objects.push_back(object);
        return objects.size() - 1;
    };
    const auto group = [&](size_t fileIndex, size_t groupIndex, size_t parent) {
        if (fileIndex >= state.files.size()) { return; }
        const auto& file = state.files[fileIndex];
        if (groupIndex >= file.groupSettings.size() || groupIndex >= meshGeometry(file.mesh).nodes.size()) { return; }
        const auto& settings = file.groupSettings[groupIndex];
        CoordinateMatrix local;
        groupTransformMatrix(settings, local.data());
        const size_t index = append(settings.objectId, parent, local, settings);
        auto& object = objects[index];
        const auto& node = meshGeometry(file.mesh).nodes[groupIndex];
        object.hasGeometry = node.indexCount || node.lineIndexCount || node.pointIndexCount;
        // Prepared local boxes avoid a mesh scan on the UI thread. Visibility and
        // render modes do not exclude any selected geometry from placement.
        if (object.hasGeometry && settings.localBoundsValid) {
            for (size_t corner = 0; corner < 8; ++corner) {
                Coordinate point;
                for (size_t k = 0; k < 3; ++k) {
                    point[k] = corner & (size_t{1} << k) ? settings.localBounds.max[k] : settings.localBounds.min[k];
                }
                includePoint(object, transformCoordinate(object.world.data(), point));
            }
        }
        object.end = objects.size();
    };
    const auto visit = [&](auto&& self, const UiSceneNode& node, size_t parent) -> void {
        if (node.kind == UiSceneNodeKind::group) { group(node.fileIndex, node.groupIndex, parent); return; }
        CoordinateMatrix local;
        size_t index;
        if (node.kind == UiSceneNodeKind::folder) {
            sceneNodeTransformMatrix(node.settings, local.data());
            index = append(node.objectId, parent, local, node.settings);
        } else {
            if (node.fileIndex >= state.files.size()) { return; }
            const auto& file = state.files[node.fileIndex];
            fileTransformMatrix(file.fileSettings, local.data());
            index = append(file.objectId, parent, local, file.fileSettings);
            if (node.children.empty()) {
                for (size_t g = 0; g < file.groupSettings.size(); ++g) { group(node.fileIndex, g, index); }
            }
        }
        for (const auto& child : node.children) { self(self, child, index); }
        objects[index].end = objects.size();
    };
    if (state.sceneNodes.empty()) {
        for (size_t f = 0; f < state.files.size(); ++f) {
            const auto& file = state.files[f];
            CoordinateMatrix local;
            fileTransformMatrix(file.fileSettings, local.data());
            const size_t index = append(file.objectId, noParent, local, file.fileSettings);
            for (size_t g = 0; g < file.groupSettings.size(); ++g) { group(f, g, index); }
            objects[index].end = objects.size();
        }
    } else {
        for (const auto& node : state.sceneNodes) { visit(visit, node, noParent); }
    }
    for (size_t i = objects.size(); i-- > 0;) {
        const auto& child = objects[i];
        if (child.parent != noParent && child.hasBounds) {
            includePoint(objects[child.parent], child.bounds[0]);
            includePoint(objects[child.parent], child.bounds[1]);
        }
    }
    return objects;
}

struct TransformSelection {
    std::vector<TransformObject> objects;
    std::vector<size_t> selected;
    size_t reference = noParent;
    const char* issue = nullptr;
};

TransformSelection transformSelection(const UiState& state, SceneObjectId reference)
{
    TransformSelection result;
    if (!scenePostLoadReady(state)) { result.issue = "Wait for model preparation to finish."; return result; }
    const boost::unordered_flat_set<SceneObjectId> selected(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end());
    if (selected.size() < 2) { result.issue = "Select at least two models, parts, or folders."; return result; }
    if (!reference || !selected.contains(reference)) { result.issue = "Include the reference model in the selection."; return result; }
    result.objects = transformObjects(state);
    boost::unordered_flat_map<SceneObjectId, size_t> occurrences;
    for (const auto& object : result.objects) { ++occurrences[object.id]; }
    for (size_t i = 0; i < result.objects.size(); ++i) {
        const auto& object = result.objects[i];
        if (!selected.contains(object.id)) { continue; }
        if (object.id == reference) { result.reference = i; }
        result.selected.push_back(i);
        for (size_t parent = object.parent; parent != noParent; parent = result.objects[parent].parent) {
            if (selected.contains(result.objects[parent].id)) {
                result.issue = "Select a parent or its parts, not both."; return result;
            }
        }
        for (size_t child = i; child < object.end; ++child) {
            const auto& entry = result.objects[child];
            if (!entry.id || occurrences[entry.id] != 1) {
                result.issue = "This selection contains a model with more than one tree occurrence."; return result;
            }
            if (!finiteCoordinate(entry.pivot) || !finitePosition(entry.translation)
                || !std::all_of(entry.local.begin(), entry.local.end(), [](double v) { return std::isfinite(v); })
                || !std::all_of(entry.world.begin(), entry.world.end(), [](double v) { return std::isfinite(v); })) {
                result.issue = "This selection contains an invalid transform."; return result;
            }
        }
    }
    if (result.selected.size() != selected.size() || result.reference == noParent) {
        result.issue = "Select models, parts, or folders only.";
    }
    return result;
}

struct TranslationEdit { SceneObjectId id; std::array<float, 3> value; };
struct TransformPlan { std::vector<TranslationEdit> edits; const char* issue = nullptr; };

bool localDisplacement(const CoordinateMatrix& parent, const Coordinate& world, Coordinate& local)
{
    const Coordinate a{parent[0],parent[1],parent[2]}, b{parent[4],parent[5],parent[6]}, c{parent[8],parent[9],parent[10]};
    const auto cross = [](const Coordinate& u, const Coordinate& v) {
        return Coordinate{u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
    };
    const auto dot = [](const Coordinate& u, const Coordinate& v) { return u[0]*v[0]+u[1]*v[1]+u[2]*v[2]; };
    const auto bc = cross(b,c);
    const double determinant = dot(a,bc);
    if (!std::isfinite(determinant) || determinant == 0) { return false; }
    local = {dot(world,bc)/determinant, dot(world,cross(c,a))/determinant, dot(world,cross(a,b))/determinant};
    return finiteCoordinate(local);
}

TransformPlan transformPlan(const TransformSelection& selection, UiSelectionTransform action)
{
    TransformPlan plan;
    if (selection.issue) { plan.issue = selection.issue; return plan; }
    if (action != UiSelectionTransform::alignOrigins && action != UiSelectionTransform::arrangeNearby) {
        plan.issue = "Unknown transform action."; return plan;
    }
    const auto& objects = selection.objects;
    const auto& reference = objects[selection.reference];
    double size = 0;
    for (const size_t index : selection.selected) {
        const auto& object = objects[index];
        if (action == UiSelectionTransform::arrangeNearby) {
            if (!object.hasBounds) { plan.issue = "Each selected object must have prepared geometry to arrange."; return plan; }
            for (size_t child = index; child < object.end; ++child) {
                if (objects[child].hasGeometry && !objects[child].hasBounds) {
                    plan.issue = "Each selected object must have prepared geometry to arrange."; return plan;
                }
            }
        }
        for (size_t k = 0; k < 3; ++k) {
            if (object.hasBounds) {
                size = std::max(size, object.bounds[1][k] - object.bounds[0][k]);
            }
        }
    }
    const double gap = size > 0 ? size * .05 : 1.0;
    double right = reference.hasBounds ? reference.bounds[1][0] : 0;
    for (const size_t index : selection.selected) {
        if (index == selection.reference) { continue; }
        const auto& object = objects[index];
        Coordinate delta;
        for (size_t k = 0; k < 3; ++k) {
            delta[k] = action == UiSelectionTransform::alignOrigins ? reference.pivot[k] - object.pivot[k]
                : k == 0 ? right + gap - object.bounds[0][k]
                : (reference.bounds[0][k] - object.bounds[0][k] + reference.bounds[1][k] - object.bounds[1][k]) * .5;
        }
        if (action == UiSelectionTransform::arrangeNearby) { right = object.bounds[1][0] + delta[0]; }
        double objectSize = 0, coordinate = 0, translationStep = 0;
        for (size_t k = 0; k < 3; ++k) {
            coordinate = std::max({coordinate, std::abs(object.pivot[k]), std::abs(reference.pivot[k]), std::abs(delta[k])});
            if (object.hasBounds) { objectSize = std::max(objectSize, object.bounds[1][k] - object.bounds[0][k]); }
        }
        CoordinateMatrix parent;
        coordinateIdentity(parent.data());
        if (object.parent != noParent) { parent = objects[object.parent].world; }
        for (size_t axis = 0; axis < 3; ++axis) {
            const float value = object.translation[axis];
            const float next = std::nextafter(value, std::numeric_limits<float>::infinity());
            const double step = std::isfinite(next) ? double(next) - value : double(value) * std::numeric_limits<float>::epsilon();
            translationStep += step * std::hypot(parent[axis*4],parent[axis*4+1],parent[axis*4+2]);
        }
        // A large peer must not turn a smaller object's move into a no-op. Base
        // repeat detection on stored translation precision, not the largest box.
        const double roundoff = coordinate * 64 * std::numeric_limits<double>::epsilon();
        const double unchanged = std::max(translationStep * 2, roundoff);
        const double tolerance = std::max(objectSize * 2e-6, roundoff);
        if (std::all_of(delta.begin(), delta.end(), [&](double v) { return std::abs(v) <= unchanged; })) { continue; }
        Coordinate local;
        if (!localDisplacement(parent, delta, local)) { plan.issue = "This selection contains an invalid parent transform."; return plan; }
        std::array<float, 3> value;
        Coordinate applied{};
        for (size_t k = 0; k < 3; ++k) {
            const double next = object.translation[k] + local[k];
            if (!std::isfinite(next) || std::abs(next) > std::numeric_limits<float>::max()) {
                plan.issue = "This move exceeds the supported translation range."; return plan;
            }
            value[k] = static_cast<float>(next);
            applied[k] = double(value[k]) - object.translation[k];
        }
        for (size_t k = 0; k < 3; ++k) {
            const double shift = parent[k]*applied[0]+parent[k+4]*applied[1]+parent[k+8]*applied[2];
            if (std::abs(shift - delta[k]) > tolerance) {
                plan.issue = "This move exceeds the supported translation precision."; return plan;
            }
        }
        // Descendants inherit the move; their local settings stay unchanged.
        if (value != object.translation) { plan.edits.push_back({object.id, value}); }
    }
    if (plan.edits.empty()) {
        plan.issue = action == UiSelectionTransform::alignOrigins
            ? "The selected origins already match." : "The selected objects are already arranged here.";
    }
    return plan;
}

} // namespace

UiSelectionTransformStatus selectedObjectTransformStatus(const UiState& state, SceneObjectId reference)
{
    const auto selection = transformSelection(state, reference);
    return {transformPlan(selection, UiSelectionTransform::alignOrigins).issue,
        transformPlan(selection, UiSelectionTransform::arrangeNearby).issue};
}

bool transformSelectedObjects(UiState& state, SceneObjectId reference, UiSelectionTransform action)
{
    const auto plan = transformPlan(transformSelection(state, reference), action);
    if (plan.issue) { return false; }
    boost::unordered_flat_map<SceneObjectId, std::array<float, 3>> values;
    for (const auto& edit : plan.edits) { values.emplace(edit.id, edit.value); }
    const auto apply = [&](SceneObjectId id, auto& settings) {
        if (const auto found = values.find(id); found != values.end()) { settings.translation = found->second; }
    };
    for (auto& file : state.files) {
        apply(file.objectId, file.fileSettings);
        for (auto& group : file.groupSettings) { apply(group.objectId, group); }
    }
    const auto folders = [&](auto&& self, std::vector<UiSceneNode>& nodes) -> void {
        for (auto& node : nodes) {
            if (node.kind == UiSceneNodeKind::folder) { apply(node.objectId, node.settings); }
            self(self, node.children);
        }
    };
    folders(folders, state.sceneNodes);
    recalculateSceneBounds(state);
    markSceneDirty(state, SceneChange::geometry);
    return true;
}

} // namespace woby
