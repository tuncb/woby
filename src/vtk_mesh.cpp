#include "vtk_mesh.h"
#include "importer_host.h"
#include "utf8_path.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <stdexcept>
#include <string>
#include <string_view>

namespace woby {
namespace {
constexpr uint64_t maxBytes = 4ull * 1024 * 1024 * 1024;
std::string token(std::istream& input)
{
    std::string value;
    if (!(input >> std::setw(257) >> value) || value.size() > 256) { throw std::runtime_error("Invalid or truncated VTK record."); }
    return value;
}
uint32_t integer(std::istream& input)
{
    const auto value = token(input);
    uint32_t number = 0;
    const auto result = std::from_chars(value.data(), value.data()+value.size(), number);
    if (result.ec != std::errc{} || result.ptr != value.data()+value.size()) { throw std::runtime_error("Invalid VTK count or point index."); }
    return number;
}
double real(std::istream& input)
{
    const auto value = token(input);
    std::string_view numeric = value;
    if (numeric.starts_with('+')) { numeric.remove_prefix(1); }
    double number = 0;
    const auto result = std::from_chars(numeric.data(), numeric.data()+numeric.size(), number);
    if (result.ec != std::errc{} || result.ptr != numeric.data()+numeric.size() || !std::isfinite(number) || std::abs(number) > 1e9) {
        throw std::runtime_error("VTK coordinates must be finite and within [-1e9, 1e9].");
    }
    return number;
}
void expect(std::istream& input, std::string_view expected)
{
    if (token(input) != expected) { throw std::runtime_error("Expected VTK " + std::string(expected) + " record."); }
}
std::string line(std::istream& input)
{
    std::string result;
    // Limit header records without allocating an unbounded line.
    char c;
    while (input.get(c)) {
        if (c == '\n') { break; }
        if (result.size() >= 256) { throw std::runtime_error("VTK header exceeds 256 characters."); }
        result.push_back(c);
    }
    while (!result.empty() && (result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) { result.pop_back(); }
    return result;
}
void integerType(std::istream& input)
{
    const auto type = token(input);
    if (type != "vtktypeint32" && type != "vtktypeint64" && type != "int" && type != "long" && type != "long_long") {
        throw std::runtime_error("Unsupported VTK cell integer type: " + type);
    }
}
}

Mesh loadVtkMesh(std::istream& input, const ModelLoadProgressCallback& progress, bool deferPreparation)
{
    reportModelLoadProgress(progress, ModelLoadStage::reading);
    const auto header = line(input);
    constexpr std::string_view prefix = "# vtk DataFile Version ";
    if (!header.starts_with(prefix)) { throw std::runtime_error("Expected a legacy VTK file header. XML VTU files are not supported."); }
    const auto version = std::string_view(header).substr(prefix.size());
    if (version != "2.0" && version != "3.0" && version != "4.2" && version != "5.1") {
        throw std::runtime_error("Unsupported legacy VTK version: " + std::string(version));
    }
    (void)line(input); // Title does not create a scene group.
    if (line(input) != "ASCII") { throw std::runtime_error("Only ASCII VTK files are supported. Convert binary files to ASCII."); }
    expect(input,"DATASET"); expect(input,"UNSTRUCTURED_GRID"); expect(input,"POINTS");
    const auto pointCount = integer(input);
    const auto pointType = token(input);
    if (pointType != "float" && pointType != "double") { throw std::runtime_error("VTK POINTS must use float or double coordinates."); }
    if (uint64_t(pointCount) * sizeof(WobyImportVertex) > maxBytes) { throw std::runtime_error("VTK points exceed supported size."); }
    std::vector<WobyImportVertex> vertices;
    for (uint32_t i = 0; i < pointCount; ++i) {
        if (i % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading, i, pointCount); }
        WobyImportVertex vertex{};
        for (auto& coordinate : vertex.position) { coordinate = real(input); }
        vertices.push_back(vertex);
    }
    expect(input,"CELLS");
    const auto firstCount = integer(input), connectivitySize = integer(input);
    if (uint64_t(firstCount) + connectivitySize > maxBytes / sizeof(uint32_t)) { throw std::runtime_error("VTK cells exceed supported size."); }
    std::vector<uint32_t> offsets{0}, connectivity;
    uint32_t cellCount = firstCount;
    if (version == "5.1") {
        if (!firstCount) { throw std::runtime_error("VTK CELLS requires at least one offset."); }
        cellCount = firstCount-1;
        expect(input,"OFFSETS"); integerType(input);
        offsets.clear();
        for (uint32_t i = 0; i < firstCount; ++i) {
            if (i % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading, i, firstCount); }
            const auto offset = integer(input);
            if (offset > connectivitySize || (i == 0 && offset != 0) || (i && offset <= offsets.back())) {
                throw std::runtime_error("Invalid VTK cell offsets.");
            }
            offsets.push_back(offset);
        }
        if (offsets.back() != connectivitySize) { throw std::runtime_error("VTK offsets do not cover the connectivity buffer."); }
        expect(input,"CONNECTIVITY"); integerType(input);
        for (uint32_t i = 0; i < connectivitySize; ++i) {
            if (i % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading, i, connectivitySize); }
            const auto index = integer(input);
            if (index >= pointCount) { throw std::runtime_error("VTK point index is outside the POINTS buffer."); }
            connectivity.push_back(index);
        }
    } else {
        uint64_t consumed = 0;
        for (uint32_t i = 0; i < cellCount; ++i) {
            if (i % 4096 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading, i, cellCount); }
            const auto count = integer(input);
            consumed += uint64_t(count)+1;
            if (!count || consumed > connectivitySize) { throw std::runtime_error("Invalid VTK cell list size."); }
            for (uint32_t n = 0; n < count; ++n) {
                if (n % 16384 == 0) { reportModelLoadProgress(progress, ModelLoadStage::reading, i, cellCount); }
                const auto index = integer(input);
                if (index >= pointCount) { throw std::runtime_error("VTK point index is outside the POINTS buffer."); }
                connectivity.push_back(index);
            }
            offsets.push_back(static_cast<uint32_t>(connectivity.size()));
        }
        if (consumed != connectivitySize) { throw std::runtime_error("VTK CELLS size does not match its contents."); }
    }
    expect(input,"CELL_TYPES");
    if (integer(input) != cellCount) { throw std::runtime_error("VTK CELL_TYPES count must match CELLS."); }
    std::vector<uint32_t> triangles, lines, points;
    std::vector<WobyImportTetrahedron> cells;
    const auto budget = [&] {
        const uint64_t bytes = vertices.size()*sizeof(WobyImportVertex) + cells.size()*sizeof(WobyImportTetrahedron)
            + (triangles.size()+lines.size()+points.size()+connectivity.size()+offsets.size())*sizeof(uint32_t);
        if (bytes > maxBytes || triangles.size() > UINT32_MAX || lines.size() > UINT32_MAX || points.size() > UINT32_MAX) {
            throw std::runtime_error("VTK geometry exceeds supported size.");
        }
    };
    for (uint32_t i = 0; i < cellCount; ++i) {
        if (i % 4096 == 0) { reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, i, cellCount); }
        const auto type = integer(input);
        const auto count = offsets[i+1]-offsets[i];
        const auto* n = connectivity.data()+offsets[i];
        const auto require = [&](uint32_t expected) {
            if (count != expected) { throw std::runtime_error("Wrong node count for VTK cell type " + std::to_string(type) + "."); }
        };
        switch (type) {
        case 1: require(1); points.push_back(n[0]); break; // VTK_VERTEX
        case 2: // VTK_POLY_VERTEX
            for (uint32_t p = 0; p < count; ++p) { points.push_back(n[p]); if (p % 16384 == 0) { budget(); reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, i, cellCount); } }
            break;
        case 3: require(2); lines.insert(lines.end(),n,n+2); break;
        case 4: // VTK_POLY_LINE
            if (count < 2) { throw std::runtime_error("VTK polylines require at least two nodes."); }
            for (uint32_t p = 1; p < count; ++p) { lines.insert(lines.end(),{n[p-1],n[p]}); if (p % 16384 == 0) { budget(); reportModelLoadProgress(progress, ModelLoadStage::buildingMesh, i, cellCount); } }
            break;
        case 5: require(3); triangles.insert(triangles.end(),n,n+3); break;
        case 9: require(4); triangles.insert(triangles.end(),{n[0],n[1],n[2],n[0],n[2],n[3]}); break;
        case 22: require(6); triangles.insert(triangles.end(),{n[0],n[3],n[5],n[3],n[1],n[4],n[5],n[4],n[2],n[3],n[4],n[5]}); break;
        case 10: case 24: {
            require(type == 10 ? 4u : 10u);
            WobyImportTetrahedron cell{}; cell.node_count = count;
            std::copy(n,n+count,cell.nodes); cells.push_back(cell); break;
        }
        default: throw std::runtime_error("Unsupported VTK cell type " + std::to_string(type) + ".");
        }
        budget();
    }
    // Attribute arrays do not affect geometry. Ignore the tail only after a
    // recognized attribute section; unknown geometry sections are errors.
    input >> std::ws;
    if (!input.eof()) {
        const auto tail = token(input);
        if (tail != "POINT_DATA" && tail != "CELL_DATA" && tail != "FIELD") { throw std::runtime_error("Unsupported VTK section: " + tail); }
    }
    if (input.bad()) { throw std::runtime_error("Cannot read VTK input."); }
    WobyImportResult result{};
    result.struct_size = sizeof(result); result.vertices = vertices.data(); result.vertex_count = pointCount;
    result.indices = triangles.data(); result.index_count = static_cast<uint32_t>(triangles.size());
    const WobyImportLines importedLines{sizeof(WobyImportLines),lines.data(),static_cast<uint32_t>(lines.size()),nullptr,0};
    const WobyImportPoints importedPoints{sizeof(WobyImportPoints),points.data(),static_cast<uint32_t>(points.size()),nullptr,0};
    const WobyImportTetrahedra tetrahedra{sizeof(WobyImportTetrahedra),cells.data(),static_cast<uint32_t>(cells.size()),nullptr,0};
    return copyImportedMesh(result,nullptr,&importedLines,nullptr,&importedPoints,nullptr,progress,deferPreparation,&tetrahedra);
}

Mesh loadVtkMesh(const std::filesystem::path& path, const ModelLoadProgressCallback& progress, bool deferPreparation)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw std::runtime_error("Cannot open VTK file: " + pathToUtf8(path)); }
    return loadVtkMesh(input,progress,deferPreparation);
}
}
