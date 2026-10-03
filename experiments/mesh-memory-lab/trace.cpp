#include "trace.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include <rapidobj/rapidobj.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace mesh_lab {
namespace {
static_assert(sizeof(woby::Vertex) == 32);
static_assert(offsetof(woby::Vertex, normal) == 12);
static_assert(offsetof(woby::Vertex, texcoord) == 24);
static_assert(sizeof(woby::Coordinate) == 24);
static_assert(sizeof(Corner) == 12);

Corner corner(const rapidobj::Index& index)
{
    return {index.position_index, index.texcoord_index, index.normal_index};
}
std::string readSource(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) { throw std::runtime_error("Cannot open the OBJ file."); }
    std::string source(maxSourceBytes + 1, '\0');
    file.read(source.data(), static_cast<std::streamsize>(source.size()));
    source.resize(static_cast<size_t>(file.gcount()));
    if (source.size() > maxSourceBytes) { throw std::runtime_error("Prototype limit: 512 KiB of OBJ source."); }
    return source;
}
template<typename T> std::vector<uint8_t> bytes(const std::vector<T>& input)
{
    std::vector<uint8_t> result(input.size() * sizeof(T));
    if (!result.empty()) { std::memcpy(result.data(), input.data(), result.size()); }
    return result;
}
} // namespace

static Trace captureSource(std::string source, woby::Mesh mesh, std::string name)
{
    Trace trace;
    trace.name = std::move(name);
    trace.source = std::move(source);
    trace.mesh = std::move(mesh);
    size_t offset = 0;
    int positionId = 0, uvId = 0, normalId = 0, faceId = 0;
    std::vector<size_t> faceLines;
    while (offset < trace.source.size()) {
        const size_t end = trace.source.find('\n', offset);
        const size_t count = end == std::string::npos ? trace.source.size() - offset : end - offset;
        SourceLine line;
        line.text = trace.source.substr(offset, count);
        line.byteOffset = offset;
        std::istringstream tokens(line.text);
        std::string keyword;
        tokens >> keyword;
        if (keyword == "v") { line.position = positionId++; }
        else if (keyword == "vt") { line.texcoord = uvId++; }
        else if (keyword == "vn") { line.normal = normalId++; }
        else if (keyword == "f") { line.face = faceId++; faceLines.push_back(trace.lines.size()); }
        else if (!keyword.empty() && keyword[0] != '#' && keyword != "o" && keyword != "g"
            && keyword != "s" && keyword != "usemtl" && keyword != "mtllib") {
            throw std::runtime_error("Prototype supports polygonal OBJ only; unsupported statement: " + keyword);
        }
        if (line.text.find('\\') != std::string::npos) {
            throw std::runtime_error("Continued OBJ lines are not supported by the source-line tracer.");
        }
        trace.lines.push_back(std::move(line));
        offset += count + (end == std::string::npos ? 0 : 1);
    }
    std::istringstream stream(trace.source);
    // Triangulate requires the per-face material array, which Ignore omits.
    auto parsed = rapidobj::ParseStream(stream, rapidobj::MaterialLibrary::String(""));
    if (parsed.error) { throw std::runtime_error("OBJ parse error: " + parsed.error.code.message()); }
    for (size_t i = 0; i < parsed.attributes.positions.size(); i += 3) {
        const woby::Coordinate point{parsed.attributes.positions[i], parsed.attributes.positions[i+1], parsed.attributes.positions[i+2]};
        if (!woby::finiteCoordinate(point)) { throw std::runtime_error("Non-finite source position."); }
        trace.positions.push_back(point);
    }
    for (size_t i = 0; i < parsed.attributes.texcoords.size(); i += 2) {
        trace.texcoords.push_back({parsed.attributes.texcoords[i], parsed.attributes.texcoords[i+1]});
    }
    for (size_t i = 0; i < parsed.attributes.normals.size(); i += 3) {
        trace.normals.push_back({parsed.attributes.normals[i], parsed.attributes.normals[i+1], parsed.attributes.normals[i+2]});
    }
    size_t triangleCount = 0;
    for (const auto& shape : parsed.shapes) {
        size_t first = 0;
        for (const auto count : shape.mesh.num_face_vertices) {
            if (count < 3 || trace.faces.size() >= faceLines.size()) { throw std::runtime_error("Invalid face lineage."); }
            Face face;
            face.line = faceLines[trace.faces.size()];
            face.firstTriangle = triangleCount;
            face.triangleCount = static_cast<size_t>(count) - 2;
            for (size_t c = 0; c < count; ++c) { face.corners.push_back(corner(shape.mesh.indices[first+c])); }
            first += count;
            triangleCount += face.triangleCount;
            trace.originalCorners += count;
            trace.faces.push_back(std::move(face));
        }
    }
    if (triangleCount == 0) { throw std::runtime_error("The trace needs at least one polygonal face."); }
    if (triangleCount > maxCorners / 3) { throw std::runtime_error("Prototype limit: 20,000 triangles."); }
    // Use the same coordinate localization before triangulation as loadObjMesh.
    const auto origin = woby::coordinateOrigin(trace.positions);
    for (size_t i = 0; i < trace.positions.size(); ++i) {
        const auto local = woby::relativePosition(trace.positions[i], origin);
        trace.localPositions.push_back(local);
        for (size_t axis = 0; axis < 3; ++axis) { parsed.attributes.positions[i*3+axis] = local[axis]; }
    }
    if (!rapidobj::Triangulate(parsed)) { throw std::runtime_error("RapidOBJ could not triangulate the source."); }
    if (trace.mesh.indices.size() != triangleCount * 3 || !trace.mesh.lineIndices.empty() || !trace.mesh.pointIndices.empty()) {
        throw std::runtime_error("Production mesh and polygon trace disagree.");
    }
    trace.vertexKeys.resize(trace.mesh.vertices.size());
    std::vector<bool> seen(trace.mesh.vertices.size());
    size_t globalCorner = 0, faceIndex = 0;
    for (const auto& shape : parsed.shapes) {
        for (size_t i = 0; i < shape.mesh.indices.size(); i += 3) {
            const size_t triangleId = globalCorner / 3;
            while (triangleId >= trace.faces[faceIndex].firstTriangle + trace.faces[faceIndex].triangleCount) { ++faceIndex; }
            Triangle triangle;
            triangle.face = faceIndex;
            for (size_t c = 0; c < 3; ++c, ++globalCorner) {
                const auto key = corner(shape.mesh.indices[i+c]);
                triangle.corners[c] = key;
                const auto vertex = trace.mesh.indices[globalCorner];
                if (vertex >= trace.vertexKeys.size() || key.position < 0
                    || trace.mesh.sourceData->indices[globalCorner] != static_cast<uint32_t>(key.position)) {
                    throw std::runtime_error("Source-to-render index correspondence failed.");
                }
                if (seen[vertex] && trace.vertexKeys[vertex] != key) { throw std::runtime_error("Vertex tuple correspondence failed."); }
                trace.vertexEvents.push_back({vertex, !seen[vertex]});
                seen[vertex] = true;
                trace.vertexKeys[vertex] = key;
                const auto local = woby::relativePosition(trace.positions[static_cast<size_t>(key.position)], origin);
                if (local != trace.mesh.precisePositions[vertex]) { throw std::runtime_error("Position correspondence failed."); }
                if (key.normal < 0 || !woby::validNormal(trace.normals[static_cast<size_t>(key.normal)])) { trace.generatedNormals = true; }
            }
            trace.triangles.push_back(triangle);
        }
    }
    trace.positionVertices.resize(trace.positions.size());
    for (size_t v = 0; v < trace.vertexKeys.size(); ++v) {
        trace.positionVertices[static_cast<size_t>(trace.vertexKeys[v].position)].push_back(static_cast<uint32_t>(v));
    }
    for (const auto& vertices : trace.positionVertices) { if (vertices.size() > 1) { ++trace.splitPositions; } }
    return trace;
}

Trace loadTrace(const std::filesystem::path& path)
{
    auto source = readSource(path);
    auto mesh = woby::loadObjMesh(std::filesystem::absolute(path));
    if (source != readSource(path)) { throw std::runtime_error("The file changed during capture. Reload it."); }
    return captureSource(std::move(source), std::move(mesh), woby::pathToUtf8(path.filename()));
}

Trace internalExample()
{
    const auto text = internalObjSource();
    return captureSource(std::string(text), woby::loadObjMeshText(text), "Folded sheet");
}

std::vector<uint8_t> vertexBytes(const Trace& trace) { return bytes(trace.mesh.vertices); }
std::vector<uint8_t> indexBytes(const Trace& trace) { return bytes(trace.mesh.indices); }
size_t vertexOffset(size_t vertex, size_t component)
{
    if (component >= 8 || vertex > (std::numeric_limits<size_t>::max() - 28) / sizeof(woby::Vertex)) {
        throw std::out_of_range("Vertex component offset out of range.");
    }
    return vertex * sizeof(woby::Vertex) + component * sizeof(float);
}
bool lineRelated(const Trace& trace, size_t lineId, size_t triangleId)
{
    if (lineId >= trace.lines.size() || triangleId >= trace.triangles.size()) { return false; }
    const auto& line = trace.lines[lineId];
    const auto& triangle = trace.triangles[triangleId];
    if (line.face >= 0 && static_cast<size_t>(line.face) == triangle.face) { return true; }
    for (const auto& key : triangle.corners) {
        if ((line.position >= 0 && line.position == key.position)
            || (line.normal >= 0 && line.normal == key.normal)
            || (line.texcoord >= 0 && line.texcoord == key.texcoord)) { return true; }
    }
    return false;
}
} // namespace mesh_lab
