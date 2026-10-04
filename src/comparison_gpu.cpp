#include "comparison_gpu.h"
#include "uv_quality_view.h"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace woby {
void destroySurface(ComparisonGpuSurface &gpu)
{
    for (const auto handle : {gpu.vertices, gpu.samples, gpu.quality, gpu.boundaries, gpu.nonManifold, gpu.winding,
        gpu.nonManifoldVertices, gpu.holes, gpu.finEdges, gpu.finFill, gpu.duplicatePoints, gpu.duplicateTriangleEdges, gpu.duplicateTriangleFill, gpu.degenerateEdges, gpu.degenerateFill, gpu.intersectionEdges, gpu.intersectionFill})
    {
        if (woby::graphics::isValid(handle))
        {
            woby::graphics::destroy(handle);
        }
    }
    for (const auto handle : {gpu.triangles, gpu.lines})
    {
        if (woby::graphics::isValid(handle))
        {
            woby::graphics::destroy(handle);
        }
    }
    gpu = {};
}
woby::graphics::VertexBufferHandle uploadEdges(const std::vector<DiagnosticEdge> &edges)
{
    if (edges.empty())
    {
        return WOBY_GPU_INVALID_HANDLE;
    }
    // DiagnosticEdge already has the packed endpoint layout consumed by the
    // renderer. Copy it directly instead of allocating another full line list.
    static_assert(sizeof(DiagnosticEdge) == 2 * sizeof(std::array<float, 3>));
    static_assert(offsetof(DiagnosticEdge, a) == 0);
    static_assert(offsetof(DiagnosticEdge, b) == sizeof(std::array<float, 3>));
    const auto bytes = comparisonBufferBytes(edges.size(), sizeof(DiagnosticEdge));
    const auto handle = woby::graphics::createVertexBuffer(
        woby::graphics::copy(edges.data(), bytes), helperLineVertexLayout());
    if (!woby::graphics::isValid(handle))
    {
        throw std::runtime_error("Cannot allocate analysis edge buffer.");
    }
    return handle;
}
woby::graphics::VertexBufferHandle uploadPositions(const std::vector<std::array<float, 3>>& positions)
{
    if (positions.empty()) { return WOBY_GPU_INVALID_HANDLE; }
    const auto handle = woby::graphics::createVertexBuffer(woby::graphics::copy(positions.data(), comparisonBufferBytes(positions.size(), sizeof(positions[0]))), helperLineVertexLayout());
    if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Cannot allocate duplicate overlay buffer."); }
    return handle;
}
void appendCross(std::vector<std::array<float, 3>>& lines, const std::array<float, 3>& point, float radius)
{
    for (size_t axis = 0; axis < 3; ++axis) {
        auto a = point, b = point; a[axis] -= radius; b[axis] += radius;
        lines.push_back(a); lines.push_back(b);
    }
}
void uploadDuplicateOverlays(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages)
{
    if (stages & comparisonDuplicatePoints) {
        std::vector<std::array<float, 3>> points;
        for (const auto& finding : surface.duplicates.points.findings) {
            for (const auto& point : finding.geometry) { appendCross(points, point, surface.source.bounds.radius*.008f); }
        }
        gpu.duplicatePoints = uploadPositions(points);
    }
    if (stages & comparisonDuplicateTriangles) {
        std::vector<std::array<float, 3>> edges, fill;
        for (const auto& finding : surface.duplicates.triangles.findings) {
            fill.insert(fill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t i = 0; i < finding.geometry.size(); i += 3) {
                for (size_t k = 0; k < 3; ++k) { edges.push_back(finding.geometry[i+k]); edges.push_back(finding.geometry[i+(k+1)%3]); }
            }
        }
        gpu.duplicateTriangleEdges = uploadPositions(edges);
        gpu.duplicateTriangleFill = uploadPositions(fill);
    }
}

void uploadSurface(ComparisonGpuSurface& gpu, const SurfaceComparison& surface, uint32_t stages,
    const PreparedComparisonSource* prepared)
{
    if (surface.source.indices.empty()) { return; }
    if (stages & comparisonSource) {
        const auto vertexBytes = comparisonBufferBytes(surface.source.vertices.size(), sizeof(Vertex));
        const auto indexBytes = comparisonBufferBytes(surface.source.indices.size(), sizeof(uint32_t));
        const auto lineBytes = comparisonBufferBytes(surface.source.indices.size(), 2 * sizeof(uint32_t));
        if (surface.source.uvQuality) {
            const auto qualityBytes = comparisonBufferBytes(surface.source.indices.size(),sizeof(Vertex));
            const auto values = prepared ? std::vector<Vertex>{} : uvQualityVertices(surface.source);
            gpu.quality = woby::graphics::createVertexBuffer(woby::graphics::copy(prepared ? prepared->quality.data() : values.data(), qualityBytes),meshVertexLayout());
            if (!woby::graphics::isValid(gpu.quality)) { throw std::runtime_error("Cannot allocate UV quality buffer."); }
        }
        auto vertices = prepared ? std::vector<Vertex>{} : surface.source.vertices;
        if (!prepared) { generateSmoothNormals(vertices, surface.source.indices); }
        gpu.vertices = woby::graphics::createVertexBuffer(woby::graphics::copy(prepared ? surface.source.vertices.data() : vertices.data(), vertexBytes), meshVertexLayout());
        const auto& indices = surface.source.indices;
        gpu.triangles = woby::graphics::createIndexBuffer(woby::graphics::copy(indices.data(), indexBytes), WOBY_GPU_BUFFER_INDEX32);
        std::vector<uint32_t> lines;
        if (!prepared) { lines.reserve(indices.size() * 2); }
        for (size_t i = 0; !prepared && i < indices.size(); i += 3) {
            for (size_t k = 0; k < 3; ++k) {
                lines.push_back(indices[i + k]);
                lines.push_back(indices[i + (k + 1) % 3]);
            }
        }
        gpu.lines = woby::graphics::createIndexBuffer(woby::graphics::copy(prepared ? prepared->lines.data() : lines.data(), lineBytes), WOBY_GPU_BUFFER_INDEX32);
        if (!woby::graphics::isValid(gpu.vertices) || !woby::graphics::isValid(gpu.triangles) || !woby::graphics::isValid(gpu.lines)) {
            throw std::runtime_error("Cannot allocate analysis surface buffers.");
        }
    }
    if ((stages & comparisonDistance) && !surface.sampled.vertices.empty()) {
        const auto& samples = surface.sampled.vertices;
        gpu.samples = woby::graphics::createVertexBuffer(woby::graphics::copy(samples.data(), comparisonBufferBytes(samples.size(), sizeof(Vertex))), meshVertexLayout());
        if (!woby::graphics::isValid(gpu.samples)) { throw std::runtime_error("Cannot allocate analysis distance buffer."); }
    }
    if (stages & comparisonTopology) {
        gpu.boundaries = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.boundaryEdges : surface.topologyBoundaries);
        gpu.nonManifold = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.nonManifoldEdges : surface.topologyNonManifold);
        gpu.nonManifoldVertices = uploadEdges(surface.nonManifoldVertexMarkers);
        gpu.holes = uploadEdges(surface.holeEdges);
        gpu.finEdges = uploadEdges(surface.finEdges);
        gpu.finFill = uploadPositions(surface.finFill);
        gpu.winding = uploadEdges(surface.topology.sources.empty() ? surface.diagnostics.inconsistentWindingEdges : surface.topologyWinding);
    }
    if (stages & comparisonDegenerates) {
        std::vector<std::array<float, 3>> edges, fill;
        for (const auto& finding : surface.degenerates.findings) {
            fill.insert(fill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t k = 0; k < 3; ++k) { edges.push_back(finding.geometry[k]); edges.push_back(finding.geometry[(k+1)%3]); }
            if (finding.reasons.collapsed) { appendCross(edges, finding.geometry[0], std::max(.00001f, surface.source.bounds.radius*.008f)); }
        }
        gpu.degenerateEdges = uploadPositions(edges);
        gpu.degenerateFill = uploadPositions(fill);
    }
    if (stages & comparisonIntersections) {
        std::vector<std::array<float, 3>> edges, fill;
        for (const auto& finding : surface.intersections.findings) {
            fill.insert(fill.end(), finding.geometry.begin(), finding.geometry.end());
            for (size_t i = 0; i < 6; i += 3) {
                for (size_t k = 0; k < 3; ++k) { edges.push_back(finding.geometry[i+k]); edges.push_back(finding.geometry[i+(k+1)%3]); }
            }
        }
        gpu.intersectionEdges = uploadPositions(edges);
        gpu.intersectionFill = uploadPositions(fill);
    }
    uploadDuplicateOverlays(gpu, surface, stages);
}
void uploadQuality(ComparisonGpuSurface& gpu, const SurfaceComparison& surface,
    SurfaceQualityMetric metric, const QualityDistribution& distribution)
{
    if (surface.source.indices.empty()) { return; }
    const auto bytes = comparisonBufferBytes(surface.source.indices.size(), sizeof(Vertex));
    const auto vertices = surfaceQualityVertices(surface.source, surface.quality, metric, distribution);
    const auto handle = woby::graphics::createVertexBuffer(woby::graphics::copy(vertices.data(), bytes), meshVertexLayout());
    if (!woby::graphics::isValid(handle)) { throw std::runtime_error("Cannot allocate surface mesh quality buffer."); }
    if (woby::graphics::isValid(gpu.quality)) { woby::graphics::destroy(gpu.quality); }
    gpu.quality = handle;
}
void destroyStage(ComparisonGpuSurface& gpu, uint32_t stages)
{
    const auto destroy = [](auto& handle) { if (woby::graphics::isValid(handle)) { woby::graphics::destroy(handle); } handle = WOBY_GPU_INVALID_HANDLE; };
    if (stages & comparisonSource) { destroy(gpu.vertices); destroy(gpu.triangles); destroy(gpu.lines); destroy(gpu.quality); }
    if (stages & comparisonTopology) { for (auto* h : {&gpu.boundaries,&gpu.nonManifold,&gpu.winding,&gpu.nonManifoldVertices,&gpu.holes,&gpu.finEdges,&gpu.finFill}) { destroy(*h); } }
    if (stages & comparisonDuplicatePoints) { destroy(gpu.duplicatePoints); }
    if (stages & comparisonDuplicateTriangles) { destroy(gpu.duplicateTriangleEdges); destroy(gpu.duplicateTriangleFill); }
    if (stages & comparisonDegenerates) { destroy(gpu.degenerateEdges); destroy(gpu.degenerateFill); }
    if (stages & comparisonIntersections) { destroy(gpu.intersectionEdges); destroy(gpu.intersectionFill); }
    if (stages & comparisonDistance) { destroy(gpu.samples); }
    if (stages & comparisonQuality) { destroy(gpu.quality); }
}

} // namespace woby
