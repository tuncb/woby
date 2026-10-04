#pragma once

#include "model_mesh.h"

namespace woby {

inline constexpr uint32_t maxFreeformDegree = 8;
inline constexpr uint32_t freeformSegmentsPerSpan = 32;

// Immutable source data. Bezier input is represented as an equivalent clamped
// B-spline. Coordinates and parameter ranges retain the original file frame.
struct FreeformTrimming;

struct FreeformPatch {
    std::string name;
    bool surface = false;
    uint32_t degreeU = 0, degreeV = 0;
    uint32_t countU = 0, countV = 1;
    std::vector<std::array<double, 4>> controls; // Euclidean xyz and positive weight.
    std::vector<std::array<double, 2>> texcoords;
    std::vector<Coordinate> normals;
    std::vector<double> knotsU, knotsV;
    std::array<double, 2> domainU{}, domainV{};
    std::shared_ptr<const FreeformTrimming> trimming;
};

struct FreeformTrimSegment {
    std::shared_ptr<const FreeformPatch> curve; // UV stored as xy; no surface geometry.
    std::array<double, 2> interval{}; // May run backwards along the curve.
};
struct FreeformTrimLoop {
    std::vector<FreeformTrimSegment> segments;
    size_t sourceLine = 0;
};
struct FreeformTrimRegion {
    FreeformTrimLoop outer; // Empty means the surface parameter rectangle.
    std::vector<FreeformTrimLoop> holes;
};
struct FreeformTrimming {
    std::vector<FreeformTrimRegion> regions;
    std::string sourceFile;
};

struct FreeformSample {
    Coordinate position{}, du{}, dv{}, normal{};
    std::array<double, 2> texcoord{};
};

struct FreeformGrid {
    uint32_t vertexOffset = 0, indexOffset = 0, groupIndex = 0;
    std::vector<double> u, v;
    // Trimmed surfaces retain irregular parameter samples and local connectivity.
    // Untrimmed grids leave these empty and preserve their original ordering.
    std::vector<std::array<double, 2>> samples;
    std::vector<uint32_t> triangles;
};

struct FreeformGeometry {
    std::vector<FreeformPatch> patches;
    std::vector<FreeformGrid> grids;
};

struct FreeformBasis {
    size_t first = 0;
    std::array<double, maxFreeformDegree + 1> values{}, derivatives{};
};
[[nodiscard]] FreeformBasis freeformBasis(const std::vector<double>& knots, uint32_t count, uint32_t degree, double t);
[[nodiscard]] inline size_t freeformVertexCount(const FreeformGrid& grid) {
    return grid.samples.empty() ? grid.u.size()*grid.v.size() : grid.samples.size();
}
void validateFreeformPatch(const FreeformPatch& patch);
[[nodiscard]] FreeformSample evaluateFreeform(const FreeformPatch& patch, double u, double v = 0);
[[nodiscard]] std::array<float, 3> freeformNormal(const FreeformPatch& patch, double u, double v);
// Appends deterministic, camera-independent geometry and parameter provenance.
// Does not change existing vertices, primitive ranges, or authored face normals.
void appendFreeformGeometry(Mesh& mesh, std::vector<FreeformPatch> patches,
    const ModelLoadProgressCallback& progress = {});

} // namespace woby
