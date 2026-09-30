#include "model_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace woby {
namespace {

std::array<float, 3> subtract(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    return {lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2]};
}

std::array<float, 3> cross(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    return {
        lhs[1] * rhs[2] - lhs[2] * rhs[1],
        lhs[2] * rhs[0] - lhs[0] * rhs[2],
        lhs[0] * rhs[1] - lhs[1] * rhs[0],
    };
}

void add(std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
{
    lhs[0] += rhs[0];
    lhs[1] += rhs[1];
    lhs[2] += rhs[2];
}

void normalize(std::array<float, 3>& value)
{
    const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
    if (length <= 0.000001f || !std::isfinite(length)) {
        value = {0.0f, 1.0f, 0.0f};
        return;
    }

    value[0] /= length;
    value[1] /= length;
    value[2] /= length;
}

bool hasCompleteNormals(const std::vector<Vertex>& vertices)
{
    return std::all_of(vertices.begin(), vertices.end(), [](const Vertex& vertex) {
        return validNormal(vertex.normal);
    });
}

} // namespace

void coordinateIdentity(double* result)
{
    std::fill_n(result, 16, 0.0);
    for (size_t i = 0; i < 4; ++i) { result[i*5] = 1.0; }
}
void coordinateMultiply(double* result, const double* a, const double* b)
{
    CoordinateMatrix product{};
    for (size_t col = 0; col < 4; ++col) {
        for (size_t row = 0; row < 4; ++row) {
            for (size_t k = 0; k < 4; ++k) { product[col*4+row] += a[col*4+k] * b[k*4+row]; }
        }
    }
    std::copy(product.begin(), product.end(), result);
}
Coordinate transformCoordinate(const double* m, const Coordinate& p)
{
    Coordinate result{};
    for (size_t k = 0; k < 3; ++k) { result[k] = m[k]*p[0] + m[k+4]*p[1] + m[k+8]*p[2] + m[k+12]; }
    return result;
}

bool finiteCoordinate(const Coordinate& point) noexcept
{
    return std::all_of(point.begin(), point.end(), [](double v) { return std::isfinite(v); });
}
Coordinate meshPosition(const Mesh& mesh, size_t index)
{
    if (!mesh.precisePositions.empty()) { return mesh.precisePositions.at(index); }
    const auto& p = mesh.vertices.at(index).position;
    return {p[0], p[1], p[2]};
}
std::array<Coordinate, 2> originalMeshBounds(const Mesh& mesh, const MeshNode* node)
{
    if (!node && mesh.originalBounds) { return *mesh.originalBounds; }
    const auto count = node ? size_t(node->indexCount) : mesh.vertices.size();
    std::array<Coordinate, 2> bounds{};
    for (size_t i = 0; i < count; ++i) {
        const auto p = originalPosition(meshPosition(mesh, node ? mesh.indices.at(size_t(node->indexOffset)+i) : i), mesh.origin);
        if (i == 0) { bounds = {p, p}; }
        else { for (size_t k = 0; k < 3; ++k) { bounds[0][k] = std::min(bounds[0][k], p[k]); bounds[1][k] = std::max(bounds[1][k], p[k]); } }
    }
    return bounds;
}
std::array<float, 3> renderPosition(const Coordinate& point)
{
    std::array<float, 3> result{};
    for (size_t k = 0; k < 3; ++k) {
        if (!std::isfinite(point[k]) || std::abs(point[k]) > std::numeric_limits<float>::max()) {
            throw std::invalid_argument("Position exceeds the supported display range.");
        }
        result[k] = static_cast<float>(point[k]);
    }
    return result;
}
Coordinate relativePosition(const Coordinate& point, const Coordinate& origin)
{
    return {point[0] - origin[0], point[1] - origin[1], point[2] - origin[2]};
}
Coordinate originalPosition(const Coordinate& point, const Coordinate& origin)
{
    return {point[0] + origin[0], point[1] + origin[1], point[2] + origin[2]};
}
Coordinate coordinateOrigin(const std::vector<Coordinate>& points)
{
    if (points.empty()) { return {}; }
    auto low = points.front(), high = low;
    for (const auto& p : points) {
        if (!finiteCoordinate(p)) { throw std::invalid_argument("Non-finite source coordinate."); }
        for (size_t k = 0; k < 3; ++k) { low[k] = std::min(low[k], p[k]); high[k] = std::max(high[k], p[k]); }
    }
    Coordinate origin{};
    // Small coordinates already have a useful working frame. Keep their existing
    // origin, including models straddling zero. Rebase large absolute offsets.
    for (size_t k = 0; k < 3; ++k) {
        const double center = low[k] * .5 + high[k] * .5;
        if (std::abs(center) >= 65536.0) { origin[k] = center; }
    }
    return origin;
}
void localizeMesh(Mesh& mesh)
{
    if (mesh.precisePositions.size() != mesh.vertices.size()) {
        throw std::invalid_argument("Source and render position counts differ.");
    }
    mesh.origin = coordinateOrigin(mesh.precisePositions);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& p = mesh.precisePositions[i];
        p = relativePosition(p, mesh.origin);
        mesh.vertices[i].position = renderPosition(p);
    }
}

void rebaseMesh(Mesh& mesh, const Coordinate& origin)
{
    if (!finiteCoordinate(origin)) { throw std::invalid_argument("Non-finite mesh origin."); }
    if (mesh.origin == origin) { return; }
    const auto delta = relativePosition(mesh.origin, origin);
    if (mesh.precisePositions.empty()) {
        mesh.precisePositions.reserve(mesh.vertices.size());
        for (const auto& vertex : mesh.vertices) { mesh.precisePositions.push_back({vertex.position[0], vertex.position[1], vertex.position[2]}); }
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        auto& p = mesh.precisePositions.at(i);
        p = originalPosition(p, delta);
        mesh.vertices[i].position = renderPosition(p);
    }
    if (mesh.sourceData) {
        auto source = std::make_shared<SourceMeshData>(*mesh.sourceData);
        for (auto& p : source->points) { p = originalPosition(p, delta); }
        mesh.sourceData = std::move(source);
    }
    if (mesh.duplicateInput) {
        auto input = std::make_shared<DuplicateInput>(*mesh.duplicateInput);
        for (auto& source : input->sources) {
            for (size_t k = 0; k < 3; ++k) { source.unusedPointTransform[12+k] += delta[k]; }
            for (auto& part : source.parts) { for (size_t k = 0; k < 3; ++k) { part.transform[12+k] += delta[k]; } }
        }
        mesh.duplicateInput = std::move(input);
    }
    mesh.origin = origin;
    mesh.annotationCache.reset();
    if (!mesh.vertices.empty()) { mesh.bounds = calculateBounds(mesh.vertices); }
}

bool empty(const Mesh& mesh) noexcept
{
    return mesh.vertices.empty() || mesh.indices.empty();
}

bool finitePosition(const std::array<float, 3>& position) noexcept
{
    return std::isfinite(position[0])
        && std::isfinite(position[1])
        && std::isfinite(position[2]);
}

bool validNormal(const std::array<float, 3>& normal) noexcept
{
    if (!std::isfinite(normal[0]) || !std::isfinite(normal[1]) || !std::isfinite(normal[2])) {
        return false;
    }

    return std::abs(normal[0]) > 0.000001f
        || std::abs(normal[1]) > 0.000001f
        || std::abs(normal[2]) > 0.000001f;
}

std::array<float, 3> calculateFaceNormal(
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c)
{
    auto normal = cross(subtract(b, a), subtract(c, a));
    normalize(normal);
    return normal;
}

void generateSmoothNormals(std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices,
    const ModelLoadProgressCallback& progress)
{
    const auto total = vertices.size() * 2 + indices.size();
    size_t completed = 0;
    for (auto& vertex : vertices) {
        if (completed % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::normals, completed, total); }
        vertex.normal = {0.0f, 0.0f, 0.0f};
        ++completed;
    }

    for (size_t index = 0; index + 2 < indices.size(); index += 3u) {
        if (index % 49152 == 0) { reportModelLoadProgress(progress, ModelLoadStage::normals, completed + index, total); }
        Vertex& a = vertices[indices[index + 0u]];
        Vertex& b = vertices[indices[index + 1u]];
        Vertex& c = vertices[indices[index + 2u]];

        const auto faceNormal = calculateFaceNormal(a.position, b.position, c.position);
        add(a.normal, faceNormal);
        add(b.normal, faceNormal);
        add(c.normal, faceNormal);
    }

    completed += indices.size();
    for (auto& vertex : vertices) {
        if (completed % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::normals, completed, total); }
        normalize(vertex.normal);
        ++completed;
    }
    reportModelLoadProgress(progress, ModelLoadStage::normals, total, total);
}

Bounds calculateBounds(const std::vector<Vertex>& vertices, const ModelLoadProgressCallback& progress)
{
    if (vertices.empty()) {
        throw std::runtime_error("Cannot calculate bounds for an empty mesh.");
    }

    Bounds bounds{};
    bounds.min = vertices.front().position;
    bounds.max = vertices.front().position;

    size_t completed = 0;
    const auto total = vertices.size() * 2;
    for (const auto& vertex : vertices) {
        if (completed % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::bounds, completed, total); }
        ++completed;
        for (size_t axis = 0; axis < 3u; ++axis) {
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
    }

    for (size_t axis = 0; axis < 3u; ++axis) {
        bounds.center[axis] = (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }

    float radiusSquared = 0.0f;
    for (const auto& vertex : vertices) {
        if (completed % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::bounds, completed, total); }
        ++completed;
        const auto offset = subtract(vertex.position, bounds.center);
        radiusSquared = std::max(radiusSquared, offset[0] * offset[0] + offset[1] * offset[1] + offset[2] * offset[2]);
    }

    bounds.radius = std::max(std::sqrt(radiusSquared), 0.001f);
    reportModelLoadProgress(progress, ModelLoadStage::bounds, total, total);
    return bounds;
}

void captureSourceMesh(Mesh& mesh, SourceProvenance provenance)
{
    auto data = std::make_shared<SourceMeshData>();
    data->provenance = provenance;
    data->points.reserve(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) { data->points.push_back(meshPosition(mesh, i)); }
    data->indices = mesh.indices;
    mesh.sourceData = std::move(data);
}

void finalizeMesh(Mesh& mesh, bool generateMissingSmoothNormals, const ModelLoadProgressCallback& progress)
{
    if (empty(mesh)) {
        throw std::runtime_error("Mesh did not contain renderable triangles.");
    }

    reportModelLoadProgress(progress, ModelLoadStage::normals);
    if (generateMissingSmoothNormals && !hasCompleteNormals(mesh.vertices)) {
        generateSmoothNormals(mesh.vertices, mesh.indices, progress);
    }

    reportModelLoadProgress(progress, ModelLoadStage::bounds);
    mesh.bounds = calculateBounds(mesh.vertices, progress);
    mesh.originalBounds.reset();
    mesh.originalBounds = originalMeshBounds(mesh);
}

} // namespace woby
