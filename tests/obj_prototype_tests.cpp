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

TEST_CASE("Standard OBJ loaders preserve mixed primitives and weighted trimmed freeforms") {
    const PrototypeFixture fixture;
    const std::string* text = &polygons;
    SUBCASE("polygons, lines and points") {}
    SUBCASE("weighted trimmed freeforms") { text = &rationalTrimmed; }
    const auto path = fixture.write(*text);
    const auto reference = woby::loadObjMeshLegacy(path);
    sameMesh(reference, woby::loadObjMesh(path));
    sameMesh(reference, woby::loadObjMeshText(*text));
}

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

TEST_CASE("RapidOBJ prototype balances medium inputs and preserves relative references") {
    const PrototypeFixture fixture;
    std::string text;
    constexpr size_t count = 24000;
    for (size_t i = 0; i < count; ++i) {
        text += "v " + std::to_string(1000000000 + i) + ".125 1000000000.25 1000000000.5\np -1\n";
    }
    REQUIRE(text.size() > 1024 * 1024);
    REQUIRE(text.size() < 4 * 1024 * 1024);
    const auto path = fixture.write(text);
    for (const bool fromFile : {false, true}) {
        std::vector<woby::Coordinate> positions, normals;
        std::vector<std::array<double, 2>> texcoords;
        rapidobj::PrototypeOptions options;
        options.workers = 4;
        const rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
        const auto result = fromFile ? rapidobj::ParseFilePrototype(path, buffers, options)
            : rapidobj::ParseMemoryPrototype(text, buffers, rapidobj::MaterialLibrary::Ignore(), options);
        REQUIRE_FALSE(result.polygons.error);
        CHECK(result.stats.workers == 4);
        CHECK(result.stats.chunks == 4);
        REQUIRE(positions.size() == count);
        REQUIRE(result.polygons.shapes.size() == 1);
        const auto& points = result.polygons.shapes[0].points.indices;
        REQUIRE(points.size() == count);
        for (size_t i = 0; i < count; ++i) {
            CHECK(positions[i] == woby::Coordinate{1000000000.125 + static_cast<double>(i), 1000000000.25, 1000000000.5});
            CHECK(points[i].position_index == static_cast<int>(i));
        }
    }
}

TEST_CASE("RapidOBJ file blocks keep bounded storage without publishing stale tail data") {
    const PrototypeFixture fixture;
    std::string text;
    constexpr size_t count = 5000;
    for (size_t i = 0; i < count; ++i) {
        text += "v " + std::to_string(i) + ".125 2.25 -3.5\np -1\n";
    }
    text.pop_back(); // the final, shorter read ends in an unterminated physical line
    rapidobj::PrototypeOptions options;
    options.chunk_bytes = 4096; options.workers = 3;
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    positions.reserve(count + 19);
    const auto* storage = positions.data();
    const auto parsed = rapidobj::ParseFilePrototype(fixture.write(text), {positions, texcoords, normals}, options);
    REQUIRE_FALSE(parsed.polygons.error);
    REQUIRE(positions.size() == count);
    REQUIRE(parsed.polygons.shapes.size() == 1);
    const auto& points = parsed.polygons.shapes[0].points.indices;
    REQUIRE(points.size() == count);
    CHECK(positions.data() == storage);
    CHECK(parsed.stats.input_bytes == text.size());
    CHECK(parsed.stats.chunks > options.workers * 2);
    CHECK(parsed.stats.peak_inflight_text_bytes <= options.chunk_bytes * options.workers);
    for (size_t i = 0; i < count; ++i) {
        CHECK(positions[i] == woby::Coordinate{static_cast<double>(i) + .125, 2.25, -3.5});
        CHECK(points[i].position_index == static_cast<int>(i));
    }
}

TEST_CASE("RapidOBJ native ranges preserve freeform bodies across read boundaries") {
    const PrototypeFixture fixture;
    const woby::ObjPrototypeOptions options{4 * 1024 * 1024, 256 * 1024, 4};
    const auto pad = [](std::string& text, size_t target) {
        while (text.size() < target) {
            const auto amount = std::min(size_t{128}, target - text.size());
            if (amount == 1) { text += '\n'; }
            else { text += '#' + std::string(amount - 2, 'x') + '\n'; }
        }
    };
    for (size_t split : {size_t{1}, size_t{11}, size_t{63}, size_t{127}}) {
        std::string text = polygons;
        pad(text, 256 * 1024 - split);
        text += rationalTrimmed;
        pad(text, 5 * 256 * 1024 + 13);
        const auto path = fixture.write(text);
        const auto memory = woby::loadObjMeshTextPrototype(text, {}, options);
        woby::ObjPrototypeMetrics metrics;
        const auto file = woby::loadObjMeshPrototype(path, {}, options, &metrics);
        sameMesh(memory, file);
        CHECK(metrics.inputBytes == text.size());
        CHECK(metrics.workers == options.workers);
        CHECK(metrics.peakInflightTextBytes <= options.chunkBytes * options.workers);
    }
}

TEST_CASE("RapidOBJ native ranges fall back safely for long boundary records") {
    const PrototypeFixture fixture;
    std::string text = "v 1 2 3 #" + std::string(600000, 'x') + "\\\n";
    for (size_t i = 0; i < 100000; ++i) { text += "#pad\n"; }
    text += "v 4 5 6\np -2 -1";
    REQUIRE(text.size() > 1024 * 1024);
    const woby::ObjPrototypeOptions options{4 * 1024 * 1024, 256 * 1024, 4};
    sameMesh(woby::loadObjMeshTextPrototype(text, {}, options), woby::loadObjMeshPrototype(fixture.write(text), {}, options));
}

TEST_CASE("RapidOBJ native ranges report the earliest error while prefetches are outstanding") {
    const PrototypeFixture fixture;
    std::string text;
    for (size_t line = 1; line <= 200000; ++line) {
        text += line == 30000 || line == 70000 ? "v broken\n" : "v 1 2 3\n";
    }
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    rapidobj::PrototypeOptions options; options.workers = 4;
    size_t expected_line = 30000;
    auto expected_error = rapidobj::rapidobj_errc::ParseError;
    SUBCASE("decoded failure after complete records") {}
    SUBCASE("limit failure before the first complete record") {
        text.replace(0, 8, "v 1234567 0 0\n");
        options.max_statement_bytes = 8;
        expected_line = 1;
        expected_error = rapidobj::rapidobj_errc::LineTooLongError;
    }
    const auto result = rapidobj::ParseFilePrototype(fixture.write(text), {positions, texcoords, normals}, options);
    REQUIRE(result.polygons.error);
    CHECK(result.polygons.error.line_num == expected_line);
    CHECK(result.polygons.error.code == rapidobj::make_error_code(expected_error));
    CHECK(positions.empty()); CHECK(texcoords.empty()); CHECK(normals.empty());
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

TEST_CASE("OBJ prototype preserves continued bodies at every raw block alignment") {
    const PrototypeFixture fixture;
    // A comment's trailing backslash is not a continuation, including when its
    // '#' and backslash arrive in different blocks. Also split CRLF and tokens.
    const std::string body = "# comment with a trailing backslash \\\r\n" + rationalTrimmed;
    const auto reference = woby::loadObjMeshLegacy(fixture.write(rationalTrimmed));
    for (size_t padding = 0; padding < 64; ++padding) {
        CAPTURE(padding);
        const auto text = "#" + std::string(padding, 'x') + "\r\n" + body;
        const auto path = fixture.write(text);
        const woby::ObjPrototypeOptions options{64, 7, 3};
        sameMesh(reference, woby::loadObjMeshPrototype(path, {}, options));
        sameMesh(reference, woby::loadObjMeshTextPrototype(text, {}, options));

        std::vector<woby::Coordinate> positions, normals;
        std::vector<std::array<double, 2>> texcoords;
        std::istringstream stream(text);
        rapidobj::PrototypeOptions settings; settings.chunk_bytes = 64; settings.read_bytes = 7; settings.workers = 3;
        const auto parsed = rapidobj::ParseStreamPrototype(stream, {positions, texcoords, normals},
            rapidobj::MaterialLibrary::Ignore(), settings);
        REQUIRE_FALSE(parsed.polygons.error);
        REQUIRE(positions.size() == 5);
        CHECK(positions.back() == woby::Coordinate{1000000001, 1, 0});
        REQUIRE(parsed.weights.size() == 2);
        CHECK(parsed.weights[0].position == 1); CHECK(parsed.weights[1].position == 3);
        CHECK(parsed.stats.input_bytes == text.size());
        const auto surface = std::find_if(parsed.statements.begin(), parsed.statements.end(), [](const auto& statement) {
            return statement.kind == rapidobj::StatementKind::surface;
        });
        REQUIRE(surface != parsed.statements.end());
        CHECK(surface->positions == 4); CHECK(surface->texcoords == 4); CHECK(surface->normals == 1);
        const auto offset = text.find("surf ");
        CHECK(surface->line == 1 + static_cast<size_t>(std::count(text.begin(), text.begin() + offset, '\n')));
    }
}

TEST_CASE("OBJ prototype reports the earliest error at every raw block alignment") {
    const PrototypeFixture fixture;
    const std::string body = "v 0 0 0\r\nv 1 0 0\r\nv 0 1 0 # trailing slash \\\r\nf 1 \\\r\n 2 invalid\r\n"
        "v also-invalid 0 0\n";
    for (size_t padding = 0; padding < 64; ++padding) {
        const auto text = "#" + std::string(padding, 'x') + "\n" + body;
        const auto path = fixture.write(text);
        for (int source = 0; source < 3; ++source) {
            CAPTURE(padding); CAPTURE(source);
            std::vector<woby::Coordinate> positions, normals;
            std::vector<std::array<double, 2>> texcoords;
            rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
            rapidobj::PrototypeOptions settings; settings.chunk_bytes = 64; settings.read_bytes = 3; settings.workers = 4;
            std::istringstream stream(text);
            const auto parsed = source == 0 ? rapidobj::ParseMemoryPrototype(text, buffers, rapidobj::MaterialLibrary::Ignore(), settings)
                : source == 1 ? rapidobj::ParseStreamPrototype(stream, buffers, rapidobj::MaterialLibrary::Ignore(), settings)
                : rapidobj::ParseFilePrototype(path, buffers, settings);
            REQUIRE(parsed.polygons.error);
            CHECK(parsed.polygons.error.line_num == 5);
            CHECK(positions.empty()); CHECK(normals.empty()); CHECK(texcoords.empty());
        }
    }
}

TEST_CASE("OBJ prototype validates numeric token endings without a delimiter prescan") {
    const auto valid = woby::loadObjMeshTextPrototype("v +0 -0 0e0\nv +1.0 0 0\nv 0 +1E+0 0\n"
        "vt +.25 .5\nvn 0 0 +1\nf 1/1/1 2/1/1 3/1/1\n", {}, {23, 2, 3});
    REQUIRE(valid.vertices.size() == 3);
    CHECK(valid.vertices[0].texcoord == std::array<float, 2>{.25f, .5f});
    for (const std::string line : {"v 1x 0 0", "v 1.0.1 0 0", "v 1e+ 0 0", "v nan 0 0",
            "v 1e309 0 0", "vt .5suffix", "vn 0 0 1x", "v + 0 0", "v 0 0 0 1 2"}) {
        CAPTURE(line);
        CHECK(failureLine(line + "\n", {7, 2, 2}).find("line 1") != std::string::npos);
    }
}

TEST_CASE("RapidOBJ position fast path preserves optional fields comments and exact numbers") {
    const PrototypeFixture fixture;
    const std::string text = " \tv\t+1e2 -0 .25# comment\\\r\n"
        "v 1 2 3 .5 # weight\r\n"
        "v 2 4 6 .1 .2 .3# color\n"
        "v 3 6 9 1 .1 .2 .3 # seven values\n"
        "v 4 \\\n 8 12# continued\n";
    const auto path = fixture.write(text);
    for (size_t block : {size_t{3}, size_t{128}, size_t{4 * 1024 * 1024}}) {
        for (int source = 0; source < 3; ++source) {
            rapidobj::PrototypeOptions options; options.chunk_bytes = block; options.read_bytes = 2; options.workers = 3;
            std::vector<woby::Coordinate> positions, normals;
            std::vector<std::array<double, 2>> texcoords;
            rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
            std::istringstream stream(text);
            const auto result = source == 0 ? rapidobj::ParseMemoryPrototype(text, buffers, rapidobj::MaterialLibrary::Ignore(), options)
                : source == 1 ? rapidobj::ParseStreamPrototype(stream, buffers, rapidobj::MaterialLibrary::Ignore(), options)
                : rapidobj::ParseFilePrototype(path, buffers, options);
            REQUIRE_FALSE(result.polygons.error);
            REQUIRE(positions.size() == 5);
            CHECK(positions[0] == woby::Coordinate{100, -0.0, .25});
            CHECK(std::signbit(positions[0][1]));
            for (size_t i = 1; i < positions.size(); ++i) {
                const auto x = static_cast<double>(i);
                CHECK(positions[i] == woby::Coordinate{x, x * 2, x * 3});
            }
            REQUIRE(result.weights.size() == 1);
            CHECK(result.weights[0].position == 1);
            CHECK(result.weights[0].weight == .5);
        }
    }
    for (const std::string line : {"v 1 2 3 4 5#bad", "v 1 2#missing", "v 1 2 3 nan", "v 1 2 3 1 2 3 4 5", "v 1 2 3x#bad"}) {
        CHECK(failureLine(line + "\n", {11, 3, 2}).find("line 1") != std::string::npos);
    }
}

TEST_CASE("RapidOBJ incomplete position tuples leave caller storage reusable") {
    for (const std::string invalid : {"v 4 5 nan", "v 4 5 6 7 8", "v 4 5 6 7x"}) {
        std::vector<woby::Coordinate> positions, normals;
        std::vector<std::array<double, 2>> texcoords;
        positions.reserve(16);
        const auto* storage = positions.data();
        rapidobj::PrototypeOptions options; options.chunk_bytes = 9; options.workers = 3;
        const rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
        const auto bad = rapidobj::ParseMemoryPrototype("v 1 2 3\n" + invalid + "\n", buffers,
            rapidobj::MaterialLibrary::Ignore(), options);
        REQUIRE(bad.polygons.error);
        CHECK(bad.polygons.error.line_num == 2);
        CHECK(positions.empty()); CHECK(texcoords.empty()); CHECK(normals.empty());
        const auto good = rapidobj::ParseMemoryPrototype("v 9.125 8.25 7.5\np -1\n", buffers,
            rapidobj::MaterialLibrary::Ignore(), options);
        REQUIRE_FALSE(good.polygons.error);
        REQUIRE(positions.size() == 1);
        CHECK(positions[0] == woby::Coordinate{9.125, 8.25, 7.5});
        CHECK(positions.data() == storage);
    }
}

TEST_CASE("RapidOBJ point fast path preserves relative forward and multi-point references") {
    const PrototypeFixture fixture;
    const std::string text = "v 0 0 0\np 3\nv 1 0 0\np -1\nv 0 1 0\n"
        "p 1#" + std::string(5000, 'x') + "\\\r\np -1\np 1 2 -1\n";
    const auto path = fixture.write(text);
    const int expected[] = {2, 1, 0, 2, 0, 1, 2};
    for (size_t block : {size_t{17}, size_t{4096}, size_t{4 * 1024 * 1024}}) {
        for (bool file : {false, true}) {
            rapidobj::PrototypeOptions options; options.chunk_bytes = block; options.workers = 3;
            std::vector<woby::Coordinate> positions, normals;
            std::vector<std::array<double, 2>> texcoords;
            rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
            const auto result = file ? rapidobj::ParseFilePrototype(path, buffers, options)
                : rapidobj::ParseMemoryPrototype(text, buffers, rapidobj::MaterialLibrary::Ignore(), options);
            REQUIRE_FALSE(result.polygons.error);
            REQUIRE(result.polygons.shapes.size() == 1);
            const auto& points = result.polygons.shapes[0].points.indices;
            REQUIRE(points.size() == std::size(expected));
            for (size_t i = 0; i < points.size(); ++i) {
                CHECK(points[i].position_index == expected[i]);
                CHECK(points[i].texcoord_index == -1);
                CHECK(points[i].normal_index == -1);
            }
        }
    }
    for (const std::string line : {"p 0", "p -2", "p 2", "p 2147483648", "p -2147483649", "p 1x", "p 1/1", "p 1//1", "p +1"}) {
        CHECK_FALSE(failureLine("v 0 0 0\n" + line + "\n", {7, 3, 3}).empty());
    }
    CHECK(failureLine("v 0 0 0\np " + std::string(4096, ' ') + "1\n", {31, 7, 3}).find("line 2") != std::string::npos);
}

TEST_CASE("RapidOBJ attribute fast paths preserve short UVs exact doubles and comments") {
    const PrototypeFixture fixture;
    const std::string text = "v 0 0 0\nvt +.123456789012345# short\r\nvt .5 .25 .9# third\n"
        "vn +1e0 -0 .123456789012345# normal\\\r\nvn 0 \\\n1 0\n";
    const auto path = fixture.write(text);
    for (size_t block : {size_t{3}, size_t{31}, size_t{4 * 1024 * 1024}}) {
        for (int source = 0; source < 3; ++source) {
            rapidobj::PrototypeOptions options; options.chunk_bytes = block; options.read_bytes = 2; options.workers = 3;
            std::vector<woby::Coordinate> positions, normals;
            std::vector<std::array<double, 2>> texcoords;
            std::istringstream stream(text);
            rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
            const auto parsed = source == 0 ? rapidobj::ParseMemoryPrototype(text, buffers, rapidobj::MaterialLibrary::Ignore(), options)
                : source == 1 ? rapidobj::ParseStreamPrototype(stream, buffers, rapidobj::MaterialLibrary::Ignore(), options)
                : rapidobj::ParseFilePrototype(path, buffers, options);
            REQUIRE_FALSE(parsed.polygons.error);
            REQUIRE(texcoords.size() == 2); REQUIRE(normals.size() == 2);
            CHECK(texcoords[0] == std::array<double, 2>{.123456789012345, 0});
            CHECK(texcoords[1] == std::array<double, 2>{.5, .25});
            CHECK(normals[0] == woby::Coordinate{1, -0.0, .123456789012345});
            CHECK(std::signbit(normals[0][1]));
            CHECK(normals[1] == woby::Coordinate{0, 1, 0});
        }
    }
    for (const std::string line : {"vt", "vt .1 .2 .3 .4", "vt .1x", "vn 1 2", "vn 1 2 3 4", "vn 1 2 3x"}) {
        CHECK_FALSE(failureLine("v 0 0 0\n" + line + "\n", {17, 2, 3}).empty());
    }
}

TEST_CASE("RapidOBJ prototype joins native block reads when cancellation throws") {
    const PrototypeFixture fixture;
    rapidobj::PrototypeOptions options; options.workers = 4;
    size_t vertices = 10000;
    SUBCASE("small native blocks") { options.chunk_bytes = 4096; }
    SUBCASE("persistent native ranges") { options.chunk_bytes = 4 * 1024 * 1024; vertices = 200000; }
    std::string text;
    for (size_t i = 0; i < vertices; ++i) { text += "v 0 0 0\n"; }
    const auto path = fixture.write(text);
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    struct Checkpoints { size_t calls = 0; std::thread::id caller = std::this_thread::get_id(); } checkpoints;
    options.user = &checkpoints;
    options.checkpoint = [](void* data) {
        auto& state = *static_cast<Checkpoints*>(data);
        REQUIRE(std::this_thread::get_id() == state.caller);
        if (++state.calls == 7) { throw std::runtime_error("cancel native blocks"); }
    };
    CHECK_THROWS_WITH(rapidobj::ParseFilePrototype(path, {positions, texcoords, normals}, options), "cancel native blocks");
    CHECK(positions.empty()); CHECK(texcoords.empty()); CHECK(normals.empty());
    // Windows rejects deleting a file while this reader still owns its handle.
    CHECK(std::filesystem::remove(path));
}

TEST_CASE("RapidOBJ tuple construction joins index validation before returning failure") {
    const PrototypeFixture fixture;
    rapidobj::PrototypeOptions options;
    SUBCASE("one worker") { options.workers = 1; }
    SUBCASE("parallel workers") { options.workers = 4; }
    std::string text;
    for (size_t i = 0; i < 100000; ++i) { text += "v 1.25 2.5 3.75\np -1\n"; }
    text += "p 100001\n"; // Global index validation fails during merge.
    const auto path = fixture.write(text);
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    positions.reserve(100000);
    const auto* storage = positions.data();
    const auto parsed = rapidobj::ParseFilePrototype(path, {positions, texcoords, normals}, options);
    REQUIRE(parsed.polygons.error);
    CHECK(parsed.polygons.error.code == rapidobj::make_error_code(rapidobj::rapidobj_errc::IndexOutOfBoundsError));
    CHECK(positions.empty()); CHECK(texcoords.empty()); CHECK(normals.empty());
    CHECK(positions.data() == storage);
    const auto retry = rapidobj::ParseMemoryPrototype("v 4 5 6\n", {positions, texcoords, normals},
        rapidobj::MaterialLibrary::Ignore(), options);
    REQUIRE_FALSE(retry.polygons.error);
    REQUIRE(positions.size() == 1);
    CHECK(positions.front() == woby::Coordinate{4, 5, 6});
    CHECK(positions.data() == storage);
}

TEST_CASE("OBJ simple faces preserve attributed relative and larger face fallback") {
    const PrototypeFixture fixture;
    const std::string positions = "v 0 0 0\nv 1 0 0\nv 2 1 0\nv 1 2 0\nv 0 1 0\nvt .25 .5\nvn 0 0 1\n";
    const std::string text = "g forward\nf 1 2 3\n" + positions
        + "s 3\ng plain\nf\t1 2\t3 4 # quad\r\nf 1 2 3 4 5\n"
          "g attributed\nf 1/1/1 2/1/1 3/1/1\nf -5 -4 -3\nf 1 2 \\\r\n3\n";
    const auto path = fixture.write(text);
    const auto reference = woby::loadObjMeshLegacy(fixture.write("g forward\nf 1 2 3\n" + positions
        + "s 3\ng plain\nf 1 2 3 4\nf 1 2 3 4 5\n"
          "g attributed\nf 1/1/1 2/1/1 3/1/1\nf -5 -4 -3\nf 1 2 3\n", "reference.obj"));
    for (size_t block : {size_t{3}, size_t{64}, size_t{4 * 1024 * 1024}}) {
        sameMesh(reference, woby::loadObjMeshPrototype(path, {}, {block, 2, 3}));
        sameMesh(reference, woby::loadObjMeshTextPrototype(text, {}, {block, 2, 3}));
    }
    for (const std::string face : {"f 1 2", "f 1 2 0", "f 1 2 6", "f 1 2 2147483648", "f 1 2 3x", "f 1 2 3/"}) {
        CHECK_FALSE(failureLine(positions + face + "\n", {23, 2, 3}).empty());
    }
}

TEST_CASE("RapidOBJ prototype allows overlapping read-only opens of the same file") {
    const PrototypeFixture fixture;
    rapidobj::PrototypeOptions options; options.workers = 3;
    std::string text = polygons;
    SUBCASE("small native blocks") { options.chunk_bytes = 31; }
    SUBCASE("persistent native ranges") {
        options.chunk_bytes = 4 * 1024 * 1024;
        while (text.size() <= 1024 * 1024) { text += "# read sharing\n"; }
    }
    const auto path = fixture.write(text);
    // Keep the first handle alive: this catches exclusive sharing without a
    // timing-dependent race between two background loaders.
    const rapidobj::detail::sys::File firstReader(path, true);
    REQUIRE(static_cast<bool>(firstReader));
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    const auto parsed = rapidobj::ParseFilePrototype(path, {positions, texcoords, normals}, options);
    REQUIRE_FALSE(parsed.polygons.error);
    CHECK(positions.size() == 7);
}

TEST_CASE("RapidOBJ prototype bounds reconstructed statements across tiny blocks") {
    rapidobj::PrototypeOptions options;
    options.chunk_bytes = 3; options.read_bytes = 2; options.workers = 3; options.max_statement_bytes = 16;
    std::vector<woby::Coordinate> positions, normals;
    std::vector<std::array<double, 2>> texcoords;
    rapidobj::GeometryBuffers buffers{positions, texcoords, normals};
    // Trailing whitespace after a continuation is discarded. The raw text can
    // exceed the logical limit while each physical line stays within its limit.
    const std::string valid = "v 0 \\        \r\n0 \\        \r\n0\n";
    const auto parsed = rapidobj::ParseMemoryPrototype(valid, buffers, rapidobj::MaterialLibrary::Ignore(), options);
    REQUIRE_FALSE(parsed.polygons.error);
    REQUIRE(positions.size() == 1);
    CHECK(positions[0] == woby::Coordinate{0, 0, 0});
    positions.clear();
    const auto oversized = rapidobj::ParseMemoryPrototype("v 0 0 \\\n          \\\n0\n", buffers,
        rapidobj::MaterialLibrary::Ignore(), options);
    REQUIRE(oversized.polygons.error);
    CHECK(oversized.polygons.error.code == rapidobj::make_error_code(rapidobj::rapidobj_errc::LineTooLongError));
    CHECK(oversized.polygons.error.line_num == 1);
    CHECK(positions.empty());
    // Only the CR belonging to CRLF is removed before detecting continuation.
    CHECK_THROWS((void)woby::loadObjMeshTextPrototype("v 0 0 \\\r\r\n0\n", {}, {4, 2, 2}));
}
