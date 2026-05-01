#include "model_load.h"
#include "stl_mesh.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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

void writeBytes(const std::filesystem::path& path, const std::vector<unsigned char>& bytes)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
}

void loadStlAndDiscard(const std::filesystem::path& path)
{
    (void)woby::loadStlMesh(path);
}

void loadModelAndDiscard(const std::filesystem::path& path)
{
    (void)woby::loadModelMesh(path);
}

bool finiteMesh(const woby::Mesh& mesh)
{
    for (const auto& vertex : mesh.vertices) {
        for (const float value : vertex.position) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
        for (const float value : vertex.normal) {
            if (!std::isfinite(value)) {
                return false;
            }
        }
    }

    return std::isfinite(mesh.bounds.min[0])
        && std::isfinite(mesh.bounds.min[1])
        && std::isfinite(mesh.bounds.min[2])
        && std::isfinite(mesh.bounds.max[0])
        && std::isfinite(mesh.bounds.max[1])
        && std::isfinite(mesh.bounds.max[2])
        && std::isfinite(mesh.bounds.radius);
}

void writeGeneratedBinaryStl(
    const std::filesystem::path& path,
    std::mt19937& random,
    uint32_t triangleCount)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    std::array<char, 80> header{};
    const std::string headerText = "generated binary stl";
    for (size_t index = 0; index < headerText.size(); ++index) {
        header[index] = headerText[index];
    }
    stream.write(header.data(), static_cast<std::streamsize>(header.size()));
    writeUint32Le(stream, triangleCount);

    std::uniform_real_distribution<float> coordinate(-50.0f, 50.0f);
    for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
        writeFloat32Le(stream, 0.0f);
        writeFloat32Le(stream, 0.0f);
        writeFloat32Le(stream, 0.0f);
        for (size_t vertex = 0; vertex < 3u; ++vertex) {
            writeFloat32Le(stream, coordinate(random));
            writeFloat32Le(stream, coordinate(random));
            writeFloat32Le(stream, coordinate(random));
        }
        writeUint16Le(stream, 0u);
    }
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

TEST_CASE("model loader rejects unsupported extensions")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_unsupported_model.txt";
    writeBytes(path, {'n', 'o', 't', ' ', 'a', ' ', 'm', 'o', 'd', 'e', 'l'});

    CHECK_THROWS_WITH_AS(
        loadModelAndDiscard(path),
        ("Unsupported model file extension: " + path.string()).c_str(),
        std::runtime_error);

    std::filesystem::remove(path);
}

TEST_CASE("STL loader rejects explicit malformed ASCII and binary inputs")
{
    const std::filesystem::path asciiPath = std::filesystem::temp_directory_path()
        / "woby_malformed_ascii.stl";
    {
        std::ofstream stream(asciiPath, std::ios::trunc);
        stream << "solid bad\n";
        stream << "  facet normal 0 0 1\n";
        stream << "    outer loop\n";
        stream << "      vertex 0 0 0\n";
        stream << "      vertex 1 0 0\n";
        stream << "    endloop\n";
        stream << "  endfacet\n";
        stream << "endsolid bad\n";
    }

    CHECK_THROWS_WITH_AS(
        loadStlAndDiscard(asciiPath),
        "ASCII STL facet did not contain exactly 3 vertices.",
        std::runtime_error);

    const std::filesystem::path binaryPath = std::filesystem::temp_directory_path()
        / "woby_nonfinite_binary.stl";
    {
        std::ofstream stream(binaryPath, std::ios::binary | std::ios::trunc);
        std::array<char, 80> header{};
        stream.write(header.data(), static_cast<std::streamsize>(header.size()));
        writeUint32Le(stream, 1u);
        writeFloat32Le(stream, 0.0f);
        writeFloat32Le(stream, 0.0f);
        writeFloat32Le(stream, 1.0f);
        writeFloat32Le(stream, std::numeric_limits<float>::quiet_NaN());
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

    CHECK_THROWS_WITH_AS(
        loadStlAndDiscard(binaryPath),
        "STL contains a non-finite vertex coordinate.",
        std::runtime_error);

    std::filesystem::remove(asciiPath);
    std::filesystem::remove(binaryPath);
}

TEST_CASE("STL loader smoke-fuzzes generated byte files")
{
    std::mt19937 random(0x571f33u);
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_generated_bytes_fuzz.stl";

    for (size_t iteration = 0; iteration < 128u; ++iteration) {
        const size_t byteCount = std::uniform_int_distribution<size_t>(0u, 240u)(random);
        std::vector<unsigned char> bytes(byteCount);
        for (unsigned char& byte : bytes) {
            byte = static_cast<unsigned char>(std::uniform_int_distribution<int>(0, 255)(random));
        }

        writeBytes(path, bytes);

        try {
            const woby::Mesh mesh = woby::loadStlMesh(path);
            CHECK_FALSE(woby::empty(mesh));
            CHECK(mesh.indices.size() % 3u == 0u);
            CHECK(finiteMesh(mesh));
        } catch (const std::exception&) {
            CHECK(true);
        }
    }

    std::filesystem::remove(path);
}

TEST_CASE("STL loader accepts generated finite binary triangles")
{
    std::mt19937 random(0x5171b1u);
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / "woby_generated_binary_fuzz.stl";

    for (uint32_t triangleCount = 1u; triangleCount <= 16u; ++triangleCount) {
        writeGeneratedBinaryStl(path, random, triangleCount);

        const woby::Mesh mesh = woby::loadStlMesh(path);

        REQUIRE(mesh.nodes.size() == 1u);
        CHECK_FALSE(mesh.vertices.empty());
        CHECK(mesh.indices.size() == size_t(triangleCount) * 3u);
        CHECK(finiteMesh(mesh));
    }

    std::filesystem::remove(path);
}
