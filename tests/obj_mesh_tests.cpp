#include "model_load.h"
#include "obj_mesh.h"

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
