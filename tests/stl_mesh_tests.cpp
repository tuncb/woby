#include "model_load.h"
#include "stl_mesh.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void writeFloat32Le(std::ofstream& stream, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    stream.put(static_cast<char>(bits & 0xffu));
    stream.put(static_cast<char>((bits >> 8u) & 0xffu));
    stream.put(static_cast<char>((bits >> 16u) & 0xffu));
    stream.put(static_cast<char>((bits >> 24u) & 0xffu));
}

void writeUint32Le(std::ofstream& stream, uint32_t value)
{
    stream.put(static_cast<char>(value & 0xffu));
    stream.put(static_cast<char>((value >> 8u) & 0xffu));
    stream.put(static_cast<char>((value >> 16u) & 0xffu));
    stream.put(static_cast<char>((value >> 24u) & 0xffu));
}

void writeUint16Le(std::ofstream& stream, uint16_t value)
{
    stream.put(static_cast<char>(value & 0xffu));
    stream.put(static_cast<char>((value >> 8u) & 0xffu));
}

void writeAsciiTriangleStl(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << "solid named_part\n";
    stream << "  facet normal 0 0 1\n";
    stream << "    outer loop\n";
    stream << "      vertex 0 0 0\n";
    stream << "      vertex 1 0 0\n";
    stream << "      vertex 0 1 0\n";
    stream << "    endloop\n";
    stream << "  endfacet\n";
    stream << "endsolid named_part\n";
}

void writeBinaryTriangleStl(const std::filesystem::path& path, const std::string& headerPrefix)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    std::array<char, 80> header{};
    for (size_t index = 0; index < headerPrefix.size() && index < header.size(); ++index) {
        header[index] = headerPrefix[index];
    }
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    writeUint32Le(stream, 1u);

    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 1.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 1.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 0.0f);
    writeFloat32Le(stream, 1.0f);
    writeFloat32Le(stream, 0.0f);
    writeUint16Le(stream, 0u);
}

} // namespace

TEST_CASE("ASCII STL loader creates one named mesh group")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_ascii_triangle.stl";
    writeAsciiTriangleStl(path);

    const woby::Mesh mesh = woby::loadStlMesh(path);

    REQUIRE(mesh.vertices.size() == 3u);
    REQUIRE(mesh.indices.size() == 3u);
    REQUIRE(mesh.nodes.size() == 1u);
    CHECK(mesh.nodes[0].name == "named_part");
    CHECK(mesh.vertices[0].normal[2] == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max[0] == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max[1] == doctest::Approx(1.0f));

    std::filesystem::remove(path);
}

TEST_CASE("binary STL loader uses exact binary size detection")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_binary_triangle.stl";
    writeBinaryTriangleStl(path, "solid binary header");

    const woby::Mesh mesh = woby::loadStlMesh(path);

    REQUIRE(mesh.vertices.size() == 3u);
    REQUIRE(mesh.indices.size() == 3u);
    REQUIRE(mesh.nodes.size() == 1u);
    CHECK(mesh.nodes[0].name == "woby_binary_triangle");
    CHECK(mesh.vertices[1].position[0] == doctest::Approx(1.0f));
    CHECK(mesh.vertices[2].normal[2] == doctest::Approx(1.0f));

    std::filesystem::remove(path);
}

TEST_CASE("model loader dispatches STL by extension")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_dispatch_triangle.STL";
    writeAsciiTriangleStl(path);

    const woby::Mesh mesh = woby::loadModelMesh(path);

    REQUIRE(mesh.nodes.size() == 1u);
    CHECK(mesh.indices.size() == 3u);

    std::filesystem::remove(path);
}
