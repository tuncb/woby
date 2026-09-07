#pragma once

#include "model_mesh.h"

#include <cstddef>
#include <stop_token>
#include <vector>

namespace woby
{

// Comparison buffers use 32-bit byte sizes and indices. Validate before
// allocating expanded geometry; these are representation limits, not a mesh cap.
[[nodiscard]] uint32_t comparisonBufferBytes(size_t count, size_t elementBytes);
void validateComparisonMeshSize(size_t vertexCount, size_t triangleCount);

struct DiagnosticEdge
{
    std::array<float, 3> a{}, b{};
};

struct MeshDiagnostics
{
    std::vector<DiagnosticEdge> boundaryEdges;
    std::vector<DiagnosticEdge> nonManifoldEdges;
    std::vector<DiagnosticEdge> inconsistentWindingEdges;
    size_t degenerateTriangles = 0;
    size_t duplicateTriangles = 0;
};

struct SurfaceComparison
{
    Mesh source;
    // Four subtriangles per source face, each colored by its centroid distance.
    // texcoord[0] holds the unsigned distance; the input mesh is never changed.
    Mesh sampled;
    std::vector<double> distances;
    std::vector<double> sampleAreas;
    double maximum = 0;
    double mean = 0;
    double percentile95 = 0;
    MeshDiagnostics diagnostics;
};

struct MeshComparison
{
    SurfaceComparison original;
    SurfaceComparison repaired;
};

[[nodiscard]] double pointTriangleDistance(const std::array<float, 3> &point, const std::array<float, 3> &a,
                                           const std::array<float, 3> &b, const std::array<float, 3> &c);
[[nodiscard]] MeshDiagnostics inspectMesh(const Mesh &mesh, std::stop_token stop = {});
// One empty mesh requests topology inspection only; no distance samples are produced.
[[nodiscard]] MeshComparison compareMeshes(const Mesh &original, const Mesh &repaired, std::stop_token stop = {});
[[nodiscard]] double surfacePercentAboveTolerance(const SurfaceComparison &surface, double tolerance);

} // namespace woby
