#include "obj_mesh.h"
#include "freeform.h"
#include "utf8_path.h"
#include <rapidobj/prototype.hpp>
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
struct PrototypeFixture {
    std::filesystem::path root;
    PrototypeFixture() {
        const auto prefix = "woby_obj_prototype_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        for (size_t i = 0;; ++i) {
            root = std::filesystem::absolute(std::filesystem::temp_directory_path()) / (prefix + "_" + std::to_string(i));
            if (std::filesystem::create_directory(root)) { break; }
        }
    }
    ~PrototypeFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path write(std::string_view text, const std::filesystem::path& name = woby::pathFromUtf8("曲面.obj")) const {
        const auto path = root / name;
        std::ofstream out(path, std::ios::binary);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) { throw std::runtime_error("Cannot write prototype fixture."); }
        return path;
    }
};

void sameMesh(const woby::Mesh& a, const woby::Mesh& b) {
    CHECK(a.origin == b.origin);
    CHECK(a.precisePositions == b.precisePositions);
    CHECK(a.indices == b.indices); CHECK(a.lineIndices == b.lineIndices); CHECK(a.pointIndices == b.pointIndices);
    REQUIRE(a.vertices.size() == b.vertices.size());
    for (size_t i = 0; i < a.vertices.size(); ++i) {
        CHECK(a.vertices[i].position == b.vertices[i].position);
        CHECK(a.vertices[i].normal == b.vertices[i].normal);
        CHECK(a.vertices[i].texcoord == b.vertices[i].texcoord);
    }
    REQUIRE(a.sourceData); REQUIRE(b.sourceData);
    CHECK(a.sourceData->points == b.sourceData->points);
    CHECK(a.sourceData->indices == b.sourceData->indices);
    CHECK(a.sourceData->originalPointIds == b.sourceData->originalPointIds);
    REQUIRE(a.nodes.size() == b.nodes.size());
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        CHECK(a.nodes[i].name == b.nodes[i].name);
        CHECK(a.nodes[i].indexOffset == b.nodes[i].indexOffset);
        CHECK(a.nodes[i].indexCount == b.nodes[i].indexCount);
        CHECK(a.nodes[i].lineIndexOffset == b.nodes[i].lineIndexOffset);
        CHECK(a.nodes[i].lineIndexCount == b.nodes[i].lineIndexCount);
        CHECK(a.nodes[i].pointIndexOffset == b.nodes[i].pointIndexOffset);
        CHECK(a.nodes[i].pointIndexCount == b.nodes[i].pointIndexCount);
        CHECK(a.nodes[i].hasTexcoords == b.nodes[i].hasTexcoords);
    }
    CHECK(a.bounds.min == b.bounds.min); CHECK(a.bounds.max == b.bounds.max);
    REQUIRE(bool(a.freeform) == bool(b.freeform));
    if (a.freeform) {
        REQUIRE(a.freeform->patches.size() == b.freeform->patches.size());
        for (size_t i = 0; i < a.freeform->patches.size(); ++i) {
            const auto& x = a.freeform->patches[i]; const auto& y = b.freeform->patches[i];
            CHECK(x.controls == y.controls); CHECK(x.texcoords == y.texcoords); CHECK(x.normals == y.normals);
            CHECK(x.knotsU == y.knotsU); CHECK(x.knotsV == y.knotsV);
            CHECK(x.domainU == y.domainU); CHECK(x.domainV == y.domainV);
        }
    }
}

const std::string polygons =
    "v 1000000000 0 0\nv 1000000002 0 0\nv 1000000002 2 0\nv 1000000001 1 0\nv 1000000000 2 0\n"
    "v 1000000000 0 0\nv 1000000000 0 0\n" // distinct identity and unused point
    "vt 0 0\nvt 1 0\nvt 1 1\nvt .5 .5\nvt 0 1\nvn 0 0 1\nvn 0 0 -1\n"
    "g concave\nf -7/1/1 -6/2/1 -5/3/1 -4/4/1 -3/5/1\n"
    "g seam\nf 1/5/2 3/3/2 2/2/2\nf 6/1/1 2/2/1 3/3/1\n"
    "l -7 -6 -5\np 6 7\n";

const std::string rationalTrimmed =
    "vp .25 .25 1\nvp .75 .25 1\nvp .75 .75 1\nvp .25 .75 1\n"
    "cstype rat bezier\ndeg 1\ncurv2 -4 -3 \\\n -2 -1 -4\nparm u 0 1 2 3 4\nend\n"
    "v 1000000000 0 0 1\nv 1000000002 0 0 .75\nv 1000000000 2 0 1\nv 1000000002 2 0 .75\n"
    "vt .123456789012345 0\nvt 1 0\nvt 0 1\nvt 1 1\nvn 0 0 -1\n"
    "g polygon\nf 1/1/1 2/2/1 4/4/1\nl 1 3\np 2\n"
    "g trimmed\ncstype rat bezier\ndeg 1 1\nsurf 0 1 0 1 -4/1/1 -3/2/1 \\\n -2/3/1 -1/4/1\n"
    "parm u 0 1\nparm v 0 1\nhole 4 0 -1\nend\n"
    "v 1000000001 1 0\n"; // must not affect preceding negative references

std::string failureLine(std::string_view text, const woby::ObjPrototypeOptions& options) {
    try { (void)woby::loadObjMeshTextPrototype(text, {}, options); }
    catch (const std::runtime_error& error) { return error.what(); }
    return {};
}
} // namespace

TEST_CASE("RapidOBJ prototype merges into caller storage and triangulates borrowed positions") {
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    positions.reserve(8);
    const auto* allocation = positions.data();
    rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
    auto materials = rapidobj::MaterialLibrary::String("");
    SUBCASE("supplied empty library") {}
    SUBCASE("ignored material data retains triangulation metadata") { materials = rapidobj::MaterialLibrary::Ignore(); }
    rapidobj::PrototypeOptions options; options.chunk_bytes = 19; options.read_bytes = 7; options.workers = 2;
    auto result = rapidobj::ParseMemoryPrototype("v 0 0 0\nv 2 0 0\nv 2 2 0\nv 0 2 0\nf -4 -3 -2 -1\n",
        buffers, materials, options);
    REQUIRE_FALSE(result.polygons.error);
    CHECK(result.polygons.attributes.positions.empty());
    CHECK(positions.data() == allocation);
    REQUIRE(positions.size() == 4);
    CHECK(result.stats.position_copy_bytes_avoided == 4 * sizeof(woby::Coordinate));
    CHECK(result.stats.chunks > 1);
    REQUIRE(rapidobj::Triangulate(result.polygons, positions));
    CHECK(result.polygons.shapes[0].mesh.indices.size() == 6);
    CHECK(positions.data() == allocation);
}

TEST_CASE("RapidOBJ merge subdivisions preserve each range's negative reference flags") {
    namespace d = rapidobj::detail;
    const rapidobj::Index input[] = {{0, -1, -1}, {1, -1, -1}, {2, -1, -1}, {0, -1, -1}, {1, -1, -1}, {2, -1, -1}};
    const auto relative = static_cast<d::OffsetFlags>(d::ApplyOffset::Position);
    const d::OffsetFlags flags[] = {0, 0, 0, relative, relative, relative};
    rapidobj::Index output[6]{};
    const d::CopyIndices copy(output, input, flags, 6, {3, 0, 0}, {6, 0, 0});
    for (const auto& task : copy.Subdivide(2)) {
        REQUIRE(std::visit([](const auto& part) { return part.Execute(); }, task) == rapidobj::rapidobj_errc::Success);
    }
    for (int i = 0; i < 6; ++i) { CHECK(output[i].position_index == i); }
}

TEST_CASE("OBJ prototype preserves polygon identities seams winding and file memory parity") {
    const PrototypeFixture fixture;
    const auto path = fixture.write(polygons);
    const auto reference = woby::loadObjMeshLegacy(path);
    for (size_t workers : {size_t{1}, size_t{2}, size_t{4}}) {
        const woby::ObjPrototypeOptions options{31, 7, workers};
        woby::ObjPrototypeMetrics metrics;
        sameMesh(reference, woby::loadObjMeshPrototype(path, {}, options, &metrics));
        sameMesh(reference, woby::loadObjMeshTextPrototype(polygons, {}, options));
        CHECK(metrics.inputBytes == polygons.size());
        CHECK(metrics.chunks > 1);
        CHECK(metrics.positionCopyBytesAvoided == 7 * sizeof(woby::Coordinate));
    }
}

TEST_CASE("OBJ prototype resolves rational trimming and attributes across logical chunks") {
    const PrototypeFixture fixture;
    const auto path = fixture.write(rationalTrimmed);
    const auto reference = woby::loadObjMeshLegacy(path);
    for (const woby::ObjPrototypeOptions options : {
            woby::ObjPrototypeOptions{1, 1, 1}, {47, 7, 2}, {113, 19, 4}, {4 * 1024 * 1024, 256 * 1024, 2}}) {
        CAPTURE(options.chunkBytes); CAPTURE(options.readBytes); CAPTURE(options.workers);
        const auto file = woby::loadObjMeshPrototype(path, {}, options);
        const auto memory = woby::loadObjMeshTextPrototype(rationalTrimmed, {}, options);
        sameMesh(reference, file); sameMesh(reference, memory);
        REQUIRE(memory.freeform);
        CHECK(memory.freeform->patches[0].texcoords[0][0] == .123456789012345);
        REQUIRE(memory.freeform->patches[0].trimming);
        CHECK(memory.freeform->patches[0].trimming->regions[0].holes[0].segments[0].interval
            == std::array<double, 2>{4, 0});
    }
}

TEST_CASE("OBJ prototype accepts short UVs weighted positions and continued CRLF records") {
    const PrototypeFixture fixture;
    const std::string text = "v 1 0 0 1\r\nv 1 1 0 .7071067811865476\r\nv 0 1 0 1\r\nvt .25\r\n"
        "g arc\r\ncstype rat bspline\r\ndeg 2\r\ncurv 0 1 -3 \\\r\n -2 -1\r\nparm u 0 0 0 1 1 1\r\nend";
    const auto reference = woby::loadObjMeshLegacy(fixture.write(text));
    sameMesh(reference, woby::loadObjMeshTextPrototype(text, {}, {21, 1, 3}));
    const auto triangle = woby::loadObjMeshTextPrototype("v 0 0 0 1\nv 1 0 0 1\nv 0 1 0 1\nvt .25\nf 1/1 2/1 3/1\n");
    CHECK(triangle.vertices[0].texcoord == std::array<float, 2>{.25f, 1});
}

TEST_CASE("OBJ prototype retains physical diagnostics independently of read and worker boundaries") {
    const std::string prefix = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    for (const auto& [tail, line] : std::vector<std::pair<std::string, size_t>>{
            {"f 1 \\\n 2 invalid\n", 4}, {"cstype bezier\ndeg 2\ncurv 0 1 1 2 3\nv 4 5 6\n", 7},
            {"cstype bezier\ndeg 2\ncurv 0 1 -4 -2 -1\nparm u 0 1\nend\n", 6},
            {"cstype cardinal\n", 4}, {"vp 0 0\ncon 1 2\n", 5}, {"f 1 2 \\\n", 4}}) {
        for (size_t workers : {size_t{1}, size_t{3}}) {
            CAPTURE(tail); CAPTURE(workers);
            const auto error = failureLine(prefix + tail, {16, 3, workers});
            CHECK_FALSE(error.empty());
            CHECK(error.find("line " + std::to_string(line)) != std::string::npos);
            CHECK(error.find("<memory>") != std::string::npos);
        }
    }
}

TEST_CASE("OBJ prototype bounds logical statements and reports earliest worker failure") {
    const auto text = "v invalid 0 0\n" + std::string(1024 * 1024 + 1, 'x');
    const auto error = failureLine(text, {8, 13, 3});
    CHECK(error.find("line 1") != std::string::npos);
    CHECK_THROWS((void)woby::loadObjMeshTextPrototype("v 0 0 0\n" + std::string(8192, '#') + "\np 1\n"));
    // Legal long freeform statements span many reads but remain one record.
    const auto curve = "v 0 0 0\nv 1 1 0\nv 2 0 0\ncstype bezier\ndeg 2\ncurv 0 1 1"
        + std::string(8192, ' ') + "2 3\nparm u 0 1\nend\n";
    REQUIRE(woby::loadObjMeshTextPrototype(curve, {}, {32, 11, 2}).freeform);
}

TEST_CASE("OBJ prototype material resolution is explicit and independent of working directory") {
    const PrototypeFixture fixture;
    fixture.write("newmtl paint\nKd 1 0 0\n", "surface.mtl");
    const auto text = "mtllib surface.mtl\nusemtl paint\n" + polygons;
    const auto path = fixture.write(text);
    sameMesh(woby::loadObjMeshLegacy(path), woby::loadObjMeshPrototype(path, {}, {32, 7, 3}));
    // Memory loading ignores the adjacent library, retaining optional IDs.
    sameMesh(woby::loadObjMeshPrototype(path), woby::loadObjMeshTextPrototype(text));
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    CHECK_THROWS_AS(rapidobj::ParseMemoryPrototype(text, {positions, texcoords, normals},
        rapidobj::MaterialLibrary::SearchPath(".")), std::invalid_argument);
}

TEST_CASE("RapidOBJ prototype cancels with active workers and leaves caller buffers reusable") {
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
    size_t checkpoints = 0;
    rapidobj::PrototypeOptions options; options.chunk_bytes = 16; options.read_bytes = 8; options.workers = 3;
    options.user = &checkpoints;
    options.checkpoint = [](void* data) {
        if (++*static_cast<size_t*>(data) == 14) { throw std::runtime_error("cancel prototype"); }
    };
    CHECK_THROWS_WITH(rapidobj::ParseMemoryPrototype(polygons, buffers, rapidobj::MaterialLibrary::String(""), options),
        "cancel prototype");
    CHECK(positions.empty()); CHECK(texcoords.empty()); CHECK(normals.empty());
    auto result = rapidobj::ParseMemoryPrototype(polygons, buffers, rapidobj::MaterialLibrary::String(""));
    CHECK_FALSE(result.polygons.error); CHECK(positions.size() == 7);
}

TEST_CASE("RapidOBJ prototype reports stream failures and rejects invalid resource settings") {
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
    std::istringstream input("v 0 0 0\n"); input.setstate(std::ios::badbit);
    CHECK(rapidobj::ParseStreamPrototype(input, buffers, rapidobj::MaterialLibrary::String("")).polygons.error);
    CHECK(positions.empty());
    CHECK_THROWS_AS((void)woby::loadObjMeshTextPrototype(polygons, {}, {0, 8, 2}), std::invalid_argument);
}
