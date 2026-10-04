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
// Polygonal OBJ held in memory; external material libraries and freeform
// statements are not loaded. Uses the same mesh construction as file import.
[[nodiscard]] Mesh loadObjMeshText(std::string_view text, const ModelLoadProgressCallback& progress = {});

} // namespace woby
