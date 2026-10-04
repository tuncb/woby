#include "model_load.h"
#include "obj_mesh.h"
#include "utf8_path.h"
#include "background_load.h"
#include "control_scene.h"
#include "scene_dimensions.h"
#include "scene_history.h"
#include "scene_pick.h"
#include "ui_operations.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

struct ObjTestDirectory {
    std::filesystem::path path;

    ObjTestDirectory()
    {
        const auto prefix = "woby_obj_loader_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t attempt = 0;; ++attempt) {
            path = std::filesystem::absolute(std::filesystem::temp_directory_path()) / (prefix + "_" + std::to_string(attempt));
            if (std::filesystem::create_directory(path)) { break; }
        }
    }

    ~ObjTestDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void writeText(const std::filesystem::path& path, const char* text)
{
    std::ofstream stream(path, std::ios::trunc);
    stream << text;
}

bool containsTexcoord(const woby::Mesh& mesh, float u, float v)
{
    return std::any_of(mesh.vertices.begin(), mesh.vertices.end(), [u, v](const woby::Vertex& vertex) {
        return vertex.texcoord[0] == doctest::Approx(u)
            && vertex.texcoord[1] == doctest::Approx(v);
    });
}

void loadObjAndDiscard(const std::filesystem::path& path)
{
    (void)woby::loadObjMesh(path);
}

} // namespace

TEST_CASE("OBJ text import shares polygon construction with file import")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "memory-parity.obj";
    constexpr auto text = "o rebased_quad\n"
        "v 1000000000 -2 0\nv 1000000002 -2 0\n"
        "v 1000000002 2 0\nv 1000000000 2 0\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "f -4/1 -3/2 -2/3 -1/4\n";
    writeText(path, text);
    const auto fileMesh = woby::loadObjMesh(path);
    const auto memoryMesh = woby::loadObjMeshText(text);
    REQUIRE(memoryMesh.vertices.size() == 4);
    REQUIRE(memoryMesh.indices.size() == 6);
    CHECK(memoryMesh.indices == fileMesh.indices);
    CHECK(memoryMesh.origin == fileMesh.origin);
    CHECK(memoryMesh.origin[0] == 1000000001.0);
    CHECK(memoryMesh.precisePositions == fileMesh.precisePositions);
    REQUIRE(memoryMesh.sourceData);
    CHECK(memoryMesh.sourceData->points == fileMesh.sourceData->points);
    CHECK(memoryMesh.sourceData->indices == fileMesh.sourceData->indices);
    REQUIRE(memoryMesh.nodes.size() == 1);
    CHECK(memoryMesh.nodes[0].name == "rebased_quad");
    for (size_t i = 0; i < memoryMesh.vertices.size(); ++i) {
        CHECK(memoryMesh.vertices[i].position == fileMesh.vertices[i].position);
        CHECK(memoryMesh.vertices[i].normal == fileMesh.vertices[i].normal);
        CHECK(memoryMesh.vertices[i].texcoord == fileMesh.vertices[i].texcoord);
        CHECK(woby::validNormal(memoryMesh.vertices[i].normal));
    }
}

TEST_CASE("large OBJ parser failures preserve subsequent valid imports")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "large-invalid.obj";
    std::string invalid = "v not_a_number 0 0\n";
    SUBCASE("malformed vertex") {}
    SUBCASE("oversized line") { invalid = "#" + std::string(8192, 'x') + "\n"; }
    // Exceed the Windows 1 MiB stream fallback and span multiple parser blocks.
    std::string padding;
    const auto comment = "#" + std::string(126, 'x') + "\n";
    while (padding.size() < 2 * 1024 * 1024) { padding += comment; }
    writeText(path, (invalid + padding).c_str());
    const auto validPath = fixture.path / "large-valid.obj";
    writeText(validPath, ("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n" + padding).c_str());
    for (int repeat = 0; repeat < 3; ++repeat) {
        CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);
        const auto mesh = woby::loadObjMesh(validPath);
        CHECK(mesh.vertices.size() == 3);
        CHECK(mesh.indices.size() == 3);
    }
}

TEST_CASE("OBJ text import reports invalid input without filesystem fallback")
{
    CHECK_THROWS_AS((void)woby::loadObjMeshText(""), std::runtime_error);
    CHECK_THROWS_AS((void)woby::loadObjMeshText("v 0 0 0\nf 1 2 3\n"), std::runtime_error);
    CHECK_THROWS_AS((void)woby::loadObjMeshText("v not_a_number 0 0\n"), std::runtime_error);
}

TEST_CASE("OBJ loader creates one mesh node per shape")
{
    const ObjTestDirectory fixture;
    const auto& root = fixture.path;
    const std::filesystem::path path = root / "parts.obj";
    writeText(
        path,
        "o base\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n"
        "o raised\n"
        "v 0 0 1\n"
        "v 1 0 1\n"
        "v 0 1 1\n"
        "f 4 5 6\n");

    const woby::Mesh mesh = woby::loadObjMesh(path);

    REQUIRE(mesh.nodes.size() == 2u);
    CHECK(mesh.nodes[0].name == "base");
    CHECK(mesh.nodes[0].indexOffset == 0u);
    CHECK(mesh.nodes[0].indexCount == 3u);
    CHECK(mesh.nodes[1].name == "raised");
    CHECK(mesh.nodes[1].indexOffset == 3u);
    CHECK(mesh.nodes[1].indexCount == 3u);
    CHECK(mesh.indices.size() == 6u);
    CHECK(mesh.bounds.min[2] == doctest::Approx(0.0f));
    CHECK(mesh.bounds.max[2] == doctest::Approx(1.0f));
    for (const auto& vertex : mesh.vertices) {
        CHECK(woby::validNormal(vertex.normal));
    }

}

TEST_CASE("OBJ loader preserves texcoords with flipped V")
{
    const ObjTestDirectory fixture;
    const auto& root = fixture.path;
    const std::filesystem::path path = root / "textured.OBJ";
    writeText(
        path,
        "o textured\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0.25 0.25\n"
        "vt 0.50 0.75\n"
        "vt 1.00 0.00\n"
        "vn 0 0 1\n"
        "f 1/1/1 2/2/1 3/3/1\n");

    const woby::Mesh mesh = woby::loadModelMesh(path);

    REQUIRE(mesh.vertices.size() == 3u);
    CHECK(containsTexcoord(mesh, 0.25f, 0.75f));
    CHECK(containsTexcoord(mesh, 0.50f, 0.25f));
    CHECK(containsTexcoord(mesh, 1.00f, 1.00f));
    for (const auto& vertex : mesh.vertices) {
        CHECK(vertex.normal[2] == doctest::Approx(1.0f));
    }

}

TEST_CASE("OBJ loader accepts vertex-only point clouds")
{
    const ObjTestDirectory fixture;
    const auto& root = fixture.path;
    const std::filesystem::path path = root / "empty.obj";
    writeText(
        path,
        "o empty\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n");

    const auto mesh = woby::loadObjMesh(path);
    CHECK_FALSE(woby::empty(mesh));
    CHECK(mesh.indices.empty());
    CHECK(mesh.lineIndices.empty());
    CHECK(mesh.pointIndices == std::vector<uint32_t>{0, 1, 2});
    REQUIRE(mesh.nodes.size() == 1);
    CHECK(mesh.nodes[0].pointIndexCount == 3);
    CHECK(mesh.sourceData->indices.empty());
    CHECK(mesh.bounds.max == std::array<float, 3>{1, 1, 0});

}

TEST_CASE("OBJ UV availability distinguishes supplied zero coordinates from missing corners per part")
{
    const ObjTestDirectory fixture;
    const auto path = std::filesystem::absolute(fixture.path / "uv-parts.obj");
    writeText(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\n"
        "g complete\nf 1/1 2/1 3/1\ng missing\nf 1 2 3\n"
        "g partial\nf 1/1 2/1 3\ng reused\nf 1/1 2/1 3/1\n");
    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.nodes.size() == 4);
    CHECK(mesh.nodes[0].hasTexcoords);
    CHECK_FALSE(mesh.nodes[1].hasTexcoords);
    CHECK_FALSE(mesh.nodes[2].hasTexcoords);
    CHECK(mesh.nodes[3].hasTexcoords);
}

TEST_CASE("OBJ dense indexing grows across many seams without merging source identities")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "many-seams.obj";
    bool sparse = false;
    SUBCASE("primary positions and secondary seams") {}
    SUBCASE("primary lookup retains many unused source positions") { sparse = true; }
    {
        std::ofstream file(path);
        file << "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
        if (sparse) { for (int i = 0; i < 1000; ++i) { file << "v 99 99 99\n"; } }
        for (unsigned i = 0; i < 80; ++i) { file << "vn " << i << " 0 1\n"; }
        for (unsigned i = 1; i <= 80; ++i) {
            file << "o seam" << i << "\nf 1//" << i << " 2//" << i << " 3//" << i << '\n';
        }
        file << "o repeat\nf 1//1 2//1 3//1\n";
    }
    const auto loaded = woby::loadObjMesh(path);
    REQUIRE(loaded.vertices.size() == 240);
    REQUIRE(loaded.indices.size() == 243);
    REQUIRE(loaded.sourceData);
    CHECK(loaded.sourceData->points.size() == (sparse ? 1003u : 3u));
    REQUIRE(loaded.sourceData->indices.size() == loaded.indices.size());
    for (size_t i = 0; i < loaded.indices.size(); ++i) {
        CHECK(loaded.sourceData->indices[i] == i % 3);
        CHECK(loaded.vertices[loaded.indices[i]].normal[0] == static_cast<float>((i / 3) % 80));
    }
    CHECK(loaded.indices[0] == loaded.indices[240]);
}

TEST_CASE("OBJ capacity estimates preserve seam vertices and reuse references across shapes")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "seams.obj";
    writeText(path,
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\n"
        "vt 0.25 0.25\nvt 0.75 0.25\nvt 0.25 0.75\n"
        "vn 0 0 1\nvn 0 0 -1\n"
        "o front\nf 1/1/1 2/2/1 3/3/1\n"
        "o back\nf 1/4/2 3/6/2 2/5/2\n"
        "o repeated\nf 1/1/1 2/2/1 3/3/1\n");

    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.vertices.size() == 6u);
    REQUIRE(mesh.indices.size() == 9u);
    REQUIRE(mesh.nodes.size() == 3u);
    CHECK(mesh.nodes[0].name == "front");
    CHECK(mesh.nodes[1].name == "back");
    CHECK(mesh.nodes[2].name == "repeated");
    for (size_t i = 0; i < 3u; ++i) {
        CHECK(mesh.nodes[i].indexOffset == i * 3u);
        CHECK(mesh.nodes[i].indexCount == 3u);
        CHECK(mesh.indices[i] == mesh.indices[i + 6u]);
        CHECK(mesh.vertices[mesh.indices[i]].normal[2] == 1.0f);
        CHECK(mesh.vertices[mesh.indices[i + 3u]].normal[2] == -1.0f);
    }
    const auto& front = mesh.vertices[mesh.indices[0]];
    const auto& back = mesh.vertices[mesh.indices[3]];
    CHECK(front.position == back.position);
    CHECK(front.texcoord == std::array<float, 2>{0.0f, 1.0f});
    CHECK(back.texcoord == std::array<float, 2>{0.25f, 0.75f});
}

TEST_CASE("OBJ loader retains identical values with distinct source indices and reuses identical tuples")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "identical-values.obj";
    const char* secondFace = "f 4/4/2 2/2/1 3/3/1\n";
    SUBCASE("supplied normals") {}
    SUBCASE("partially missing normals") { secondFace = "f 4/4 2/2/1 3/3/1\n"; }
    const std::string text = std::string(
        "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 0\n"
        "vt 0 0\nvt 1 0\nvt 0 1\nvt 0 0\nvn 0 0 1\nvn 0 0 1\n"
        "g first\nf 1/1/1 2/2/1 3/3/1\ng duplicate\n")
        + secondFace + "g repeated\nf 1/1/1 2/2/1 3/3/1\n";
    writeText(path, text.c_str());

    const auto mesh = woby::loadObjMesh(path);

    REQUIRE(mesh.vertices.size() == 4u);
    CHECK(mesh.indices == std::vector<uint32_t>{0, 1, 2, 3, 1, 2, 0, 1, 2});
    CHECK(mesh.vertices[0].position == mesh.vertices[3].position);
    CHECK(mesh.vertices[0].normal == mesh.vertices[3].normal);
    CHECK(mesh.vertices[0].texcoord == mesh.vertices[3].texcoord);
    CHECK(mesh.vertices[0].normal == std::array<float, 3>{0, 0, 1});
    CHECK(mesh.vertices[0].texcoord == std::array<float, 2>{0, 1});
    REQUIRE(mesh.nodes.size() == 3u);
    CHECK(mesh.nodes[1].name == "duplicate");
    CHECK(mesh.nodes[2].name == "repeated");
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
        CHECK(mesh.nodes[i].indexOffset == i * 3u);
        CHECK(mesh.nodes[i].indexCount == 3u);
    }
    REQUIRE(mesh.sourceData);
    CHECK(mesh.sourceData->points.size() == 4u);
    CHECK(mesh.sourceData->indices == mesh.indices);
}

TEST_CASE("OBJ loader triangulates polygons while preserving area and winding")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "polygon.obj";
    const char* face = "f 1 2 3 4 5\n";
    const char* positions = "v 0 0 0\nv 3 0 0\nv 3 3 0\nv 2 1 0\nv 0 3 0\n";
    size_t u = 0u;
    size_t v = 1u;
    float winding = 1.0f;
    SUBCASE("XY plane") {
        SUBCASE("counterclockwise") {}
        SUBCASE("clockwise") { winding = -1.0f; }
    }
    SUBCASE("YZ plane") {
        positions = "v 0 0 0\nv 0 3 0\nv 0 3 3\nv 0 2 1\nv 0 0 3\n";
        u = 1u;
        v = 2u;
        SUBCASE("counterclockwise") {}
        SUBCASE("clockwise") { winding = -1.0f; }
    }
    SUBCASE("ZX plane") {
        positions = "v 0 0 0\nv 0 0 3\nv 3 0 3\nv 1 0 2\nv 3 0 0\n";
        u = 2u;
        v = 0u;
        SUBCASE("counterclockwise") {}
        SUBCASE("clockwise") { winding = -1.0f; }
    }
    if (winding < 0.0f) { face = "f 5 4 3 2 1\n"; }
    const std::string text = std::string("o concave\n") + positions + face;
    writeText(path, text.c_str());

    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.indices.size() == 9u);
    REQUIRE(mesh.nodes.size() == 1u);
    CHECK(mesh.nodes[0].name == "concave");
    CHECK(mesh.nodes[0].indexCount == 9u);
    float area = 0.0f;
    for (size_t i = 0; i < mesh.indices.size(); i += 3u) {
        const auto& a = mesh.vertices.at(mesh.indices[i]).position;
        const auto& b = mesh.vertices.at(mesh.indices[i + 1u]).position;
        const auto& c = mesh.vertices.at(mesh.indices[i + 2u]).position;
        const float signedArea = 0.5f * ((b[u] - a[u]) * (c[v] - a[v])
            - (b[v] - a[v]) * (c[u] - a[u]));
        CHECK(signedArea * winding > 0.0f);
        area += signedArea * winding;
    }
    // A fan from the first vertex incorrectly covers area 9 for this polygon.
    CHECK(area == doctest::Approx(6.0f));
}

TEST_CASE("OBJ loader resolves negative position normal and UV indices")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "relative-indices.obj";
    writeText(path,
        "v 0 0 0\nv 2 0 0\nv 0 2 0\n"
        "vt 0.25 0.75\nvt 0.5 0.5\nvt 1 0\nvn 0 0 -1\n"
        "g relative\nf -3/-3/-1 -1/-1/-1 -2/-2/-1\n");

    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.vertices.size() == 3u);
    REQUIRE(mesh.indices.size() == 3u);
    REQUIRE(mesh.nodes.size() == 1u);
    CHECK(mesh.nodes[0].name == "relative");
    CHECK(containsTexcoord(mesh, 0.25f, 0.25f));
    CHECK(containsTexcoord(mesh, 0.5f, 0.5f));
    CHECK(containsTexcoord(mesh, 1.0f, 1.0f));
    CHECK(mesh.bounds.max == std::array<float, 3>{2.0f, 2.0f, 0.0f});
    for (const auto& vertex : mesh.vertices) {
        CHECK(vertex.normal[2] == doctest::Approx(-1.0f));
    }
}

TEST_CASE("OBJ loader imports geometry when a material library is missing")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "missing-material.obj";
    writeText(path,
        "mtllib absent.mtl\nusemtl absent\n"
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3 4\n");

    const auto mesh = woby::loadObjMesh(path);
    CHECK(mesh.vertices.size() == 4u);
    CHECK(mesh.indices.size() == 6u);
    REQUIRE(mesh.nodes.size() == 1u);
    CHECK(mesh.nodes[0].name == "shape 1");
}

TEST_CASE("OBJ loader rejects invalid face indices")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "invalid-index.obj";
    const char* face = "f 1 2 4\n";
    SUBCASE("position out of range") {}
    SUBCASE("negative position out of range") { face = "f -4 -2 -1\n"; }
    SUBCASE("zero position") { face = "f 0 2 3\n"; }
    SUBCASE("normal out of range") { face = "f 1//1 2//1 3//1\n"; }
    SUBCASE("UV out of range") { face = "f 1/1 2/1 3/1\n"; }
    const std::string text = std::string("v 0 0 0\nv 1 0 0\nv 0 1 0\n") + face;
    writeText(path, text.c_str());
    CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);
}

TEST_CASE("OBJ parse errors identify the file and source line")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "malformed.obj";
    writeText(path, "v 0 0 0\nv 1 0 0\nv invalid 1 0\nf 1 2 3\n");
    try {
        loadObjAndDiscard(path);
        FAIL("Expected an OBJ parse error");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        CHECK(message.find(path.string()) != std::string::npos);
        CHECK(message.find("line 3") != std::string::npos);
    }
}

TEST_CASE("OBJ loader reports missing files")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "absent.obj";
    CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);
}

TEST_CASE("buffered OBJ reads preserve optional materials beside the model independently of the working directory")
{
    const ObjTestDirectory fixture;
    const auto folder = fixture.path / "nested";
    std::filesystem::create_directory(folder);
    const auto path = folder / "material.obj";
    writeText(path, "mtllib local.mtl\nusemtl named\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    SUBCASE("valid adjacent material") {
        writeText(folder / "local.mtl", "newmtl named\nKd 1 0 0\n");
        CHECK(woby::loadObjMesh(path).indices.size() == 3);
    }
    SUBCASE("malformed optional material does not block geometry") {
        writeText(folder / "local.mtl", "newmtl named\nKd broken 0 0\n");
        CHECK(woby::loadObjMesh(path).indices.size() == 3);
    }
}

TEST_CASE("small buffered and large parallel OBJ reads preserve the same geometry")
{
    const ObjTestDirectory fixture;
    const auto small = fixture.path / "small.obj", large = fixture.path / "large.obj";
    const char* text = "o face\nv 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
    writeText(small, text);
    {
        std::ofstream file(large, std::ios::binary);
        file << text;
        const auto padding = std::string(1023, '#') + '\n';
        for (int i = 0; i < 1100; ++i) { file << padding; }
    }
    const auto a = woby::loadObjMesh(small), b = woby::loadObjMesh(large);
    REQUIRE(a.vertices.size() == b.vertices.size());
    CHECK(a.indices == b.indices);
    CHECK(a.sourceData->indices == b.sourceData->indices);
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        CHECK(a.vertices[i].position == b.vertices[i].position);
        CHECK(a.vertices[i].normal == b.vertices[i].normal);
        CHECK(a.vertices[i].texcoord == b.vertices[i].texcoord);
    }
}

TEST_CASE("OBJ loader supports Unicode filenames")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / std::filesystem::path(u8"model_\u6a21\u578b.obj");
    writeText(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto mesh = woby::loadObjMesh(path);
    CHECK(mesh.indices.size() == 3u);
}

TEST_CASE("OBJ position lookup preserves first use across shapes and unused positions")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "first-use.obj";
    std::string attributes;
    std::string faces = "g first\nf 4 2 3\ng second\nf 3 2 4\n";
    bool sparse = false;
    SUBCASE("position-only direct lookup") {}
    SUBCASE("unused attributes prevent the position-only shortcut") {
        attributes = "vn 0 0 1\nvt 0 0\n";
    }
    SUBCASE("normal tuples share their primary position entry") {
        attributes = "vn 0 0 1\n";
        faces = "g first\nf 4//1 2//1 3//1\ng second\nf 3//1 2//1 4//1\n";
    }
    SUBCASE("position-only lookup retains many unused source positions") { sparse = true; }
    std::string text = "v 99 99 99\nv 1 0 0\nv 0 1 0\nv 0 0 0\n";
    if (sparse) { for (int i = 0; i < 100; ++i) { text += "v 99 99 99\n"; } }
    text += attributes + faces;
    writeText(path, text.c_str());
    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.sourceData);
    CHECK(sizeof(mesh.sourceData->points[0]) == 3 * sizeof(double));
    CHECK(mesh.sourceData->points.size() == (sparse ? 104u : 4u));
    CHECK(mesh.sourceData->indices == std::vector<uint32_t>{3,1,2,2,1,3});
    CHECK(mesh.indices == std::vector<uint32_t>{0,1,2,2,1,0});
    REQUIRE(mesh.vertices.size() == 3);
    CHECK(mesh.vertices[0].position == std::array<float,3>{0,0,0});
    CHECK(mesh.vertices[1].position == std::array<float,3>{1,0,0});
    CHECK(mesh.vertices[2].position == std::array<float,3>{0,1,0});
    REQUIRE(mesh.nodes.size() == 2);
    CHECK(mesh.nodes[1].indexOffset == 3);
    CHECK(mesh.nodes[1].indexCount == 3);
}

TEST_CASE("OBJ errors preserve Unicode filenames")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / std::filesystem::path(u8"model_\u6a21\u578b.obj");
    SUBCASE("missing file") {}
    SUBCASE("malformed file") { writeText(path, "v invalid 0 0\n"); }
    SUBCASE("empty geometry") { writeText(path, "# no geometry\n"); }
    try {
        loadObjAndDiscard(path);
        FAIL("Expected an OBJ load error");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find(woby::pathToUtf8(path)) != std::string::npos);
    }
}


TEST_CASE("OBJ polylines preserve connectivity negative indices and separate groups")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "lines.obj";
    writeText(path, "v 0 0 0\nv 2 0 0\nv 2 3 0\nv 0 3 0\nvt 0 0\nvt 1 0\n"
        "g boundary\nl 1/1 2/2 3/1\nl -1 -4\ng diagonal\nl 2 4\n");
    const auto mesh = woby::loadObjMesh(path);
    CHECK_FALSE(woby::empty(mesh));
    CHECK(mesh.indices.empty());
    CHECK(mesh.pointIndices.empty());
    CHECK(mesh.lineIndices == std::vector<uint32_t>{0,1,1,2,3,0,1,3});
    REQUIRE(mesh.nodes.size() == 2);
    CHECK(mesh.nodes[0].name == "boundary");
    CHECK(mesh.nodes[0].lineIndexCount == 6);
    CHECK(mesh.nodes[1].name == "diagonal");
    CHECK(mesh.nodes[1].lineIndexOffset == 6);
    CHECK(mesh.nodes[1].lineIndexCount == 2);
    CHECK_FALSE(mesh.nodes[0].hasTexcoords);
    CHECK(mesh.sourceData->indices.empty());
    CHECK(woby::originalMeshBounds(mesh)[1] == woby::Coordinate{2,3,0});
}

TEST_CASE("OBJ points select explicit vertices and preserve source coordinates")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "points.obj";
    writeText(path, "v 1000000000 0 0\nv 1000000000.125 1 0\nv 1000000005 9 0\n"
        "g samples\np 2 -3\ng shared\np 1\n");
    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.nodes.size() == 2);
    CHECK(mesh.nodes[0].name == "samples");
    CHECK(mesh.nodes[0].pointIndexCount == 2);
    CHECK(mesh.nodes[1].pointIndexOffset == 2);
    CHECK(mesh.pointIndices == std::vector<uint32_t>{0,1,1});
    CHECK(mesh.vertices.size() == 2);
    CHECK(mesh.sourceData->points.size() == 3);
    CHECK(woby::originalMeshBounds(mesh)[1] == woby::Coordinate{1000000000.125,1,0});
    CHECK(woby::originalMeshBounds(mesh, &mesh.nodes[1])[0] == woby::Coordinate{1000000000,0,0});
}

TEST_CASE("Mixed OBJ keeps face groups first and preserves normals and triangle provenance")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "mixed.obj";
    writeText(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 2 2 0\nv 90 90 90\n"
        "vn 0 1 0\ng mixed\nf 1//1 2//1 3//1\nl 1 4\np 4\n"
        "g mixed (lines)\nf 1//1 2//1 3//1\n");
    const auto mesh = woby::loadObjMesh(path);
    REQUIRE(mesh.nodes.size() == 4);
    CHECK(mesh.nodes[0].name == "mixed");
    CHECK(mesh.nodes[1].name == "mixed (lines)");
    CHECK(mesh.nodes[2].name == "mixed (lines) (2)");
    CHECK(mesh.nodes[3].name == "mixed (points)");
    CHECK(mesh.indices.size() == 6);
    CHECK(mesh.lineIndices.size() == 2);
    CHECK(mesh.pointIndices.size() == 1);
    CHECK(mesh.nodes[2].indexCount == 0);
    CHECK(mesh.nodes[3].indexCount == 0);
    CHECK(mesh.sourceData->indices == std::vector<uint32_t>{0,1,2,0,1,2});
    for (const auto i : mesh.indices) { CHECK(mesh.vertices[i].normal == std::array<float,3>{0,1,0}); }
    CHECK(mesh.bounds.max == std::array<float,3>{2,2,0});
}

TEST_CASE("OBJ rejects empty geometry and malformed point or line references")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "invalid.obj";
    for (const auto* primitive : {"p 0\n", "p 4\n", "p -4\n", "l 0 1\n", "l 1 4\n", "l -4 -1\n", "l 1\n"}) {
        CAPTURE(primitive);
        const std::string text = std::string("v 0 0 0\nv 1 0 0\nv 0 1 0\n") + primitive;
        writeText(path, text.c_str());
        CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);
    }
    writeText(path, "# empty\n");
    CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);
}

TEST_CASE("OBJ point and line parts support picking dimensions and appearance controls")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "picking.obj";
    writeText(path, "v 0 0 .4\nv .8 0 .4\nv -.8 -.5 .4\nv .8 -.5 .4\n"
        "g samples\np 1 2\ng wire\nl 3 4\n");
    woby::UiState state;
    state.files.push_back(woby::createUiFileState(path, woby::loadObjMesh(path), 0));
    woby::appendDefaultSceneNodesForFiles(state, 0);
    woby::assignSceneObjectIds(state);
    REQUIRE(state.files[0].groupSettings.size() == 2);
    const auto points = state.files[0].groupSettings[0].objectId;
    const auto lines = state.files[0].groupSettings[1].objectId;
    CHECK(state.files[0].groupSettings[0].showVertices);
    woby::ScenePickView view;
    bx::mtxIdentity(view.view.data()); bx::mtxIdentity(view.projection.data());
    view.width = view.height = 100;
    CHECK(woby::pickSceneObject(woby::scenePickParts(state), view, {50,50}) == points);
    CHECK(woby::pickSceneObject(woby::scenePickParts(state), view, {50,75}) == lines);
    CHECK(woby::pickSceneObject(woby::scenePickParts(state), view, {70,50}) == woby::invalidSceneObjectId);
    CHECK(woby::comparisonObjectParts(state, {state.files[0].objectId}).empty());
    woby::selectSceneObject(state, points);
    const auto parts = woby::scenePickParts(state);
    const auto dimensions = woby::sceneDimensions(parts);
    REQUIRE(dimensions);
    CHECK(dimensions->lengths[0] == doctest::Approx(.8));
    CHECK(woby::sceneSelectionLines(parts).size() == 24);
    CHECK_FALSE(woby::selectedObjectProperty(state, woby::UiObjectProperty::solidMesh).available);
    CHECK_FALSE(woby::selectedObjectProperty(state, woby::UiObjectProperty::lineWidth).available);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::vertices, 0);
    CHECK(woby::pickSceneObject(woby::scenePickParts(state), view, {50,50}) == woby::invalidSceneObjectId);
    woby::resetSelectedObjectProperties(state, woby::UiPropertyGroup::appearance);
    CHECK(state.files[0].groupSettings[0].showVertices);
    CHECK(woby::pickSceneObject(woby::scenePickParts(state), view, {50,50}) == points);
    const auto details = woby::controlObjectDetails(state, points, [](auto id) { return std::to_string(id); });
    CHECK(details.at("primitive") == "points");
    CHECK(details.at("pointCount") == 2);
    CHECK(woby::controlSceneInfo(state).at("pointCount") == 2);
}

TEST_CASE("OBJ mixed primitive appearance survives scene loading views and undo")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "mixed.obj";
    writeText(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\ng mixed\nf 1 2 3\nl 1 2\np 3\n");
    auto batch = woby::loadModelBatchCpu({path}, 0, {}, {});
    REQUIRE(batch.files.size() == 1);
    woby::UiState state; state.files = std::move(batch.files);
    woby::appendDefaultSceneNodesForFiles(state, 0);
    const auto clean = woby::createSceneDocument(state);
    woby::SceneHistory history; woby::resetSceneHistory(history, state);
    const auto points = state.files[0].groupSettings[2].objectId;
    const auto lines = state.files[0].groupSettings[1].objectId;
    woby::selectSceneObject(state, points);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::vertexSize, 3);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::red, .7f);
    REQUIRE(woby::setObjectLineStyle(state, {lines}, 8.0f, false));
    const auto changed = woby::createSceneDocument(state);
    CHECK(changed.files[0].groups[2].pointGroup);
    CHECK(changed.files[0].groups[1].lineGroup);
    REQUIRE(woby::recordSceneHistory(history, state));
    auto undo = woby::prepareSceneHistoryStep(history, state, clean, false); REQUIRE(undo);
    woby::commitSceneHistoryStep(history, state, std::move(*undo), false);
    CHECK(woby::createSceneDocument(state) == clean);
    auto redo = woby::prepareSceneHistoryStep(history, state, clean, true); REQUIRE(redo);
    woby::commitSceneHistoryStep(history, state, std::move(*redo), true);
    CHECK(woby::createSceneDocument(state) == changed);
    const auto view = woby::createView(state);
    woby::setSelectedObjectProperty(state, woby::UiObjectProperty::vertices, 0);
    woby::applyView(state, view);
    CHECK(state.files[0].groupSettings[2].showVertices);
    const auto saved = woby::createSceneDocument(state);
    const auto scenePath = fixture.path / "points.woby";
    woby::writeSceneDocument(scenePath, saved);
    auto loaded = woby::loadSceneCpu(scenePath, {}, {});
    auto restored = woby::prepareSceneReplacement(state, std::move(loaded.files), loaded.document);
    CHECK(woby::createSceneDocument(restored) == saved);
    auto mismatched = saved.files[0]; mismatched.groups[2].pointGroup = false;
    CHECK_THROWS_AS(woby::applySceneFileRecord(restored.files[0], mismatched), std::runtime_error);
}


TEST_CASE("OBJ non-triangle import stays cancellable after an odd number of face indices")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / "cancel.obj";
    const char* primitive = "l 1 2\n";
    SUBCASE("polylines") {}
    SUBCASE("points") { primitive = "p 1\n"; }
    {
        std::ofstream stream(path);
        stream << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        for (size_t i = 0; i < 20000; ++i) { stream << primitive; }
    }
    bool canceled = false;
    woby::ImportCallbacks callbacks;
    callbacks.stageProgress = [&](const woby::ModelLoadProgress& progress) {
        if (progress.stage == woby::ModelLoadStage::buildingMesh
            && progress.completed > 3 && progress.completed < progress.total) { canceled = true; }
    };
    callbacks.canceled = [&] { return canceled; };
    const auto loaded = woby::loadModel(path, {}, callbacks);
    CHECK(canceled);
    CHECK(loaded.canceled);
    CHECK(woby::empty(loaded.mesh));
}
