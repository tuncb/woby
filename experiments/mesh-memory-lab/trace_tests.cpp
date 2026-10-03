#include "trace.h"
#include "ui_state.h"
#include "obj_mesh.h"
#include <doctest/doctest.h>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace {
struct Fixture {
    std::filesystem::path root;
    Fixture()
    {
        const auto prefix = "mesh_memory_lab_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0;; ++i) {
            root = std::filesystem::temp_directory_path() / (prefix + "_" + std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path write(const std::string& source)
    {
        const auto path = root / "input.obj";
        std::ofstream file(path, std::ios::binary); file << source; file.close();
        return path;
    }
};
mesh_lab::Trace sample(const char* name)
{
    // Test inputs and all derived files share a unique temporary root.
    Fixture fixture;
    std::ifstream file(std::filesystem::path(MESH_LAB_SAMPLE_DIRECTORY) / name, std::ios::binary);
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    return mesh_lab::loadTrace(fixture.write(source));
}
}

TEST_CASE("UV seam preserves source identity while splitting render tuples")
{
    const auto trace = sample("uv-seam.obj");
    CHECK(trace.positions.size() == 4);
    CHECK(trace.triangles.size() == 2);
    CHECK(trace.mesh.vertices.size() == 6);
    CHECK(trace.splitPositions == 2);
    CHECK(trace.positionVertices[0] == std::vector<uint32_t>{0, 3});
    CHECK(trace.vertexKeys[0].position == trace.vertexKeys[3].position);
    CHECK(trace.vertexKeys[0].texcoord != trace.vertexKeys[3].texcoord);
    CHECK(trace.mesh.vertices[0].texcoord[1] == 1.0f);
    CHECK_FALSE(trace.generatedNormals);
    for (size_t t = 0; t < trace.triangles.size(); ++t) {
        for (size_t c = 0; c < 3; ++c) {
            CHECK(trace.vertexKeys[trace.mesh.indices[t*3+c]] == trace.triangles[t].corners[c]);
        }
        CHECK(mesh_lab::lineRelated(trace, trace.faces[trace.triangles[t].face].line, t));
    }
}
TEST_CASE("Quad triangulation retains polygon lineage and generates normals")
{
    const auto trace = sample("shared-quad.obj");
    REQUIRE(trace.faces.size() == 1);
    CHECK(trace.faces[0].corners.size() == 4);
    CHECK(trace.faces[0].triangleCount == 2);
    CHECK(trace.triangles[0].face == 0);
    CHECK(trace.triangles[1].face == 0);
    CHECK(trace.mesh.vertices.size() == 4);
    CHECK(trace.mesh.indices.size() == 6);
    CHECK(trace.generatedNormals);
    for (const auto& v : trace.mesh.vertices) { CHECK(v.normal[2] == doctest::Approx(1.0f)); }
}
TEST_CASE("Hard normal seams split eight positions into twenty-four vertices")
{
    const auto trace = sample("hard-cube.obj");
    CHECK(trace.positions.size() == 8);
    CHECK(trace.mesh.vertices.size() == 24);
    CHECK(trace.triangles.size() == 12);
    CHECK(trace.splitPositions == 8);
    for (const auto& ids : trace.positionVertices) { CHECK(ids.size() == 3); }
    for (size_t t = 0; t < trace.triangles.size(); ++t) { CHECK(trace.triangles[t].face == t / 2); }
}
TEST_CASE("Large coordinates retain double precision before float localization")
{
    const auto trace = sample("large-coordinates.obj");
    CHECK(trace.mesh.origin[0] == doctest::Approx(1000000000.003));
    CHECK(trace.mesh.vertices.size() == 4);
    CHECK(trace.triangles.size() == 2);
    for (size_t v = 0; v < trace.mesh.vertices.size(); ++v) {
        const auto p = static_cast<size_t>(trace.vertexKeys[v].position);
        CHECK(trace.mesh.precisePositions[v] == woby::relativePosition(trace.positions[p], trace.mesh.origin));
        CHECK(std::abs(trace.mesh.vertices[v].position[0]) < 0.003f);
    }
    CHECK(trace.mesh.bounds.radius > 0.002f);
}
TEST_CASE("Concave triangulation preserves area and face provenance")
{
    const auto trace = sample("concave-polygon.obj");
    REQUIRE(trace.triangles.size() == 3);
    double area = 0;
    for (size_t t = 0; t < 3; ++t) {
        const auto& a = trace.mesh.vertices[trace.mesh.indices[t*3]].position;
        const auto& b = trace.mesh.vertices[trace.mesh.indices[t*3+1]].position;
        const auto& c = trace.mesh.vertices[trace.mesh.indices[t*3+2]].position;
        area += ((b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])) * 0.5;
        CHECK(trace.triangles[t].face == 0);
    }
    CHECK(area == doctest::Approx(3.0));
}
TEST_CASE("Byte views expose the exact native vertex and index representations")
{
    const auto trace = sample("uv-seam.obj");
    const auto vertices = mesh_lab::vertexBytes(trace), indices = mesh_lab::indexBytes(trace);
    CHECK(vertices.size() == 192);
    CHECK(indices.size() == 24);
    CHECK(mesh_lab::vertexOffset(3, 6) == 120);
    float u = 0;
    std::memcpy(&u, vertices.data() + mesh_lab::vertexOffset(3, 6), sizeof(u));
    CHECK(u == trace.mesh.vertices[3].texcoord[0]);
    uint32_t id = 0;
    std::memcpy(&id, indices.data() + 12, sizeof(id));
    CHECK(id == trace.mesh.indices[3]);
    CHECK_THROWS_AS((void)mesh_lab::vertexOffset(0, 8), std::out_of_range);
    CHECK_THROWS_AS((void)mesh_lab::vertexOffset(std::numeric_limits<size_t>::max(), 0), std::out_of_range);
}
TEST_CASE("Selection and camera operations enforce valid inspector state")
{
    const auto trace = sample("uv-seam.obj");
    mesh_lab::UiState state;
    mesh_lab::selectTriangle(state, trace, 100);
    mesh_lab::selectCorner(state, 100);
    mesh_lab::selectComponent(state, 100);
    CHECK(state.triangle == 1); CHECK(state.corner == 2); CHECK(state.component == 7);
    CHECK(mesh_lab::selectedVertex(state, trace) == 5);
    mesh_lab::selectVertex(state, trace, 3);
    CHECK(state.triangle == 1); CHECK(state.corner == 0);
    mesh_lab::selectLine(state, trace, trace.faces[0].line);
    CHECK(state.triangle == 0);
    mesh_lab::orbit(state, 0, 20, 100);
    CHECK(state.pitch == doctest::Approx(1.45f)); CHECK(state.zoom == 4.0f);
    mesh_lab::orbit(state, 0, 0, std::numeric_limits<float>::quiet_NaN());
    CHECK(state.zoom == 4.0f);
    mesh_lab::selectNode(state, static_cast<mesh_lab::Node>(100));
    CHECK(state.node == mesh_lab::Node::pack);
    CHECK_FALSE(mesh_lab::lineRelated(trace, 999, 0));
}
TEST_CASE("Malformed and unsupported captures fail before GPU allocation")
{
    Fixture fixture;
    CHECK_THROWS((void)mesh_lab::loadTrace(fixture.write("v 0 0 0\nf 1 2 3\n")));
    CHECK_THROWS((void)mesh_lab::loadTrace(fixture.write("v 0 0 0\np 1\n")));
    CHECK_THROWS((void)mesh_lab::loadTrace(fixture.write("# no geometry\n")));
    CHECK_THROWS((void)mesh_lab::loadTrace(fixture.write(std::string(mesh_lab::maxSourceBytes + 1, '#'))));
    CHECK_THROWS((void)mesh_lab::loadTrace(fixture.root / "missing.obj"));
}
TEST_CASE("Multiple shapes and CRLF keep correct source byte and face offsets")
{
    Fixture fixture;
    const auto trace = mesh_lab::loadTrace(fixture.write(
        "v 0 0 0\r\nv 1 0 0\r\nv 0 1 0\r\no A\r\nf 1 2 3\r\ng B\r\nf -3 -1 -2\r\n"));
    CHECK(trace.lines[1].byteOffset == 9);
    CHECK(trace.triangles[0].face == 0);
    CHECK(trace.triangles[1].face == 1);
    CHECK(trace.faces[0].line == 4);
    CHECK(trace.faces[1].line == 6);
    CHECK(trace.mesh.nodes.size() == 2);
}

TEST_CASE("The internal example exposes rebasing triangulation and seam identity")
{
    const auto trace = mesh_lab::internalExample();
    CHECK(trace.positions.size() == 6);
    CHECK(trace.faces.size() == 2);
    CHECK(trace.originalCorners == 8);
    CHECK(trace.triangles.size() == 4);
    CHECK(trace.mesh.vertices.size() == 8);
    CHECK(trace.splitPositions == 2);
    CHECK_FALSE(trace.generatedNormals);
    CHECK(trace.mesh.origin[0] > 1.0e9);
    for (size_t i = 0; i < trace.positions.size(); ++i) {
        CHECK(trace.localPositions[i] == woby::relativePosition(trace.positions[i], trace.mesh.origin));
    }
    CHECK(static_cast<float>(trace.positions[0][0]) == static_cast<float>(trace.positions[1][0]));
    CHECK(trace.localPositions[0][0] != trace.localPositions[1][0]);
    CHECK(trace.positionVertices[2].size() == 2);
    CHECK(trace.positionVertices[3].size() == 2);
}
TEST_CASE("Intern events reconstruct actual first-use IDs and reuse decisions")
{
    const auto trace = mesh_lab::internalExample();
    std::vector<bool> seen(trace.mesh.vertices.size()); size_t inserts = 0, reused = 0;
    REQUIRE(trace.vertexEvents.size() == trace.mesh.indices.size());
    for (size_t i = 0; i < trace.vertexEvents.size(); ++i) {
        const auto event = trace.vertexEvents[i];
        CHECK(event.vertex == trace.mesh.indices[i]);
        CHECK(event.created == !seen[event.vertex]);
        if (event.created) { CHECK(event.vertex == inserts); ++inserts; }
        else { ++reused; }
        seen[event.vertex] = true;
    }
    CHECK(inserts == 8); CHECK(reused == 4);
}
TEST_CASE("Pipeline edges alternate data and transformers with exact payload sizes")
{
    using mesh_lab::Node;
    const auto trace = mesh_lab::internalExample();
    CHECK(mesh_lab::payloadBytes(Node::attributes,trace) == 328);
    CHECK(mesh_lab::payloadBytes(Node::corners,trace) == 376);
    CHECK(mesh_lab::payloadBytes(Node::mesh,trace) == 688);
    CHECK(mesh_lab::payloadBytes(Node::gpu,trace) == 304);
    CHECK(mesh_lab::payloadBytes(Node::source,trace) == trace.source.size());
    for (size_t i = 0; i < mesh_lab::pipeline.size(); ++i) {
        const auto& node = mesh_lab::pipeline[i];
        CHECK(static_cast<size_t>(node.id) == i);
        CHECK(node.transformer == (i%2 == 1));
        if (node.transformer) {
            const auto edges = mesh_lab::transformationEndpoints(node.id);
            CHECK(static_cast<size_t>(edges[0]) == i-1);
            CHECK(static_cast<size_t>(edges[1]) == i+1);
        }
    }
    CHECK_THROWS_AS((void)mesh_lab::payloadBytes(Node::parse,trace), std::invalid_argument);
    CHECK_THROWS_AS((void)mesh_lab::transformationEndpoints(Node::mesh), std::invalid_argument);
    CHECK_THROWS_AS((void)mesh_lab::transformationEndpoints(Node::count), std::invalid_argument);
    mesh_lab::UiState state;
    mesh_lab::selectNode(state,Node::upload); CHECK(state.node == Node::upload);
    mesh_lab::selectPosition(state,trace,3);
    CHECK(trace.vertexKeys[mesh_lab::selectedVertex(state,trace)].position == 3);
    mesh_lab::selectPosition(state,trace,999);
    CHECK(trace.vertexKeys[mesh_lab::selectedVertex(state,trace)].position == 3);
}
TEST_CASE("In-memory OBJ construction matches production file import")
{
    Fixture fixture;
    const auto source = mesh_lab::internalObjSource();
    const auto memory = woby::loadObjMeshText(source);
    const auto file = woby::loadObjMesh(fixture.write(std::string(source)));
    CHECK(memory.indices == file.indices);
    CHECK(memory.precisePositions == file.precisePositions);
    CHECK(memory.origin == file.origin);
    REQUIRE(memory.vertices.size() == file.vertices.size());
    CHECK(std::memcmp(memory.vertices.data(),file.vertices.data(),memory.vertices.size()*sizeof(woby::Vertex)) == 0);
    CHECK_THROWS((void)woby::loadObjMeshText("v 0 0 0\nf 1 2 3\n"));
    CHECK_THROWS((void)woby::loadObjMeshText("# empty\n"));
}
