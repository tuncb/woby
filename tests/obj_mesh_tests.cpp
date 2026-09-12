#include "model_load.h"
#include "obj_mesh.h"
#include "utf8_path.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

struct ObjTestDirectory {
    std::filesystem::path path;

    ObjTestDirectory()
    {
        const auto prefix = "woby_obj_loader_"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t attempt = 0;; ++attempt) {
            path = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(attempt));
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

TEST_CASE("OBJ loader rejects files without renderable triangles")
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

    CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);

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

TEST_CASE("OBJ loader supports Unicode filenames")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / std::filesystem::path(u8"model_\u6a21\u578b.obj");
    writeText(path, "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    const auto mesh = woby::loadObjMesh(path);
    CHECK(mesh.indices.size() == 3u);
}

TEST_CASE("OBJ errors preserve Unicode filenames")
{
    const ObjTestDirectory fixture;
    const auto path = fixture.path / std::filesystem::path(u8"model_\u6a21\u578b.obj");
    SUBCASE("missing file") {}
    SUBCASE("malformed file") { writeText(path, "v invalid 0 0\n"); }
    SUBCASE("empty geometry") { writeText(path, "v 0 0 0\n"); }
    try {
        loadObjAndDiscard(path);
        FAIL("Expected an OBJ load error");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()).find(woby::pathToUtf8(path)) != std::string::npos);
    }
}
