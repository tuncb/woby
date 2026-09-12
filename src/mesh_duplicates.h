#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace woby {

enum class SourceProvenance { objPositions, stlCorners, importerVertices };

// Captured before render optimization. Triangle IDs refer to generated triangles,
// not original polygons. Promoting importer floats does not recover precision.
struct SourceMeshData {
    SourceProvenance provenance = SourceProvenance::importerVertices;
    std::vector<std::array<double, 3>> points;
    std::vector<uint32_t> indices;
};

struct DuplicateSettings {
    bool points = true, triangles = true;
    bool showPoints = true, showTriangles = true;
    friend bool operator==(const DuplicateSettings&, const DuplicateSettings&) = default;
};

struct SourcePartInstance {
    uint64_t partId = 0;
    size_t firstIndex = 0, indexCount = 0;
    std::array<float, 16> transform{};
};

struct DuplicateSource {
    uint64_t fileId = 0;
    std::string name;
    std::shared_ptr<const SourceMeshData> data;
    bool wholeFile = false;
    std::vector<SourcePartInstance> parts;
    // Unreferenced points belong to the source file, not an arbitrary mesh part.
    std::array<float, 16> unusedPointTransform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
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
};

struct DuplicateResult {
    bool enabled = true;
    size_t unavailableSources = 0, availableSources = 0;
    size_t duplicateCount = 0, informationalCount = 0;
    std::vector<DuplicateFinding> findings;
};

struct MeshDuplicates { DuplicateResult points, triangles; };

[[nodiscard]] const char* duplicateStatus(const DuplicateResult& result);
[[nodiscard]] const char* sourceProvenanceName(SourceProvenance provenance);
[[nodiscard]] MeshDuplicates inspectDuplicates(const DuplicateInput& input, std::stop_token stop = {});

} // namespace woby
