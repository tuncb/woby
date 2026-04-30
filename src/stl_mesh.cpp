#include "stl_mesh.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace woby {
namespace {

std::string trim(std::string_view value)
{
    size_t first = 0;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }

    size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1u]))) {
        --last;
    }

    return std::string(value.substr(first, last - first));
}

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0u, prefix.size()) == prefix;
}

uint32_t readUint32Le(const unsigned char* data)
{
    return uint32_t(data[0])
        | (uint32_t(data[1]) << 8u)
        | (uint32_t(data[2]) << 16u)
        | (uint32_t(data[3]) << 24u);
}

float readFloat32Le(const unsigned char* data)
{
    const uint32_t bits = readUint32Le(data);
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool isBinaryStlBytes(const std::vector<unsigned char>& bytes)
{
    if (bytes.size() < 84u) {
        return false;
    }

    const uint32_t triangleCount = readUint32Le(bytes.data() + 80u);
    constexpr uint64_t headerSize = 84u;
    constexpr uint64_t triangleSize = 50u;
    const uint64_t expectedSize = headerSize + uint64_t(triangleCount) * triangleSize;
    return expectedSize == bytes.size();
}

std::vector<unsigned char> readFileBytes(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("Failed to open STL: " + path.string());
    }

    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        throw std::runtime_error("Failed to read STL size: " + path.string());
    }

    stream.seekg(0, std::ios::beg);
    std::vector<unsigned char> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            throw std::runtime_error("Failed to read STL: " + path.string());
        }
    }

    return bytes;
}

void reserveTriangles(Mesh& mesh, uint32_t triangleCount)
{
    constexpr size_t verticesPerTriangle = 3u;
    if (triangleCount > std::numeric_limits<uint32_t>::max() / verticesPerTriangle) {
        throw std::runtime_error("STL contains too many triangles.");
    }

    mesh.vertices.reserve(size_t(triangleCount) * verticesPerTriangle);
    mesh.indices.reserve(size_t(triangleCount) * verticesPerTriangle);
}

void appendTriangle(
    Mesh& mesh,
    std::array<float, 3> normal,
    const std::array<float, 3>& a,
    const std::array<float, 3>& b,
    const std::array<float, 3>& c)
{
    if (!finitePosition(a) || !finitePosition(b) || !finitePosition(c)) {
        throw std::runtime_error("STL contains a non-finite vertex coordinate.");
    }

    if (!validNormal(normal)) {
        normal = calculateFaceNormal(a, b, c);
    }

    if (mesh.vertices.size() > std::numeric_limits<uint32_t>::max() - 3u) {
        throw std::runtime_error("STL contains too many vertices.");
    }

    const uint32_t firstIndex = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({a, normal, {}});
    mesh.vertices.push_back({b, normal, {}});
    mesh.vertices.push_back({c, normal, {}});
    mesh.indices.push_back(firstIndex + 0u);
    mesh.indices.push_back(firstIndex + 1u);
    mesh.indices.push_back(firstIndex + 2u);
}

Mesh loadBinaryStlMesh(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    const uint32_t triangleCount = readUint32Le(bytes.data() + 80u);
    Mesh mesh;
    reserveTriangles(mesh, triangleCount);

    size_t offset = 84u;
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        std::array<float, 3> normal = {
            readFloat32Le(bytes.data() + offset + 0u),
            readFloat32Le(bytes.data() + offset + 4u),
            readFloat32Le(bytes.data() + offset + 8u),
        };

        std::array<float, 3> vertices[3] = {};
        for (size_t vertex = 0; vertex < 3u; ++vertex) {
            const size_t vertexOffset = offset + 12u + vertex * 12u;
            vertices[vertex] = {
                readFloat32Le(bytes.data() + vertexOffset + 0u),
                readFloat32Le(bytes.data() + vertexOffset + 4u),
                readFloat32Le(bytes.data() + vertexOffset + 8u),
            };
        }

        appendTriangle(mesh, normal, vertices[0], vertices[1], vertices[2]);
        offset += 50u;
    }

    if (empty(mesh)) {
        throw std::runtime_error("STL did not contain renderable triangles: " + path.string());
    }

    MeshNode node;
    node.name = path.stem().string().empty() ? "stl" : path.stem().string();
    node.indexOffset = 0u;
    node.indexCount = static_cast<uint32_t>(mesh.indices.size());
    mesh.nodes.push_back(std::move(node));
    finalizeMesh(mesh, false);
    return mesh;
}

std::array<float, 3> parseFloat3(std::istringstream& stream, const std::string& context)
{
    std::array<float, 3> value{};
    if (!(stream >> value[0] >> value[1] >> value[2])) {
        throw std::runtime_error("Invalid ASCII STL " + context + ".");
    }

    return value;
}

Mesh loadAsciiStlMesh(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    const std::string text(bytes.begin(), bytes.end());
    std::istringstream input(text);
    Mesh mesh;
    std::string solidName;
    std::array<float, 3> currentNormal{};
    std::vector<std::array<float, 3>> facetVertices;
    facetVertices.reserve(3u);

    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }

        if (startsWith(trimmed, "solid")) {
            if (solidName.empty()) {
                solidName = trim(std::string_view(trimmed).substr(5u));
            }
            continue;
        }

        if (startsWith(trimmed, "facet normal")) {
            std::istringstream stream(trimmed.substr(12u));
            currentNormal = parseFloat3(stream, "facet normal");
            facetVertices.clear();
            continue;
        }

        if (startsWith(trimmed, "outer loop") || startsWith(trimmed, "endloop")) {
            continue;
        }

        if (startsWith(trimmed, "vertex")) {
            std::istringstream stream(trimmed.substr(6u));
            facetVertices.push_back(parseFloat3(stream, "vertex"));
            continue;
        }

        if (startsWith(trimmed, "endfacet")) {
            if (facetVertices.size() != 3u) {
                throw std::runtime_error("ASCII STL facet did not contain exactly 3 vertices.");
            }

            appendTriangle(mesh, currentNormal, facetVertices[0], facetVertices[1], facetVertices[2]);
            facetVertices.clear();
            currentNormal = {};
            continue;
        }

        if (startsWith(trimmed, "endsolid")) {
            continue;
        }
    }

    if (empty(mesh)) {
        throw std::runtime_error("STL did not contain renderable triangles: " + path.string());
    }

    MeshNode node;
    node.name = solidName.empty() ? path.stem().string() : solidName;
    if (node.name.empty()) {
        node.name = "stl";
    }
    node.indexOffset = 0u;
    node.indexCount = static_cast<uint32_t>(mesh.indices.size());
    mesh.nodes.push_back(std::move(node));
    finalizeMesh(mesh, false);
    return mesh;
}

} // namespace

Mesh loadStlMesh(const std::filesystem::path& path)
{
    const std::vector<unsigned char> bytes = readFileBytes(path);
    if (isBinaryStlBytes(bytes)) {
        return loadBinaryStlMesh(path, bytes);
    }

    return loadAsciiStlMesh(path, bytes);
}

} // namespace woby
