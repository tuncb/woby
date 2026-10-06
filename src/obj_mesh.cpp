#include "obj_mesh.h"
#include "obj_freeform.h"
#include "scene_buffer_size.h"
#include <sstream>
#include "utf8_path.h"

#include <rapidobj/rapidobj.hpp>
#include <rapidobj/prototype.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace woby {
namespace {

struct IndexKey {
    int vertex = -1;
    int normal = -1;
    int texcoord = -1;

    bool operator==(const IndexKey& other) const noexcept
    {
        return vertex == other.vertex && normal == other.normal && texcoord == other.texcoord;
    }
};

// First-use IDs are global across shapes. Most corners hit the primary entry
// for their source position; only alternate normal/UV tuples need hashing.
struct VertexIndexTable {
    std::vector<IndexKey> keys;
    std::vector<uint32_t> primary, buckets;
    size_t secondaryCount = 0;
    bool direct = false;
    uint32_t next = 0;
};
constexpr uint32_t emptyBucket = std::numeric_limits<uint32_t>::max();

size_t indexHash(const IndexKey& key)
{
    uint64_t hash = uint64_t(static_cast<uint32_t>(key.vertex)) * 0x9e3779b185ebca87ull;
    hash ^= uint64_t(static_cast<uint32_t>(key.normal)) * 0xc2b2ae3d27d4eb4full;
    hash ^= uint64_t(static_cast<uint32_t>(key.texcoord)) * 0x165667b19e3779f9ull;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    return static_cast<size_t>(hash ^ (hash >> 33));
}

void growSecondary(VertexIndexTable& table)
{
    std::vector<uint32_t> buckets(std::max(size_t{16}, table.buckets.size() * 2), emptyBucket);
    for (const auto id : table.buckets) {
        if (id == emptyBucket) { continue; }
        size_t bucket = indexHash(table.keys[id]) & (buckets.size() - 1);
        while (buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (buckets.size() - 1); }
        buckets[bucket] = id;
    }
    table.buckets = std::move(buckets);
}

uint32_t vertexIndex(VertexIndexTable& table, const IndexKey& key)
{
    auto& primary = table.primary[static_cast<size_t>(key.vertex)];
    if (table.direct) {
        if (primary == emptyBucket) {
            (void)sceneBufferBytes(size_t(table.next) + 1, sizeof(Vertex));
            primary = table.next++;
        }
        return primary;
    }
    if (primary == emptyBucket) {
        (void)sceneBufferBytes(table.keys.size() + 1, sizeof(Vertex));
        primary = static_cast<uint32_t>(table.keys.size());
        table.keys.push_back(key);
        return primary;
    }
    if (table.keys[primary] == key) { return primary; }
    if (table.buckets.empty()) { growSecondary(table); }
    size_t bucket = indexHash(key) & (table.buckets.size() - 1);
    while (table.buckets[bucket] != emptyBucket) {
        const auto id = table.buckets[bucket];
        if (table.keys[id] == key) { return id; }
        bucket = (bucket + 1) & (table.buckets.size() - 1);
    }
    (void)sceneBufferBytes(table.keys.size() + 1, sizeof(Vertex));
    if (table.secondaryCount >= table.buckets.size() * 3 / 4) {
        growSecondary(table);
        bucket = indexHash(key) & (table.buckets.size() - 1);
        while (table.buckets[bucket] != emptyBucket) { bucket = (bucket + 1) & (table.buckets.size() - 1); }
    }
    const auto id = static_cast<uint32_t>(table.keys.size());
    table.keys.push_back(key);
    table.buckets[bucket] = id;
    ++table.secondaryCount;
    return id;
}

void reserveIndexTable(VertexIndexTable& table, size_t positions, size_t corners, bool positionOnly)
{
    table.primary.assign(positions, emptyBucket);
    table.direct = positionOnly;
    if (!table.direct) { table.keys.reserve(std::min(positions, corners)); }
}

rapidobj::Result parseObj(const std::filesystem::path& path)
{
#if defined(_WIN32)
    // RapidOBJ's Windows file reader bypasses the OS page cache. That is useful
    // for large parallel reads but makes thousands of tiny files I/O-bound.
    // Match its single-thread cutoff, retaining ParseFile for larger meshes.
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (!error && bytes <= 1024 * 1024) {
        std::ifstream stream(path, std::ios::binary);
        if (stream) {
            return rapidobj::ParseStream(stream, rapidobj::MaterialLibrary::SearchPath(
                std::filesystem::absolute(path).parent_path(), rapidobj::Load::Optional));
        }
    }
#endif
    return rapidobj::ParseFile(path, rapidobj::MaterialLibrary::Default(rapidobj::Load::Optional));
}

void preflightObjCapacity(const std::filesystem::path& path, const ModelLoadProgressCallback& progress)
{
    // The shortest valid position row is "v 0 0 0\n" (the final newline may
    // be absent). File size only decides whether a scan could help; it never
    // rejects a file. Smaller files cannot contain an oversized vertex cloud.
    constexpr uintmax_t minimumOversizedCloudBytes =
        (uintmax_t(std::numeric_limits<SceneBufferSize>::max()) / sizeof(Vertex) + 1) * 8 - 1;
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes < minimumOversizedCloudBytes) { return; }
    std::ifstream input(path, std::ios::binary);
    if (!input) { return; } // Let the parser report its usual path-aware error.
    if (const auto count = scanObjCapacity(input, progress, bytes)) {
        validateObjMeshCounts({*count, 0, 0, *count, true});
    }
}

template <bool Prototype = false>
Mesh buildObjMesh(rapidobj::Result result, std::vector<FreeformPatch> freeformPatches,
    const std::filesystem::path& path, const ModelLoadProgressCallback& progress,
    rapidobj::GeometryBuffers* buffers = nullptr)
{
    const auto throwLoadError = [&](const char* operation) {
        std::string message = std::string(operation) + ": " + pathToUtf8(path);
        if (result.error) {
            message += "\n" + result.error.code.message();
            if (result.error.line_num != 0) {
                message += " (line " + std::to_string(result.error.line_num) + ")";
            }
        }
        throw std::runtime_error(message);
    };
    if (result.error) {
        throwLoadError("Failed to load OBJ");
    }

    // Reject known failures before allocating double coordinates, vertex maps,
    // render vertices or upload copies. Polygon expansion, line segment and
    // standalone cloud counts are known from the parsed primitive records.
    reportModelLoadProgress(progress, ModelLoadStage::reading);
    ObjMeshCounts counts;
    counts.sourcePositions = Prototype ? buffers->positions.size() : result.attributes.positions.size() / 3;
    size_t primitiveVisits = 0;
    for (const auto& shape : result.shapes) {
        counts.triangleIndices += objTriangleIndexCount(shape.mesh.indices.size(), shape.mesh.num_face_vertices.size());
        counts.pointIndices += shape.points.indices.size();
        size_t lineVertices = 0;
        for (const auto count : shape.lines.num_line_vertices) {
            if (primitiveVisits++ % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading); }
            if (count < 2) { throw std::runtime_error("OBJ polyline needs at least two vertices."); }
            lineVertices += static_cast<size_t>(count);
            counts.lineIndices += (static_cast<size_t>(count) - 1) * 2;
            (void)sceneBufferBytes(counts.lineIndices, sizeof(uint32_t));
        }
        if (lineVertices != shape.lines.indices.size()) { throw std::runtime_error("Invalid OBJ polyline range."); }
        validateObjMeshCounts(counts);
        if (primitiveVisits++ % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading); }
    }
    counts.vertexCloud = counts.triangleIndices == 0 && counts.lineIndices == 0
        && counts.pointIndices == 0 && freeformPatches.empty();
    if (counts.vertexCloud) { counts.pointIndices = counts.sourcePositions; }
    validateObjMeshCounts(counts);

    std::vector<Coordinate> sourcePoints;
    if constexpr (Prototype) { sourcePoints = std::move(buffers->positions); }
    else {
        sourcePoints.reserve(result.attributes.positions.size() / 3);
        for (size_t i = 0; i < result.attributes.positions.size(); i += 3) {
            sourcePoints.push_back({result.attributes.positions[i], result.attributes.positions[i+1], result.attributes.positions[i+2]});
        }
    }
    const auto origin = coordinateOrigin(sourcePoints);
    for (size_t i = 0; i < sourcePoints.size(); ++i) {
        sourcePoints[i] = relativePosition(sourcePoints[i], origin);
        if constexpr (!Prototype) {
            for (size_t k = 0; k < 3; ++k) { result.attributes.positions[i*3+k] = sourcePoints[i][k]; }
        }
    }
    reportModelLoadProgress(progress, ModelLoadStage::triangulating);
    if (!(Prototype ? rapidobj::Triangulate(result, sourcePoints) : rapidobj::Triangulate(result))) {
        throwLoadError("Failed to triangulate OBJ");
    }

    const auto& attrib = result.attributes;
    const auto& shapes = result.shapes;
    const auto uvValue = [&](size_t index) {
        return Prototype ? static_cast<float>(buffers->texcoords[index / 2][index % 2]) : attrib.texcoords[index];
    };
    const auto normalValue = [&](size_t index) {
        return Prototype ? static_cast<float>(buffers->normals[index / 3][index % 3]) : attrib.normals[index];
    };

    Mesh mesh;
    auto source = std::make_shared<SourceMeshData>();
    source->provenance = SourceProvenance::objPositions;
    mesh.origin = origin;
    source->points = std::move(sourcePoints);
    reportModelLoadProgress(progress, ModelLoadStage::sourcePositions, source->points.size(), source->points.size());
    VertexIndexTable vertexMap;
    counts.triangleIndices = 0;
    for (const auto& shape : shapes) {
        counts.triangleIndices += shape.mesh.indices.size();
        validateObjMeshCounts(counts);
    }
    // Vertex-only OBJ files are commonly used as point clouds. In files with
    // primitives, only explicit p records become standalone point geometry.
    const auto indexCount = counts.triangleIndices;
    const auto lineIndexCount = counts.lineIndices;
    const auto pointIndexCount = counts.pointIndices;
    const size_t totalIndices = indexCount + lineIndexCount + pointIndexCount;
    // Position count is only an estimate: normal/UV seams can split vertices.
    const size_t vertexCapacity = std::min({source->points.size(), totalIndices,
        size_t(std::numeric_limits<SceneBufferSize>::max() / sizeof(Vertex))});
    mesh.indices.reserve(indexCount);
    mesh.lineIndices.reserve(lineIndexCount);
    mesh.pointIndices.reserve(pointIndexCount);
    mesh.vertices.reserve(vertexCapacity);
    mesh.precisePositions.reserve(vertexCapacity);
    mesh.nodes.reserve(shapes.size());
    source->indices.reserve(indexCount);
    reserveIndexTable(vertexMap, source->points.size(), vertexCapacity,
        Prototype ? buffers->normals.empty() && buffers->texcoords.empty() : attrib.normals.empty() && attrib.texcoords.empty());
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, 0, totalIndices);

    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const uint32_t nodeIndexOffset = static_cast<uint32_t>(mesh.indices.size());
        bool hasTexcoords = true;

        for (const auto& index : shape.mesh.indices) {
            hasTexcoords = hasTexcoords && index.texcoord_index >= 0;
            if (index.texcoord_index >= 0) {
                const auto uv = static_cast<size_t>(index.texcoord_index) * 2u;
                hasTexcoords = hasTexcoords && std::isfinite(uvValue(uv)) && std::isfinite(uvValue(uv + 1u));
            }
            if (mesh.indices.size() % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, mesh.indices.size(), totalIndices); }
            if (index.position_index < 0 || static_cast<size_t>(index.position_index) >= source->points.size()) {
                throw std::runtime_error("OBJ contains an invalid source position index.");
            }
            source->indices.push_back(static_cast<uint32_t>(index.position_index));
            const IndexKey key{index.position_index, index.normal_index, index.texcoord_index};
            const auto mappedIndex = vertexIndex(vertexMap, key);
            if (mappedIndex < mesh.vertices.size()) {
                mesh.indices.push_back(mappedIndex);
                continue;
            }

            if (index.position_index < 0) {
                throw std::runtime_error("OBJ contains a face vertex without a position index.");
            }

            Vertex vertex{};
            const auto& point = source->points[static_cast<size_t>(index.position_index)];
            mesh.precisePositions.push_back(point);
            vertex.position = renderPosition(point);

            if (index.normal_index >= 0) {
                const auto normalIndex = static_cast<size_t>(index.normal_index) * 3u;
                vertex.normal = {
                    normalValue(normalIndex + 0u),
                    normalValue(normalIndex + 1u),
                    normalValue(normalIndex + 2u),
                };
            }

            if (index.texcoord_index >= 0) {
                const auto texcoordIndex = static_cast<size_t>(index.texcoord_index) * 2u;
                vertex.texcoord = {
                    uvValue(texcoordIndex + 0u),
                    1.0f - uvValue(texcoordIndex + 1u),
                };
            }

            const uint32_t newIndex = static_cast<uint32_t>(mesh.vertices.size());
            mesh.vertices.push_back(vertex);
            mesh.indices.push_back(newIndex);
        }

        const uint32_t nodeIndexCount = static_cast<uint32_t>(mesh.indices.size()) - nodeIndexOffset;
        if (nodeIndexCount > 0) {
            MeshNode node;
            node.hasTexcoords = hasTexcoords;
            node.name = shape.name.empty() ? "shape " + std::to_string(shapeIndex + 1u) : shape.name;
            node.indexOffset = nodeIndexOffset;
            node.indexCount = nodeIndexCount;
            mesh.nodes.push_back(std::move(node));
        }
    }

    // Append new primitive groups after all face groups, preserving existing
    // face group indexes and saved appearance in older mixed-geometry scenes.
    std::unordered_set<std::string> names;
    for (const auto& node : mesh.nodes) { names.insert(node.name); }
    const auto groupName = [&](const std::string& base) {
        std::string name = base;
        for (size_t suffix = 2; !names.insert(name).second; ++suffix) { name = base + " (" + std::to_string(suffix) + ")"; }
        return name;
    };
    primitiveVisits = 0;
    const auto primitiveVertex = [&](int positionIndex) {
        if (positionIndex < 0 || static_cast<size_t>(positionIndex) >= source->points.size()) {
            throw std::runtime_error("OBJ contains an invalid source position index.");
        }
        const auto mapped = vertexIndex(vertexMap, {positionIndex, -1, -1});
        if (mapped == mesh.vertices.size()) {
            const auto& point = source->points[static_cast<size_t>(positionIndex)];
            mesh.precisePositions.push_back(point);
            // Unlit primitives do not need normals. A valid placeholder avoids
            // triggering regeneration of authored face normals in mixed files.
            mesh.vertices.push_back({renderPosition(point), {0, 0, 1}, {}});
        }
        const size_t completed = mesh.indices.size() + mesh.lineIndices.size() + mesh.pointIndices.size();
        if (primitiveVisits++ % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, completed, totalIndices); }
        return mapped;
    };
    for (size_t shapeIndex = 0; shapeIndex < shapes.size(); ++shapeIndex) {
        const auto& shape = shapes[shapeIndex];
        const auto base = shape.name.empty() ? "shape " + std::to_string(shapeIndex + 1) : shape.name;
        if (!shape.lines.indices.empty()) {
            MeshNode node;
            node.name = groupName(base + (shape.mesh.indices.empty() ? "" : " (lines)"));
            node.lineIndexOffset = static_cast<uint32_t>(mesh.lineIndices.size());
            size_t begin = 0;
            for (const auto count : shape.lines.num_line_vertices) {
                auto previous = primitiveVertex(shape.lines.indices[begin].position_index);
                for (size_t i = 1; i < static_cast<size_t>(count); ++i) {
                    const auto next = primitiveVertex(shape.lines.indices[begin + i].position_index);
                    mesh.lineIndices.push_back(previous);
                    mesh.lineIndices.push_back(next);
                    previous = next;
                }
                begin += static_cast<size_t>(count);
            }
            node.lineIndexCount = static_cast<uint32_t>(mesh.lineIndices.size()) - node.lineIndexOffset;
            mesh.nodes.push_back(std::move(node));
        }
        if (!shape.points.indices.empty()) {
            MeshNode node;
            node.name = groupName(base + (shape.mesh.indices.empty() && shape.lines.indices.empty() ? "" : " (points)"));
            node.pointIndexOffset = static_cast<uint32_t>(mesh.pointIndices.size());
            for (const auto& index : shape.points.indices) { mesh.pointIndices.push_back(primitiveVertex(index.position_index)); }
            node.pointIndexCount = static_cast<uint32_t>(mesh.pointIndices.size()) - node.pointIndexOffset;
            mesh.nodes.push_back(std::move(node));
        }
    }
    if (counts.vertexCloud && !source->points.empty()) {
        MeshNode node;
        node.name = "Points";
        for (size_t i = 0; i < source->points.size(); ++i) { mesh.pointIndices.push_back(primitiveVertex(static_cast<int>(i))); }
        node.pointIndexCount = static_cast<uint32_t>(mesh.pointIndices.size());
        mesh.nodes.push_back(std::move(node));
    }
    if (empty(mesh) && freeformPatches.empty()) { throw std::runtime_error("OBJ did not contain renderable geometry: " + pathToUtf8(path)); }

    mesh.sourceData = std::move(source);
    reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, totalIndices, totalIndices);
    if (!empty(mesh)) { finalizeMesh(mesh, true, progress); }
    if (!freeformPatches.empty()) {
        appendFreeformGeometry(mesh, std::move(freeformPatches), progress);
        finalizeMesh(mesh, false, progress);
    }
    return mesh;
}

} // namespace

size_t objTriangleIndexCount(size_t corners, size_t faces)
{
    if (faces > corners / 3 || (faces == 0 && corners != 0)) {
        throw std::runtime_error("Invalid OBJ polygon range.");
    }
    const auto triangles = corners - 2 * faces;
    (void)sceneBufferBytes(triangles, 3 * sizeof(uint32_t));
    return triangles * 3;
}

void validateObjMeshCounts(const ObjMeshCounts& counts)
{
    (void)sceneBufferBytes(counts.triangleIndices, sizeof(uint32_t));
    (void)sceneBufferBytes(counts.lineIndices, sizeof(uint32_t));
    if (counts.vertexCloud) { (void)sceneBufferBytes(counts.sourcePositions, sizeof(Vertex)); }
    // Explicit point references are deduplicated per group by the renderer.
    // Their source count is not an exact GPU point-buffer size.
    constexpr size_t maxIndices = std::numeric_limits<uint32_t>::max();
    if (counts.sourcePositions > static_cast<size_t>(std::numeric_limits<int32_t>::max())
        || counts.pointIndices > maxIndices
        || counts.triangleIndices > maxIndices - counts.pointIndices
        || counts.lineIndices > maxIndices - counts.pointIndices - counts.triangleIndices) {
        throw std::runtime_error("OBJ exceeds the supported vertex or index range.");
    }
}

void checkObjPointReference(ObjPointCapacity& capacity, size_t position)
{
    if (position <= capacity.highestPosition) { return; }
    (void)sceneBufferBytes(capacity.distinctLowerBound, sizeof(Vertex));
    (void)sceneBufferBytes(capacity.distinctLowerBound + 1, sizeof(Vertex));
    capacity.highestPosition = position;
    ++capacity.distinctLowerBound;
}

std::optional<size_t> scanObjCapacity(std::istream& input, const ModelLoadProgressCallback& progress, uintmax_t fileBytesHint)
{
    std::vector<char> buffer(objPreflightBlockBytes);
    std::array<char, 5> keyword;
    size_t keywordLength = 0, positions = 0;
    enum class ScanState { keyword, skipLine, points };
    auto state = ScanState::keyword;
    ObjPointCapacity pointCapacity;
    size_t pointValue = 0;
    bool hasPoints = false, pointStarted = false, pointDigits = false, pointNegative = false;
    bool firstBlock = true;
    const auto finishKeyword = [&] {
        const std::string_view token(keyword.data(), keywordLength);
        state = ScanState::skipLine;
        if (token == "v") { ++positions; return true; }
        if (token == "p") { hasPoints = true; state = ScanState::points; return true; }
        // Attribute-rich files commonly precede faces with long position and
        // normal/UV sections. Avoid duplicating that full parse; the parsed
        // count checks still run before coordinate and mesh expansion.
        return token != "vn" && token != "vt" && token != "vp" && token != "f" && token != "l"
            && token != "curv" && token != "curv2" && token != "surf";
    };
    const auto finishPoint = [&] {
        if (!pointStarted) { return true; }
        if (!pointDigits || pointValue == 0 || (pointNegative && pointValue > positions)) { return false; }
        const auto position = pointNegative ? positions - (pointValue - 1) : pointValue;
        checkObjPointReference(pointCapacity, position);
        pointValue = 0;
        pointStarted = pointDigits = pointNegative = false;
        return true;
    };
    while (input) {
        reportModelLoadProgress(progress, ModelLoadStage::reading);
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const char* cursor = buffer.data();
        const char* end = cursor + input.gcount();
        while (cursor != end) {
            // Position rows dominate simple clouds. Skip their numeric
            // payload without token construction; block boundaries use the
            // same general scanner below.
            if (state == ScanState::keyword && keywordLength == 0 && end - cursor >= 3) {
                if (cursor[0] == 'v' && (cursor[1] == ' ' || cursor[1] == '\t')) {
                    ++positions; cursor += 2; state = ScanState::skipLine; continue;
                }
            }
            if (state == ScanState::skipLine) {
                const auto* newline = static_cast<const char*>(std::memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
                if (!newline) { break; }
                cursor = newline + 1;
                state = ScanState::keyword;
                keywordLength = 0;
                continue;
            }
            const char ch = *cursor++;
            if (state == ScanState::points) {
                if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t' || ch == '#') {
                    if (!finishPoint()) { return std::nullopt; }
                    if (ch == '\n') { state = ScanState::keyword; }
                    if (ch == '#') { state = ScanState::skipLine; }
                } else if ((ch == '+' || ch == '-') && !pointStarted) {
                    pointStarted = true;
                    pointNegative = ch == '-';
                } else if (ch >= '0' && ch <= '9') {
                    const auto digit = static_cast<size_t>(ch - '0');
                    constexpr auto maxPrefix = std::numeric_limits<size_t>::max() / 10;
                    constexpr auto maxDigit = std::numeric_limits<size_t>::max() % 10;
                    if (pointValue > maxPrefix || (pointValue == maxPrefix && digit > maxDigit)) { return std::nullopt; }
                    pointValue = pointValue * 10 + digit;
                    pointStarted = pointDigits = true;
                } else { return std::nullopt; }
                continue;
            }
            if (ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') {
                if (keywordLength != 0) {
                    if (!finishKeyword()) { return std::nullopt; }
                    keywordLength = 0;
                    if (ch == '\n') { state = ScanState::keyword; }
                }
            } else if (keywordLength < keyword.size()) {
                keyword[keywordLength++] = ch;
            } else {
                keywordLength = 0;
                state = ScanState::skipLine; // Longer keywords cannot describe geometry here.
            }
        }
        if (firstBlock && fileBytesHint && input.good()) {
            // Sampling decides only whether to continue this optional scan.
            // Never reject from an estimate: the parser and incremental mesh
            // checks remain authoritative even when row lengths vary later.
            const auto estimate = static_cast<long double>(positions) * static_cast<long double>(fileBytesHint)
                / static_cast<long double>(input.gcount());
            if (estimate <= std::numeric_limits<SceneBufferSize>::max() / sizeof(Vertex)) { return std::nullopt; }
        }
        firstBlock = false;
    }
    if (input.bad() || (input.fail() && !input.eof())) {
        throw std::runtime_error("Failed to read OBJ capacity preflight.");
    }
    if (keywordLength != 0 && !finishKeyword()) { return std::nullopt; }
    if (state == ScanState::points && !finishPoint()) { return std::nullopt; }
    if (hasPoints) { return std::nullopt; }
    return positions;
}

Mesh loadObjMeshLegacy(const std::filesystem::path& path, const ModelLoadProgressCallback& progress)
{
    reportModelLoadProgress(progress, ModelLoadStage::reading);
    preflightObjCapacity(path, progress);
    auto result = parseObj(path);
    std::vector<FreeformPatch> freeformPatches;
    if (result.error && objFreeformStatement(result.error.line)) {
        auto input = readObjFreeform(path, progress);
        std::istringstream stream(std::move(input.polygonText));
        result = rapidobj::ParseStream(stream, rapidobj::MaterialLibrary::SearchPath(
            std::filesystem::absolute(path).parent_path(), rapidobj::Load::Optional));
        freeformPatches = std::move(input.patches);
    }
    return buildObjMesh(std::move(result), std::move(freeformPatches), path, progress);
}

Mesh loadObjMeshTextLegacy(std::string_view text, const ModelLoadProgressCallback& progress)
{
    reportModelLoadProgress(progress, ModelLoadStage::reading);
    std::istringstream stream{std::string(text)};
    // An empty supplied library retains per-face material IDs for Triangulate,
    // unlike MaterialLibrary::Ignore, without consulting the filesystem.
    auto result = rapidobj::ParseStream(stream, rapidobj::MaterialLibrary::String(""));
    return buildObjMesh(std::move(result), {}, "<memory>", progress);
}

namespace {
template <typename Parse>
Mesh buildObjPrototype(Parse parse, const std::filesystem::path& path,
    const ModelLoadProgressCallback& progress, const ObjPrototypeOptions& options, ObjPrototypeMetrics* metrics)
{
    using Clock = std::chrono::steady_clock;
    if (metrics) { *metrics = {}; }
    reportModelLoadProgress(progress, ModelLoadStage::reading);
    std::vector<Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
    rapidobj::PrototypeOptions settings;
    settings.chunk_bytes = options.chunkBytes; settings.read_bytes = options.readBytes; settings.workers = options.workers;
    struct Checkpoint { const ModelLoadProgressCallback* callback; } checkpoint{&progress};
    settings.user = &checkpoint;
    settings.checkpoint = [](void* data) {
        reportModelLoadProgress(*static_cast<Checkpoint*>(data)->callback, ModelLoadStage::reading);
    };
    const auto start = Clock::now();
    auto parsed = parse(buffers, settings);
    const auto parsedAt = Clock::now();
    if (parsed.polygons.error) { return buildObjMesh(std::move(parsed.polygons), {}, path, progress); }
    auto patches = resolveObjFreeform(parsed, buffers, path, progress);
    const auto resolvedAt = Clock::now();
    if (metrics) {
        const auto& stats = parsed.stats;
        metrics->inputBytes = stats.input_bytes; metrics->chunks = stats.chunks; metrics->workers = stats.workers;
        metrics->peakInflightTextBytes = stats.peak_inflight_text_bytes; metrics->parsedChunkBytes = stats.parsed_chunk_bytes;
        metrics->positionCopyBytesAvoided = stats.position_copy_bytes_avoided;
        metrics->parseMs = std::chrono::duration<double, std::milli>(parsedAt - start).count();
        metrics->freeformMs = std::chrono::duration<double, std::milli>(resolvedAt - parsedAt).count();
    }
    // Numeric records and sparse weights are no longer needed during mesh
    // expansion; analytic patches now own their original-frame control data.
    parsed.statements = decltype(parsed.statements){}; parsed.weights = decltype(parsed.weights){};
    auto mesh = buildObjMesh<true>(std::move(parsed.polygons), std::move(patches), path, progress, &buffers);
    if (metrics) { metrics->prepareMs = std::chrono::duration<double, std::milli>(Clock::now() - resolvedAt).count(); }
    return mesh;
}
} // namespace

Mesh loadObjMeshPrototype(const std::filesystem::path& path, const ModelLoadProgressCallback& progress,
    const ObjPrototypeOptions& options, ObjPrototypeMetrics* metrics)
{
    preflightObjCapacity(path, progress);
    return buildObjPrototype([&](auto buffers, const auto& settings) {
        return rapidobj::ParseFilePrototype(path, buffers, settings);
    }, path, progress, options, metrics);
}

Mesh loadObjMeshTextPrototype(std::string_view text, const ModelLoadProgressCallback& progress,
    const ObjPrototypeOptions& options, ObjPrototypeMetrics* metrics)
{
    return buildObjPrototype([&](auto buffers, const auto& settings) {
        auto memorySettings = settings;
        memorySettings.material_names_only = true;
        return rapidobj::ParseMemoryPrototype(text, buffers, rapidobj::MaterialLibrary::String(""), memorySettings);
    }, "<memory>", progress, options, metrics);
}

Mesh loadObjMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress)
{
    return loadObjMeshPrototype(path, progress);
}

Mesh loadObjMeshText(std::string_view text, const ModelLoadProgressCallback& progress)
{
    return loadObjMeshTextPrototype(text, progress);
}

} // namespace woby
