#pragma once

#include "model_mesh.h"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mesh_lab {

constexpr size_t maxSourceBytes = 512 * 1024;
constexpr size_t maxCorners = 60000;

struct Corner {
    int position = -1, texcoord = -1, normal = -1;
    bool operator==(const Corner&) const = default;
};
struct SourceLine {
    std::string text;
    size_t byteOffset = 0;
    int position = -1, texcoord = -1, normal = -1, face = -1;
};
struct Face {
    size_t line = 0, firstTriangle = 0, triangleCount = 0;
    std::vector<Corner> corners;
};
struct Triangle {
    size_t face = 0;
    std::array<Corner, 3> corners;
};
struct VertexEvent {
    uint32_t vertex = 0;
    bool created = false;
};
struct Trace {
    std::string name, source;
    std::vector<SourceLine> lines;
    std::vector<woby::Coordinate> positions;
    std::vector<std::array<float, 2>> texcoords;
    std::vector<std::array<float, 3>> normals;
    std::vector<Face> faces;
    std::vector<Triangle> triangles;
    std::vector<Corner> vertexKeys;
    std::vector<std::vector<uint32_t>> positionVertices;
    std::vector<woby::Coordinate> localPositions;
    std::vector<VertexEvent> vertexEvents; // One event per triangulated corner.
    woby::Mesh mesh;
    size_t originalCorners = 0, splitPositions = 0;
    bool generatedNormals = false;
};

// Runs the production loader. All extra lineage is reconstructed and checked
// against its actual output. Only face-based OBJ is supported by this prototype.
[[nodiscard]] Trace loadTrace(const std::filesystem::path& path);
[[nodiscard]] Trace internalExample();
[[nodiscard]] std::string_view internalObjSource();
[[nodiscard]] std::vector<uint8_t> vertexBytes(const Trace& trace);
[[nodiscard]] std::vector<uint8_t> indexBytes(const Trace& trace);
[[nodiscard]] size_t vertexOffset(size_t vertex, size_t component);
[[nodiscard]] bool lineRelated(const Trace& trace, size_t line, size_t triangle);

} // namespace mesh_lab
