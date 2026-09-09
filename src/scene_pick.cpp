#include "scene_pick.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace woby {
namespace {
using Point = std::array<double, 3>;
using Clip = std::array<double, 4>;

PickMatrix identity()
{
    PickMatrix result;
    bx::mtxIdentity(result.data());
    return result;
}
PickMatrix compose(const PickMatrix& parent, const PickMatrix& child)
{
    PickMatrix result;
    // Keep the same multiplication order as submitSceneNode/submitGroupRange.
    bx::mtxMul(result.data(), parent.data(), child.data());
    return result;
}
Clip transform(const PickMatrix& m, const Clip& p)
{
    Clip result{};
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) { result[row] += m[column * 4 + row] * p[column]; }
    }
    return result;
}
Point position(const PickMatrix& m, const Point& p)
{
    const auto q = transform(m, {p[0], p[1], p[2], 1});
    return {q[0] / q[3], q[1] / q[3], q[2] / q[3]};
}
Point subtract(const Point& a, const Point& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Point cross(const Point& a, const Point& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Point& a, const Point& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool selected(const UiState& state, SceneObjectId id)
{
    return std::find(state.selectedSceneObjects.begin(), state.selectedSceneObjects.end(), id) != state.selectedSceneObjects.end();
}

void appendGroup(std::vector<ScenePickPart>& parts, const UiState& state, size_t fileIndex,
    size_t groupIndex, const PickMatrix& parent, float opacity, bool parentSelected)
{
    if (fileIndex >= state.files.size()) { return; }
    const auto& file = state.files[fileIndex];
    if (groupIndex >= file.mesh.nodes.size() || groupIndex >= file.groupSettings.size()) { return; }
    const auto& group = file.groupSettings[groupIndex];
    opacity *= group.opacity;
    if (!group.visible || opacity <= 0.0f || (!group.showSolidMesh && !group.showTriangles && !group.showVertices)) { return; }
    const auto& node = file.mesh.nodes[groupIndex];
    PickMatrix local;
    groupTransformMatrix(group, local.data());
    ScenePickPart part;
    part.objectId = group.objectId;
    part.mesh = &file.mesh;
    part.indexOffset = node.indexOffset;
    part.indexCount = node.indexCount;
    part.model = compose(parent, local);
    if (group.localBoundsValid) { part.bounds = group.localBounds; }
    part.solid = group.showSolidMesh;
    part.edges = group.showTriangles;
    part.vertices = group.showVertices;
    part.opacity = opacity;
    part.pointSize = std::round(std::clamp(state.masterVertexPointSize * file.vertexSizeScale * group.vertexSizeScale,
        minVertexPointSize, maxVertexPointSize));
    part.selected = parentSelected || selected(state, group.objectId);
    parts.push_back(part);
}

void appendNode(std::vector<ScenePickPart>& parts, const UiState& state, const UiSceneNode& node,
    const PickMatrix& parent, float opacity, bool parentSelected)
{
    const bool nodeSelected = parentSelected || selected(state, node.objectId);
    if (node.kind == UiSceneNodeKind::group) {
        appendGroup(parts, state, node.fileIndex, node.groupIndex, parent, opacity, nodeSelected);
        return;
    }
    PickMatrix local;
    if (node.kind == UiSceneNodeKind::folder) {
        if (!node.settings.visible) { return; }
        opacity *= node.settings.opacity;
        sceneNodeTransformMatrix(node.settings, local.data());
    } else {
        if (node.fileIndex >= state.files.size()) { return; }
        const auto& file = state.files[node.fileIndex];
        if (!file.fileSettings.visible) { return; }
        opacity *= file.fileSettings.opacity;
        fileTransformMatrix(file.fileSettings, local.data());
    }
    if (opacity <= 0.0f) { return; }
    const auto model = compose(parent, local);
    if (node.kind == UiSceneNodeKind::file && node.children.empty()) {
        for (size_t i = 0; i < state.files[node.fileIndex].groupSettings.size(); ++i) {
            appendGroup(parts, state, node.fileIndex, i, model, opacity, nodeSelected);
        }
    } else {
        for (const auto& child : node.children) { appendNode(parts, state, child, model, opacity, nodeSelected); }
    }
}

bool intersectsBounds(const Bounds& bounds, const Point& origin, const Point& direction)
{
    double near = 0, far = 1;
    for (size_t axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0) {
            if (origin[axis] < bounds.min[axis] || origin[axis] > bounds.max[axis]) { return false; }
        } else {
            double a = (bounds.min[axis] - origin[axis]) / direction[axis];
            double b = (bounds.max[axis] - origin[axis]) / direction[axis];
            if (a > b) { std::swap(a, b); }
            near = std::max(near, a);
            far = std::min(far, b);
            if (near > far) { return false; }
        }
    }
    return true;
}

std::optional<double> triangleHit(const Point& origin, const Point& direction, const std::array<Point, 3>& p)
{
    const auto a = subtract(p[1], p[0]), b = subtract(p[2], p[0]);
    const auto h = cross(direction, b);
    const double determinant = dot(a, h);
    const double scale = std::sqrt(dot(a, a) * dot(h, h));
    // Both faces are pickable: the scene renderer does not cull back faces.
    if (!std::isfinite(determinant) || std::abs(determinant) <= scale * 1e-12) { return {}; }
    const auto s = subtract(origin, p[0]);
    const double u = dot(s, h) / determinant;
    const auto q = cross(s, a);
    const double v = dot(direction, q) / determinant;
    const double t = dot(b, q) / determinant;
    if (u < -1e-9 || v < -1e-9 || u + v > 1.0 + 1e-9 || t < 0 || t > 1) { return {}; }
    return t;
}

double clipPlane(const Clip& p, size_t plane, bool homogeneous)
{
    switch (plane) {
    case 0: return p[3] + p[0];
    case 1: return p[3] - p[0];
    case 2: return p[3] + p[1];
    case 3: return p[3] - p[1];
    case 4: return homogeneous ? p[3] + p[2] : p[2];
    default: return p[3] - p[2];
    }
}
bool inside(const Clip& p, bool homogeneous)
{
    if (!std::all_of(p.begin(), p.end(), [](double value) { return std::isfinite(value); }) || p[3] <= 0) { return false; }
    for (size_t plane = 0; plane < 6; ++plane) { if (clipPlane(p, plane, homogeneous) < 0) { return false; } }
    return true;
}
Point screen(const Clip& p, const ScenePickView& view)
{
    return {(p[0] / p[3] * .5 + .5) * view.width, (.5 - p[1] / p[3] * .5) * view.height, p[2] / p[3]};
}
std::optional<double> edgeHit(Clip a, Clip b, const ScenePickView& view, PickPoint point)
{
    // Clip before the perspective divide, including edges crossing the near plane.
    for (size_t plane = 0; plane < 6; ++plane) {
        const double da = clipPlane(a, plane, view.homogeneousDepth), db = clipPlane(b, plane, view.homogeneousDepth);
        if (da < 0 && db < 0) { return {}; }
        if ((da < 0) != (db < 0)) {
            Clip cut;
            const double t = da / (da - db);
            for (size_t k = 0; k < 4; ++k) { cut[k] = a[k] + t * (b[k] - a[k]); }
            if (da < 0) { a = cut; } else { b = cut; }
        }
    }
    if (a[3] <= 0 || b[3] <= 0) { return {}; }
    const auto pa = screen(a, view), pb = screen(b, view);
    const double dx = pb[0] - pa[0], dy = pb[1] - pa[1];
    const double length = dx * dx + dy * dy;
    const double t = length > 0 ? std::clamp(((point[0] - pa[0]) * dx + (point[1] - pa[1]) * dy) / length, 0.0, 1.0) : 0;
    const double x = pa[0] + t * dx - point[0], y = pa[1] + t * dy - point[1];
    const double radius = 3.0 * view.pixelScale;
    if (x * x + y * y > radius * radius) { return {}; }
    const double depth = pa[2] + t * (pb[2] - pa[2]);
    return std::isfinite(depth) ? std::optional<double>(depth) : std::nullopt;
}
void closest(std::optional<double>& best, std::optional<double> value)
{
    if (value && (!best || *value < *best)) { best = value; }
}
} // namespace

void beginScenePointer(ScenePointerGesture& gesture, PickPoint point, bool alt, bool toggle)
{
    gesture = {true, false, alt, toggle, point};
}
bool moveScenePointer(ScenePointerGesture& gesture, PickPoint point)
{
    const float dx = point[0] - gesture.start[0], dy = point[1] - gesture.start[1];
    if (gesture.active && dx * dx + dy * dy >= 16.0f) { gesture.dragging = true; }
    return gesture.active && gesture.dragging;
}
std::optional<SceneClick> endScenePointer(ScenePointerGesture& gesture, PickPoint point, bool allowed)
{
    moveScenePointer(gesture, point);
    const bool click = gesture.active && !gesture.dragging && !gesture.alt && allowed;
    const bool toggle = gesture.toggle;
    gesture = {};
    return click ? std::optional<SceneClick>({point, toggle}) : std::nullopt;
}

ScenePickView scenePickView(const SceneCamera& camera, SceneUpAxis upAxis, const Bounds& bounds,
    uint32_t width, uint32_t height, bool homogeneousDepth, float pixelScale)
{
    ScenePickView result;
    result.width = std::max(width, 1u);
    result.height = std::max(height, 1u);
    result.homogeneousDepth = homogeneousDepth;
    result.pixelScale = pixelScale;
    const float aspect = static_cast<float>(result.width) / static_cast<float>(result.height);
    bx::mtxLookAt(result.view.data(), cameraEye(camera, upAxis), cameraLookAt(camera), cameraUp(camera, upAxis));
    bx::mtxProj(result.projection.data(), cameraViewportFov(camera, aspect), aspect,
        camera.nearPlane, cameraFarPlane(camera, bounds), homogeneousDepth);
    return result;
}

std::vector<ScenePickPart> scenePickParts(const UiState& state)
{
    std::vector<ScenePickPart> result;
    if (!state.sceneNodes.empty()) {
        for (const auto& node : state.sceneNodes) { appendNode(result, state, node, identity(), 1, false); }
    } else {
        for (size_t i = 0; i < state.files.size(); ++i) {
            const auto& file = state.files[i];
            if (!file.fileSettings.visible || file.fileSettings.opacity <= 0) { continue; }
            PickMatrix model;
            fileTransformMatrix(file.fileSettings, model.data());
            for (size_t j = 0; j < file.groupSettings.size(); ++j) {
                appendGroup(result, state, i, j, model, file.fileSettings.opacity, selected(state, file.objectId));
            }
        }
    }
    return result;
}

void appendComparisonPickParts(std::vector<ScenePickPart>& parts, const UiComparison& comparison,
    const ComparisonSettings& settings, const MeshComparison& result, bool isSelected)
{
    if (!comparison.settings.enabled) { return; }
    const bool original = settings.mode == ComparisonMode::original ||
        (settings.mode == ComparisonMode::distance && settings.distanceOnOriginal) ||
        (settings.mode == ComparisonMode::surfaceQuality && settings.quality.onOriginal);
    const auto& surface = original ? result.original : result.repaired;
    ScenePickPart part;
    part.objectId = comparison.objectId;
    // Sampled heatmap triangles have the same surface as their source mesh.
    part.mesh = &surface.source;
    part.indexCount = surface.source.indices.size();
    part.bounds = surface.source.bounds;
    bx::mtxTranslate(part.model.data(), comparison.translation[0], comparison.translation[1], comparison.translation[2]);
    part.solid = true;
    part.edges = settings.showEdges;
    part.edgeXray = false;
    part.surfaceLessEqual = true;
    part.selected = isSelected;
    parts.push_back(part);
    if (settings.mode == ComparisonMode::overlay) {
        auto overlay = part;
        overlay.mesh = &result.original.source;
        overlay.indexCount = result.original.source.indices.size();
        overlay.bounds = result.original.source.bounds;
        overlay.solid = false;
        overlay.edges = true;
        overlay.edgeXray = true;
        parts.push_back(overlay);
    }
    const auto appendEdges = [&](std::span<const DiagnosticEdge> edges) {
        if (edges.empty()) { return; }
        auto lines = part;
        lines.mesh = nullptr;
        lines.solid = false;
        lines.edges = false;
        lines.edgeXray = true;
        lines.selected = false;
        lines.diagnosticEdges = edges;
        parts.push_back(lines);
    };
    if (settings.showBoundaries) { appendEdges(surface.diagnostics.boundaryEdges); }
    if (settings.showNonManifold) {
        appendEdges(surface.diagnostics.nonManifoldEdges);
        appendEdges(surface.diagnostics.inconsistentWindingEdges);
    }
}

SceneObjectId pickSceneObject(std::span<const ScenePickPart> parts, const ScenePickView& view, PickPoint point)
{
    if (view.width == 0 || view.height == 0 || !std::isfinite(point[0]) || !std::isfinite(point[1]) ||
        point[0] < 0 || point[1] < 0 || point[0] >= view.width || point[1] >= view.height) { return invalidSceneObjectId; }
    PickMatrix inverse;
    // bx::mtxMul(a,b) composes column-vector transforms as b*a.
    const auto viewProjection = compose(view.view, view.projection);
    bx::mtxInverse(inverse.data(), viewProjection.data());
    const double x = 2.0 * point[0] / view.width - 1.0, y = 1.0 - 2.0 * point[1] / view.height;
    const auto near = position(inverse, {x, y, view.homogeneousDepth ? -1.0 : 0.0});
    const auto far = position(inverse, {x, y, 1.0});
    double depthBuffer = 1.0;
    SceneObjectId hit = invalidSceneObjectId;
    for (const auto& part : parts) {
        if (part.objectId == invalidSceneObjectId || part.opacity <= 0) { continue; }
        const auto mvp = compose(part.model, viewProjection);
        bx::mtxInverse(inverse.data(), part.model.data());
        const auto origin = position(inverse, near), end = position(inverse, far);
        const auto direction = subtract(end, origin);
        const bool testSurface = part.solid && (!part.bounds || intersectsBounds(*part.bounds, origin, direction));
        if (!testSurface && !part.edges && !part.vertices && part.diagnosticEdges.empty()) { continue; }
        std::optional<double> surfaceDepth, edgeDepth, pointDepth;
        const auto clip = [&](const Point& p) { return transform(mvp, {p[0], p[1], p[2], 1}); };
        if (part.mesh) {
            const auto& mesh = *part.mesh;
            const size_t begin = std::min(part.indexOffset, mesh.indices.size());
            const size_t finish = begin + std::min(part.indexCount, mesh.indices.size() - begin);
            for (size_t i = begin; i + 2 < finish; i += 3) {
                std::array<Point, 3> triangle;
                bool valid = true;
                for (size_t k = 0; k < 3; ++k) {
                    const auto index = mesh.indices[i + k];
                    if (index >= mesh.vertices.size() || !finitePosition(mesh.vertices[index].position)) { valid = false; break; }
                    const auto& p = mesh.vertices[index].position;
                    triangle[k] = {p[0], p[1], p[2]};
                }
                if (!valid) { continue; }
                if (testSurface) {
                    if (const auto t = triangleHit(origin, direction, triangle)) {
                        const auto p = clip({origin[0] + *t * direction[0], origin[1] + *t * direction[1], origin[2] + *t * direction[2]});
                        if (p[3] > 0 && std::isfinite(p[2] / p[3])) { closest(surfaceDepth, p[2] / p[3]); }
                    }
                }
                if (part.edges || part.vertices) {
                    const std::array<Clip, 3> projected = {clip(triangle[0]), clip(triangle[1]), clip(triangle[2])};
                    for (size_t k = 0; k < 3; ++k) {
                        if (part.edges) { closest(edgeDepth, edgeHit(projected[k], projected[(k + 1) % 3], view, point)); }
                        if (part.vertices && inside(projected[k], view.homogeneousDepth)) {
                            const auto p = screen(projected[k], view);
                            const double dx = p[0] - point[0], dy = p[1] - point[1];
                            const double radius = std::max(part.pointSize * .5, 3.0 * view.pixelScale);
                            if (dx * dx + dy * dy <= radius * radius) { closest(pointDepth, p[2]); }
                        }
                    }
                }
            }
        }
        for (const auto& line : part.diagnosticEdges) {
            closest(edgeDepth, edgeHit(clip({line.a[0], line.a[1], line.a[2]}),
                clip({line.b[0], line.b[1], line.b[2]}), view, point));
        }
        if (surfaceDepth && (part.surfaceLessEqual ? *surfaceDepth <= depthBuffer : *surfaceDepth < depthBuffer)) {
            hit = part.objectId;
            if (part.opacity >= .999f) { depthBuffer = *surfaceDepth; }
        }
        if (edgeDepth && (part.edgeXray || *edgeDepth <= depthBuffer + 1e-7)) { hit = part.objectId; }
        if (pointDepth && *pointDepth <= depthBuffer + 1e-7) {
            hit = part.objectId;
            if (part.opacity >= .999f) { depthBuffer = *pointDepth; }
        }
    }
    return hit;
}

std::vector<SceneObjectId> sceneSelectionPath(const UiState& state, SceneObjectId id)
{
    std::vector<SceneObjectId> path;
    if (id == invalidSceneObjectId) { return path; }
    const auto visit = [&](const auto& self, const UiSceneNode& node) -> bool {
        path.push_back(node.objectId);
        if (node.objectId == id) { return true; }
        for (const auto& child : node.children) { if (self(self, child)) { return true; } }
        path.pop_back();
        return false;
    };
    for (const auto& node : state.sceneNodes) { if (visit(visit, node)) { break; } }
    return path;
}

std::vector<std::array<float, 3>> sceneSelectionLines(std::span<const ScenePickPart> parts)
{
    std::vector<std::array<float, 3>> lines;
    for (const auto& part : parts) {
        if (!part.selected || !part.mesh || part.indexCount == 0) { continue; }
        auto bounds = part.bounds;
        if (!bounds) {
            const auto& mesh = *part.mesh;
            const size_t begin = std::min(part.indexOffset, mesh.indices.size());
            const size_t end = begin + std::min(part.indexCount, mesh.indices.size() - begin);
            for (size_t i = begin; i < end; ++i) {
                const auto index = mesh.indices[i];
                if (index >= mesh.vertices.size() || !finitePosition(mesh.vertices[index].position)) { continue; }
                const auto& p = mesh.vertices[index].position;
                if (!bounds) { bounds = Bounds{p, p, p, 0}; }
                for (size_t k = 0; k < 3; ++k) { bounds->min[k] = std::min(bounds->min[k], p[k]); bounds->max[k] = std::max(bounds->max[k], p[k]); }
            }
        }
        if (!bounds) { continue; }
        std::array<std::array<float, 3>, 8> corners;
        bool valid = true;
        for (size_t i = 0; i < corners.size(); ++i) {
            Point local;
            for (size_t k = 0; k < 3; ++k) { local[k] = (i & (size_t{1} << k)) ? bounds->max[k] : bounds->min[k]; }
            const auto p = position(part.model, local);
            for (size_t k = 0; k < 3; ++k) { corners[i][k] = static_cast<float>(p[k]); }
            valid = valid && finitePosition(corners[i]);
        }
        if (!valid) { continue; }
        for (size_t i = 0; i < corners.size(); ++i) {
            for (size_t k = 0; k < 3; ++k) {
                const size_t other = i ^ (size_t{1} << k);
                if (i < other) { lines.push_back(corners[i]); lines.push_back(corners[other]); }
            }
        }
    }
    return lines;
}
} // namespace woby
