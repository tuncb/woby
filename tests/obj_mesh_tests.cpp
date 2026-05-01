#include "model_load.h"
#include "obj_mesh.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {

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
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_obj_loader_shapes";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
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

    std::filesystem::remove_all(root);
}

TEST_CASE("OBJ loader preserves texcoords with flipped V")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_obj_loader_texcoords";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
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

    std::filesystem::remove_all(root);
}

TEST_CASE("OBJ loader rejects files without renderable triangles")
{
    const std::filesystem::path root = std::filesystem::temp_directory_path()
        / "woby_obj_loader_empty";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::filesystem::path path = root / "empty.obj";
    writeText(
        path,
        "o empty\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n");

    CHECK_THROWS_AS(loadObjAndDiscard(path), std::runtime_error);

    std::filesystem::remove_all(root);
}
