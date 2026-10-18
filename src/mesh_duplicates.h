#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include <utility>

namespace woby {

enum class SourceProvenance { objPositions, importerVertices };

// Captured before render optimization. Triangle IDs refer to generated triangles,
// not original polygons. Positions retain double precision in the mesh local frame.
struct SourceMeshData {
    SourceProvenance provenance = SourceProvenance::importerVertices;
    std::vector<std::array<double, 3>> draftPositions;
    std::shared_ptr<const std::vector<std::array<double, 3>>> positions;
    std::vector<uint32_t> indices;
    bool indicesPending = false;
    // Optional importer identities, one per point. Only duplicate-point counting
    // uses these; connectivity and reported vertex indices remain unchanged.
    std::vector<uint64_t> originalPointIds;
    // Freeform samples follow authored positions. Compare duplicates within
    // each domain; a sample at an authored control is not a repeated point.
    std::optional<size_t> firstGeneratedPosition;
};

// Published coordinates have a separate owner so render snapshots need not retain
// source topology or importer metadata. Mutable access detaches the coordinates.
[[nodiscard]] inline const std::vector<std::array<double, 3>>& sourcePositions(const SourceMeshData& source)
{
    return source.positions ? *source.positions : source.draftPositions;
}
[[nodiscard]] inline std::vector<std::array<double, 3>>& editSourcePositions(SourceMeshData& source)
{
    if (source.positions) {
        source.draftPositions = *source.positions;
        source.positions.reset();
    }
    return source.draftPositions;
}
inline void publishSourcePositions(SourceMeshData& source)
{
    if (!source.positions) {
        source.positions = std::make_shared<const std::vector<std::array<double, 3>>>(std::move(source.draftPositions));
    }
}

[[nodiscard]] inline std::array<double, 3> promoteSourcePoint(const std::array<double, 3>& point)
{
    return {point[0], point[1], point[2]};
}

struct DuplicateSettings {
    bool points = true, triangles = true;
    bool showPoints = true, showTriangles = true;
    friend bool operator==(const DuplicateSettings&, const DuplicateSettings&) = default;
};

struct SourcePartInstance {
    uint64_t partId = 0;
    size_t firstIndex = 0, indexCount = 0;
    std::array<double, 16> transform{};
};

struct DuplicateSource {
    uint64_t fileId = 0;
    std::string name;
    std::shared_ptr<const SourceMeshData> data;
    bool wholeFile = false;
    std::vector<SourcePartInstance> parts;
    // Unreferenced points belong to the source file, not an arbitrary mesh part.
    std::array<double, 16> unusedPointTransform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    uint64_t batchId = 0; // Zero means whole file; otherwise a stable representative part ID.
};

struct DuplicateInput {
    DuplicateSettings settings;
    std::vector<DuplicateSource> sources;
};

struct DuplicateMember {
    size_t id = 0; // Zero-based source point or generated triangle ID.
    bool reversed = false;
};

struct DuplicateFinding {
    uint64_t fileId = 0;
    std::string source;
    SourceProvenance provenance = SourceProvenance::importerVertices;
    std::vector<DuplicateMember> members; // Representative first.
    // World positions of all selected instances; points or triangle triples.
    std::vector<std::array<float, 3>> geometry;
    uint64_t batchId = 0;
};

struct DuplicateResult {
    bool enabled = true;
    size_t unavailableSources = 0, availableSources = 0;
    size_t duplicateCount = 0;
    std::vector<DuplicateFinding> findings;
};

struct MeshDuplicates { DuplicateResult points, triangles; };

[[nodiscard]] const char* duplicateStatus(const DuplicateResult& result);
[[nodiscard]] const char* sourceProvenanceName(SourceProvenance provenance);
[[nodiscard]] MeshDuplicates inspectDuplicates(const DuplicateInput& input, std::stop_token stop = {});

} // namespace woby
