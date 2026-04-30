#include "ui_state.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <bx/math.h>

namespace woby {
namespace {

float finiteOr(float value, float fallback)
{
    return std::isfinite(value) ? value : fallback;
}

float clampFinite(float value, float minValue, float maxValue, float fallback)
{
    return std::clamp(finiteOr(value, fallback), minValue, maxValue);
}

std::array<float, 3> finiteArrayOrZero(const std::array<float, 3>& value)
{
    return {
        finiteOr(value[0], 0.0f),
        finiteOr(value[1], 0.0f),
        finiteOr(value[2], 0.0f),
    };
}

std::array<float, 3> clampFiniteArray(
    const std::array<float, 3>& value,
    float minValue,
    float maxValue)
{
    return {
        clampFinite(value[0], minValue, maxValue, 0.0f),
        clampFinite(value[1], minValue, maxValue, 0.0f),
        clampFinite(value[2], minValue, maxValue, 0.0f),
    };
}

std::array<float, 4> clampColor(const std::array<float, 4>& value)
{
    return {
        clampFinite(value[0], 0.0f, 1.0f, 1.0f),
        clampFinite(value[1], 0.0f, 1.0f, 1.0f),
        clampFinite(value[2], 0.0f, 1.0f, 1.0f),
        clampFinite(value[3], 0.0f, 1.0f, 1.0f),
    };
}

std::array<float, 3> transformPoint(const float* matrix, const std::array<float, 3>& position)
{
    const float x = matrix[0] * position[0] + matrix[4] * position[1] + matrix[8] * position[2] + matrix[12];
    const float y = matrix[1] * position[0] + matrix[5] * position[1] + matrix[9] * position[2] + matrix[13];
    const float z = matrix[2] * position[0] + matrix[6] * position[1] + matrix[10] * position[2] + matrix[14];
    const float w = matrix[3] * position[0] + matrix[7] * position[1] + matrix[11] * position[2] + matrix[15];
    if (std::abs(w) <= 0.000001f) {
        return {x, y, z};
    }

    return {x / w, y / w, z / w};
}

void transformFromSettings(
    const std::array<float, 3>& center,
    const std::array<float, 3>& translation,
    const std::array<float, 3>& rotationDegrees,
    float scale,
    float* model)
{
    float toOrigin[16];
    float transformed[16];
    bx::mtxTranslate(toOrigin, -center[0], -center[1], -center[2]);
    bx::mtxSRT(
        transformed,
        scale,
        scale,
        scale,
        bx::toRad(rotationDegrees[0]),
        bx::toRad(rotationDegrees[1]),
        bx::toRad(rotationDegrees[2]),
        center[0] + translation[0],
        center[1] + translation[1],
        center[2] + translation[2]);
    bx::mtxMul(model, transformed, toOrigin);
}

bool boundsContainFinitePoints(const Bounds& bounds)
{
    return std::isfinite(bounds.min[0])
        && std::isfinite(bounds.min[1])
        && std::isfinite(bounds.min[2])
        && std::isfinite(bounds.max[0])
        && std::isfinite(bounds.max[1])
        && std::isfinite(bounds.max[2])
        && bounds.min[0] <= bounds.max[0]
        && bounds.min[1] <= bounds.max[1]
        && bounds.min[2] <= bounds.max[2];
}

Bounds emptyAccumulatedBounds()
{
    Bounds bounds;
    bounds.min = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    bounds.max = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };
    bounds.center = {};
    bounds.radius = 0.0f;
    return bounds;
}

void expandBounds(Bounds& bounds, const std::array<float, 3>& position)
{
    for (size_t axis = 0; axis < 3u; ++axis) {
        bounds.min[axis] = std::min(bounds.min[axis], position[axis]);
        bounds.max[axis] = std::max(bounds.max[axis], position[axis]);
    }
}

void finalizeBounds(Bounds& bounds)
{
    for (size_t axis = 0; axis < 3u; ++axis) {
        bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }

    float radiusSquared = 0.0f;
    for (size_t corner = 0; corner < 8u; ++corner) {
        std::array<float, 3> position{};
        for (size_t axis = 0; axis < 3u; ++axis) {
            position[axis] = (corner & (size_t{1u} << axis)) != 0u
                ? bounds.max[axis]
                : bounds.min[axis];
        }

        const float x = position[0] - bounds.center[0];
        const float y = position[1] - bounds.center[1];
        const float z = position[2] - bounds.center[2];
        radiusSquared = std::max(radiusSquared, x * x + y * y + z * z);
    }

    bounds.radius = std::max(std::sqrt(radiusSquared), 0.001f);
}

Bounds nodeBounds(const Mesh& mesh, const MeshNode& node)
{
    Bounds bounds = emptyAccumulatedBounds();
    const uint32_t endIndex = std::min(
        node.indexOffset + node.indexCount,
        static_cast<uint32_t>(mesh.indices.size()));
    for (uint32_t index = node.indexOffset; index < endIndex; ++index) {
        const uint32_t vertexIndex = mesh.indices[index];
        if (vertexIndex < mesh.vertices.size()) {
            expandBounds(bounds, mesh.vertices[vertexIndex].position);
        }
    }

    if (!boundsContainFinitePoints(bounds)) {
        return mesh.bounds;
    }

    finalizeBounds(bounds);
    return bounds;
}

void expandTransformedBounds(Bounds& bounds, const Bounds& localBounds, const float* model)
{
    for (size_t corner = 0; corner < 8u; ++corner) {
        std::array<float, 3> position{};
        for (size_t axis = 0; axis < 3u; ++axis) {
            position[axis] = (corner & (size_t{1u} << axis)) != 0u
                ? localBounds.max[axis]
                : localBounds.min[axis];
        }
        expandBounds(bounds, transformPoint(model, position));
    }
}

} // namespace

std::array<float, 4> defaultGroupColor(size_t groupIndex)
{
    constexpr std::array<std::array<float, 4>, 12> palette = {{
        {0.90f, 0.22f, 0.24f, 1.0f},
        {0.13f, 0.58f, 0.95f, 1.0f},
        {0.20f, 0.72f, 0.38f, 1.0f},
        {1.00f, 0.70f, 0.18f, 1.0f},
        {0.67f, 0.42f, 0.95f, 1.0f},
        {0.06f, 0.76f, 0.78f, 1.0f},
        {0.96f, 0.36f, 0.67f, 1.0f},
        {0.58f, 0.72f, 0.16f, 1.0f},
        {0.98f, 0.48f, 0.16f, 1.0f},
        {0.40f, 0.55f, 1.00f, 1.0f},
        {0.74f, 0.62f, 0.34f, 1.0f},
        {0.72f, 0.78f, 0.86f, 1.0f},
    }};

    return palette[groupIndex % palette.size()];
}

std::array<float, 3> nodeCenter(const Mesh& mesh, const MeshNode& node)
{
    if (node.indexCount == 0u || mesh.indices.empty() || mesh.vertices.empty()) {
        return mesh.bounds.center;
    }

    std::array<float, 3> minPosition = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> maxPosition = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    const uint32_t endIndex = node.indexOffset + node.indexCount;
    for (uint32_t index = node.indexOffset; index < endIndex; ++index) {
        const auto& position = mesh.vertices[mesh.indices[index]].position;
        for (size_t axis = 0; axis < 3u; ++axis) {
            minPosition[axis] = std::min(minPosition[axis], position[axis]);
            maxPosition[axis] = std::max(maxPosition[axis], position[axis]);
        }
    }

    return {
        (minPosition[0] + maxPosition[0]) * 0.5f,
        (minPosition[1] + maxPosition[1]) * 0.5f,
        (minPosition[2] + maxPosition[2]) * 0.5f,
    };
}

std::vector<UiGroupState> createUiGroupStates(const Mesh& mesh, size_t firstColorIndex)
{
    std::vector<UiGroupState> settings;
    settings.reserve(mesh.nodes.size());

    for (size_t groupIndex = 0; groupIndex < mesh.nodes.size(); ++groupIndex) {
        UiGroupState group;
        group.color = defaultGroupColor(firstColorIndex + groupIndex);
        group.localBounds = nodeBounds(mesh, mesh.nodes[groupIndex]);
        group.localBoundsValid = true;
        group.center = nodeCenter(mesh, mesh.nodes[groupIndex]);
        settings.push_back(group);
    }

    return settings;
}

UiFileState createUiFileState(std::filesystem::path modelPath, Mesh mesh, size_t firstColorIndex)
{
    UiFileState file;
    file.path = std::move(modelPath);
    file.mesh = std::move(mesh);
    file.groupSettings = createUiGroupStates(file.mesh, firstColorIndex);
    file.fileSettings.center = file.mesh.bounds.center;
    return file;
}

void groupTransformMatrix(const UiGroupState& settings, float* model)
{
    transformFromSettings(
        settings.center,
        settings.translation,
        settings.rotationDegrees,
        settings.scale,
        model);
}

void fileTransformMatrix(const UiFileSettings& settings, float* model)
{
    transformFromSettings(
        settings.center,
        settings.translation,
        settings.rotationDegrees,
        settings.scale,
        model);
}

Bounds defaultDisplayBounds()
{
    Bounds bounds;
    bounds.min = {defaultDisplayBoundsMin, defaultDisplayBoundsMin, defaultDisplayBoundsMin};
    bounds.max = {defaultDisplayBoundsMax, defaultDisplayBoundsMax, defaultDisplayBoundsMax};
    finalizeBounds(bounds);
    return bounds;
}

Bounds combineBounds(const std::vector<UiFileState>& files)
{
    Bounds bounds = emptyAccumulatedBounds();

    for (const auto& file : files) {
        float fileModel[16];
        fileTransformMatrix(file.fileSettings, fileModel);
        if (file.groupSettings.empty()) {
            expandTransformedBounds(bounds, file.mesh.bounds, fileModel);
            continue;
        }

        const size_t groupCount = std::min(file.groupSettings.size(), file.mesh.nodes.size());
        for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
            float groupModel[16];
            float model[16];
            groupTransformMatrix(file.groupSettings[groupIndex], groupModel);
            bx::mtxMul(model, fileModel, groupModel);
            const Bounds localBounds = file.groupSettings[groupIndex].localBoundsValid
                ? file.groupSettings[groupIndex].localBounds
                : nodeBounds(file.mesh, file.mesh.nodes[groupIndex]);
            expandTransformedBounds(bounds, localBounds, model);
        }
    }

    if (!boundsContainFinitePoints(bounds)) {
        return defaultDisplayBounds();
    }

    finalizeBounds(bounds);
    return bounds;
}

SceneFileSettings sceneFileSettings(const UiFileSettings& settings)
{
    SceneFileSettings result;
    result.visible = settings.visible;
    result.scale = settings.scale;
    result.opacity = settings.opacity;
    result.translation = settings.translation;
    result.rotationDegrees = settings.rotationDegrees;
    return result;
}

SceneGroupSettings sceneGroupSettings(const UiGroupState& settings)
{
    SceneGroupSettings result;
    result.visible = settings.visible;
    result.showSolidMesh = settings.showSolidMesh;
    result.showTriangles = settings.showTriangles;
    result.showVertices = settings.showVertices;
    result.scale = settings.scale;
    result.opacity = settings.opacity;
    result.vertexSizeScale = settings.vertexSizeScale;
    result.translation = settings.translation;
    result.rotationDegrees = settings.rotationDegrees;
    result.color = settings.color;
    return result;
}

SceneDocument createSceneDocument(const UiState& state)
{
    SceneDocument document;
    document.masterVertexPointSize = state.masterVertexPointSize;
    document.showOrigin = state.showOrigin;
    document.showGrid = state.showGrid;
    document.upAxis = state.upAxis;
    document.files.reserve(state.files.size());

    for (const auto& file : state.files) {
        SceneFileRecord fileRecord;
        fileRecord.path = file.path;
        fileRecord.settings = sceneFileSettings(file.fileSettings);
        fileRecord.vertexSizeScale = file.vertexSizeScale;
        fileRecord.groups.reserve(file.groupSettings.size());

        for (size_t groupIndex = 0; groupIndex < file.groupSettings.size(); ++groupIndex) {
            SceneGroupRecord groupRecord;
            groupRecord.name = file.mesh.nodes[groupIndex].name;
            groupRecord.settings = sceneGroupSettings(file.groupSettings[groupIndex]);
            fileRecord.groups.push_back(std::move(groupRecord));
        }

        document.files.push_back(std::move(fileRecord));
    }

    return document;
}

void applySceneFileRecord(UiFileState& file, const SceneFileRecord& record)
{
    file.fileSettings.visible = record.settings.visible;
    file.fileSettings.scale = clampFinite(record.settings.scale, minGroupScale, maxGroupScale, 1.0f);
    file.fileSettings.opacity = clampFinite(record.settings.opacity, minGroupOpacity, maxGroupOpacity, 1.0f);
    file.fileSettings.translation = finiteArrayOrZero(record.settings.translation);
    file.fileSettings.rotationDegrees = clampFiniteArray(
        record.settings.rotationDegrees,
        minRotationDegrees,
        maxRotationDegrees);
    file.vertexSizeScale = clampFinite(record.vertexSizeScale, minVertexSizeScale, maxVertexSizeScale, 1.0f);

    const size_t groupCount = std::min(file.groupSettings.size(), record.groups.size());
    for (size_t groupIndex = 0; groupIndex < groupCount; ++groupIndex) {
        auto& group = file.groupSettings[groupIndex];
        const auto& recordGroup = record.groups[groupIndex].settings;
        group.visible = recordGroup.visible;
        group.showSolidMesh = recordGroup.showSolidMesh;
        group.showTriangles = recordGroup.showTriangles;
        group.showVertices = recordGroup.showVertices;
        group.scale = clampFinite(recordGroup.scale, minGroupScale, maxGroupScale, 1.0f);
        group.opacity = clampFinite(recordGroup.opacity, minGroupOpacity, maxGroupOpacity, 1.0f);
        group.vertexSizeScale = clampFinite(
            recordGroup.vertexSizeScale,
            minVertexSizeScale,
            maxVertexSizeScale,
            1.0f);
        group.translation = finiteArrayOrZero(recordGroup.translation);
        group.rotationDegrees = clampFiniteArray(
            recordGroup.rotationDegrees,
            minRotationDegrees,
            maxRotationDegrees);
        group.color = clampColor(recordGroup.color);
    }

    if (!record.settings.visible) {
        for (auto& group : file.groupSettings) {
            group.visible = false;
        }
        file.fileSettings.visible = false;
    } else if (!file.groupSettings.empty()) {
        file.fileSettings.visible = std::any_of(
            file.groupSettings.begin(),
            file.groupSettings.end(),
            [](const UiGroupState& group) {
                return group.visible;
            });
    }
}

} // namespace woby
