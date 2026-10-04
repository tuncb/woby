#pragma once

#include "model_mesh.h"

#include <filesystem>
#include <istream>
#include <optional>
#include <string_view>

namespace woby {

struct ObjMeshCounts {
    size_t sourcePositions = 0;
    size_t triangleIndices = 0;
    size_t lineIndices = 0;
    size_t pointIndices = 0;
    bool vertexCloud = false;
};

// RapidOBJ emits n - 2 triangles for each valid n-corner polygon.
[[nodiscard]] size_t objTriangleIndexCount(size_t corners, size_t faces);

// Counts known before mesh expansion. Only vertex-only clouds have an exact
// render vertex count here; other OBJ files can omit positions or split seams.
void validateObjMeshCounts(const ObjMeshCounts& counts);

struct ObjPointCapacity {
    size_t highestPosition = 0;
    size_t distinctLowerBound = 0;
};
// Each new record-high position is provably distinct, without a growing set.
void checkObjPointReference(ObjPointCapacity& capacity, size_t position);

// Constant-memory preflight, not a syntax validator. Checks a lower bound on
// explicit point vertices, and returns an exact count for vertex-only clouds.
// Other geometry or attributes return nullopt to avoid duplicating a full
// mesh parse. Unused source positions must stay legal.
inline constexpr size_t objPreflightBlockBytes = 1024 * 1024;
[[nodiscard]] std::optional<size_t> scanObjCapacity(std::istream& input,
    const ModelLoadProgressCallback& progress = {}, uintmax_t fileBytesHint = 0);

[[nodiscard]] Mesh loadObjMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress = {});
// OBJ held in memory, with no external material-library reads. The default
// legacy reader accepts polygons; WOBY_RAPIDOBJ_PROTOTYPE also accepts freeforms.
[[nodiscard]] Mesh loadObjMeshText(std::string_view text, const ModelLoadProgressCallback& progress = {});

// Explicit entry points keep the reference loader available for comparisons,
// regardless of WOBY_RAPIDOBJ_PROTOTYPE's application-wide default.
[[nodiscard]] Mesh loadObjMeshLegacy(const std::filesystem::path& path, const ModelLoadProgressCallback& progress = {});
[[nodiscard]] Mesh loadObjMeshTextLegacy(std::string_view text, const ModelLoadProgressCallback& progress = {});
struct ObjPrototypeOptions {
    size_t chunkBytes = 4 * 1024 * 1024, readBytes = 256 * 1024, workers = 0;
};
struct ObjPrototypeMetrics {
    size_t inputBytes = 0, chunks = 0, workers = 0, peakInflightTextBytes = 0, parsedChunkBytes = 0;
    size_t positionCopyBytesAvoided = 0;
    double parseMs = 0, freeformMs = 0, prepareMs = 0;
};
[[nodiscard]] Mesh loadObjMeshPrototype(const std::filesystem::path& path,
    const ModelLoadProgressCallback& progress = {}, const ObjPrototypeOptions& options = {},
    ObjPrototypeMetrics* metrics = nullptr);
[[nodiscard]] Mesh loadObjMeshTextPrototype(std::string_view text,
    const ModelLoadProgressCallback& progress = {}, const ObjPrototypeOptions& options = {},
    ObjPrototypeMetrics* metrics = nullptr);

} // namespace woby
