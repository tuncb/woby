#include "obj_mesh.h"
#include "obj_corner_mapping.h"
#include "freeform.h"
#include "utf8_path.h"
#include <rapidobj/prototype.hpp>
#include <doctest/doctest.h>
#include <chrono>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <limits>
#include <sstream>
#include <thread>

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
    REQUIRE(meshGeometry(a).vertices.size() == meshGeometry(b).vertices.size());
    for (size_t i = 0; i < meshGeometry(a).vertices.size(); ++i) { CHECK(woby::meshPosition(a, i) == woby::meshPosition(b, i)); }
    CHECK(meshGeometry(a).indices == meshGeometry(b).indices); CHECK(meshGeometry(a).lineIndices == meshGeometry(b).lineIndices); CHECK(meshGeometry(a).pointIndices == meshGeometry(b).pointIndices);
    CHECK(meshGeometry(a).vertexColors == meshGeometry(b).vertexColors);
    REQUIRE(meshGeometry(a).vertices.size() == meshGeometry(b).vertices.size());
    for (size_t i = 0; i < meshGeometry(a).vertices.size(); ++i) {
        CHECK(meshGeometry(a).vertices[i].position == meshGeometry(b).vertices[i].position);
        CHECK(meshGeometry(a).vertices[i].normal == meshGeometry(b).vertices[i].normal);
        CHECK(meshGeometry(a).vertices[i].texcoord == meshGeometry(b).vertices[i].texcoord);
    }
    REQUIRE(a.sourceData); REQUIRE(b.sourceData);
    CHECK(woby::sourcePositions(*a.sourceData) == woby::sourcePositions(*b.sourceData));
    CHECK(a.sourceData->indices == b.sourceData->indices);
    CHECK(a.sourceData->originalPointIds == b.sourceData->originalPointIds);
    REQUIRE(meshGeometry(a).nodes.size() == meshGeometry(b).nodes.size());
    for (size_t i = 0; i < meshGeometry(a).nodes.size(); ++i) {
        CHECK(meshGeometry(a).nodes[i].name == meshGeometry(b).nodes[i].name);
        CHECK(meshGeometry(a).nodes[i].indexOffset == meshGeometry(b).nodes[i].indexOffset);
        CHECK(meshGeometry(a).nodes[i].indexCount == meshGeometry(b).nodes[i].indexCount);
        CHECK(meshGeometry(a).nodes[i].lineIndexOffset == meshGeometry(b).nodes[i].lineIndexOffset);
        CHECK(meshGeometry(a).nodes[i].lineIndexCount == meshGeometry(b).nodes[i].lineIndexCount);
        CHECK(meshGeometry(a).nodes[i].pointIndexOffset == meshGeometry(b).nodes[i].pointIndexOffset);
        CHECK(meshGeometry(a).nodes[i].pointIndexCount == meshGeometry(b).nodes[i].pointIndexCount);
        CHECK(meshGeometry(a).nodes[i].hasTexcoords == meshGeometry(b).nodes[i].hasTexcoords);
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

TEST_CASE("Staged OBJ corners preserve global first-use IDs attributes and group UV flags") {
    const std::string text = "v 99 99 99\nv 1 0 0\nv 0 1 0\nv 0 0 0\n"
        "vt .125 .25\nvt .5 .75\nvt 1 0\nvt 1e40 .5\nvn 0 0 1\nvn 0 0 -1\n"
        "g first\nf 4/2/1 2/1/1 3/3/1\n"
        "g second\nf 3/3/2 2/1/1 4/4/1\n"
        "g reuse\nf 4/2/1 2/1/1 3/3/1\n";
    for (const bool deferred : {false,true}) {
        woby::ObjPrototypeOptions options; options.deferPreparation = deferred;
        const auto reference = woby::loadObjMeshTextPrototype(text,{},options);
        for (const size_t workers : {size_t{1},size_t{8}}) {
            options.gatherWorkers = workers; woby::ObjPrototypeMetrics metrics;
            const auto mesh = woby::loadObjMeshTextPrototype(text,{},options,&metrics);
            sameMesh(reference,mesh);
            CHECK(metrics.mapping.staged); CHECK(metrics.mapping.releasedTriangleBytes > 0);
            CHECK(metrics.mapping.primaryCapacityBytes == 4*12);
            CHECK(meshGeometry(mesh).indices == std::vector<uint32_t>{0,1,2,3,1,4,0,1,2});
            CHECK(meshGeometry(mesh).positionIndices == std::vector<uint32_t>{3,1,2,2,3});
            REQUIRE(meshGeometry(mesh).nodes.size() == 3);
            CHECK(meshGeometry(mesh).nodes[0].hasTexcoords);
            CHECK_FALSE(meshGeometry(mesh).nodes[1].hasTexcoords);
            CHECK(meshGeometry(mesh).nodes[2].hasTexcoords);
            CHECK(mesh.sourceData->indicesPending == deferred);
        }
    }
}

TEST_CASE("Staged OBJ mapping retains identity positions with unused trailing source records") {
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 99 99 99\nv 98 98 98\n"
        "vn 0 0 1\nf 1//1 2//1 3//1\nf 3//1 2//1 1//1\n";
    woby::ObjPrototypeMetrics metrics;
    const auto mesh = woby::loadObjMeshTextPrototype(text,{}, {},&metrics);
    CHECK(metrics.mapping.staged);
    CHECK(meshGeometry(mesh).positionIndexing == woby::PositionIndexing::identity);
    CHECK(meshGeometry(mesh).positionIndices.empty());
    REQUIRE(mesh.sourceData); CHECK(woby::sourcePositions(*mesh.sourceData).size() == 5);
    CHECK(meshGeometry(mesh).indices == std::vector<uint32_t>{0,1,2,2,1,0});
}

TEST_CASE("OBJ mapping uses the common resolver for sparse positions mixed primitives and freeforms") {
    std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    bool sparse = false, attributes = true, freeform = false;
    SUBCASE("many unused source positions") { sparse = true; }
    SUBCASE("explicit lines and points") {}
    SUBCASE("position-only triangles") { attributes = false; }
    SUBCASE("freeform patches") { freeform = true; }
    if (sparse) { for (size_t i = 0; i < 40; ++i) { text += "v 99 99 99\n"; } }
    if (attributes) { text += "vt .125 .25\nvt .5 .75\nvn 0 0 1\nf 3/1/1 1/1/1 2/2/1\nf 2/2/1 1/2/1 3/1/1\n"; }
    else { text += "f 3 1 2\nf 2 1 3\n"; }
    if (!sparse && attributes) { text += "l 1 2\np 3\n"; }
    if (freeform) {
        text = rationalTrimmed;
        const auto primitives = text.find("l 1 3\np 2\n");
        REQUIRE(primitives != std::string::npos); text.erase(primitives,std::string_view("l 1 3\np 2\n").size());
    }
    woby::ObjPrototypeOptions options;
    const PrototypeFixture fixture;
    const auto reference = woby::loadObjMeshLegacy(fixture.write(text));
    woby::ObjPrototypeMetrics metrics;
    const auto mesh = woby::loadObjMeshTextPrototype(text,{},options,&metrics);
    sameMesh(reference,mesh); CHECK(metrics.mapping.staged);
    CHECK(metrics.mapping.releasedTriangleBytes > 0);
    if (!freeform) { CHECK(metrics.mapping.primaryCapacityBytes == woby::sourcePositions(*mesh.sourceData).size()*sizeof(woby::obj_mapping::Primary)); }
}

TEST_CASE("OBJ primitives preserve face IDs authored normals and unlit vertex normals") {
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
        "vt .25 .5\nvn 0 1 0\ng part\nf 3/1/1 1/1/1 2/1/1\nl 3 4 1\np 4 3 4\n";
    for (const bool deferred : {false,true}) {
        woby::ObjPrototypeOptions options; options.deferPreparation = deferred;
        woby::ObjPrototypeMetrics metrics;
        const auto mesh = woby::loadObjMeshTextPrototype(text,{},options,&metrics);
        const auto& geometry = meshGeometry(mesh);
        CHECK(metrics.mapping.staged);
        CHECK(geometry.indices == std::vector<uint32_t>{0,1,2});
        CHECK(metrics.mapping.seamVertices == 2);
        CHECK(geometry.lineIndices == std::vector<uint32_t>{3,4,4,5});
        CHECK(geometry.pointIndices == std::vector<uint32_t>{4,3,4});
        CHECK(geometry.positionIndices == std::vector<uint32_t>{2,0,1,2,3,0});
        REQUIRE(geometry.vertices.size() == 6);
        for (size_t i = 0; i < geometry.vertices.size(); ++i) {
            CHECK(geometry.vertices[i].normal == (i < 3 ? std::array<float,3>{0,1,0} : std::array<float,3>{0,0,1}));
            CHECK(geometry.vertices[i].texcoord == (i < 3 ? std::array<float,2>{.25f,.5f} : std::array<float,2>{}));
        }
        REQUIRE(geometry.nodes.size() == 3);
        CHECK(geometry.nodes[0].name == "part"); CHECK(geometry.nodes[0].hasTexcoords);
        CHECK(geometry.nodes[1].name == "part (lines)"); CHECK(geometry.nodes[1].lineIndexCount == 4);
        CHECK(geometry.nodes[2].name == "part (points)"); CHECK(geometry.nodes[2].pointIndexCount == 3);
        CHECK_FALSE(geometry.nodes[1].hasTexcoords); CHECK_FALSE(geometry.nodes[2].hasTexcoords);
        CHECK(mesh.sourceData->indices == (deferred ? std::vector<uint32_t>{} : std::vector<uint32_t>{2,0,1}));
    }
}

TEST_CASE("OBJ untextured faces share primitive vertices after all face groups") {
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
        "g a\nl 4 1\np 2\ng a (lines)\nf 3 1 2\ng a\nf 4 2 1\n";
    const auto mesh = woby::loadObjMeshTextPrototype(text);
    const auto& geometry = meshGeometry(mesh);
    CHECK(geometry.indices == std::vector<uint32_t>{0,1,2,3,2,1});
    CHECK(geometry.lineIndices == std::vector<uint32_t>{3,1});
    CHECK(geometry.pointIndices == std::vector<uint32_t>{2});
    CHECK(geometry.positionIndices == std::vector<uint32_t>{2,0,1,3});
    REQUIRE(geometry.vertices.size() == 4); REQUIRE(geometry.nodes.size() == 4);
    CHECK(geometry.nodes[0].name == "a (lines)"); CHECK(geometry.nodes[0].indexOffset == 0);
    CHECK(geometry.nodes[1].name == "a"); CHECK(geometry.nodes[1].indexOffset == 3);
    CHECK(geometry.nodes[2].name == "a (2)"); CHECK(geometry.nodes[2].lineIndexCount == 2);
    CHECK(geometry.nodes[3].name == "a (points)"); CHECK(geometry.nodes[3].pointIndexCount == 1);
    for (const auto& vertex : geometry.vertices) { CHECK(woby::validNormal(vertex.normal)); }
}

TEST_CASE("OBJ line and point files retain repeated references and ignore unused attributes") {
    const std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\n"
        "vt .25 .5\nvn 0 1 0\ng duplicates\nl 4 2 4\np 2 1 4 1\n";
    const auto mesh = woby::loadObjMeshTextPrototype(text);
    const auto& geometry = meshGeometry(mesh);
    CHECK(geometry.indices.empty());
    CHECK(geometry.lineIndices == std::vector<uint32_t>{0,1,1,0});
    CHECK(geometry.pointIndices == std::vector<uint32_t>{1,2,0,2});
    CHECK(geometry.positionIndices == std::vector<uint32_t>{3,1,0});
    REQUIRE(geometry.vertices.size() == 3);
    for (const auto& vertex : geometry.vertices) {
        CHECK(vertex.normal == std::array<float,3>{0,0,1});
        CHECK(vertex.texcoord == std::array<float,2>{});
    }
    CHECK(mesh.sourceData->indices.empty());
}

TEST_CASE("OBJ clouds fill source-order IDs in parallel without a lookup table") {
    constexpr size_t count = 65536+17;
    std::string text = "vn 0 0 0\nvt .25 .5\n";
    for (size_t i = 0; i < count; ++i) { text += "v "+std::to_string(i%37)+" "+std::to_string(i%23)+" 0\n"; }
    const auto caller = std::this_thread::get_id(); std::atomic<bool> wrongThread = false;
    const auto progress = [&](const woby::ModelLoadProgress& value) {
        if (value.stage == woby::ModelLoadStage::buildingMesh) {
            if (std::this_thread::get_id() != caller) { wrongThread.store(true); }
            CHECK(value.completed <= value.total);
        }
    };
    for (const bool deferred : {false,true}) {
        woby::ObjPrototypeOptions options; options.gatherWorkers = 8; options.deferPreparation = deferred;
        woby::ObjPrototypeMetrics metrics;
        const auto mesh = woby::loadObjMeshTextPrototype(text,progress,options,&metrics);
        const auto& geometry = meshGeometry(mesh);
        REQUIRE(geometry.vertices.size() == count); REQUIRE(geometry.pointIndices.size() == count);
        CHECK(geometry.indices.empty()); CHECK(geometry.lineIndices.empty());
        CHECK(geometry.positionIndexing == woby::PositionIndexing::identity); CHECK(geometry.positionIndices.empty());
        CHECK(metrics.mapping.primaryCapacityBytes == 0); CHECK(metrics.mapping.keyCapacityBytes == 0);
        CHECK(metrics.mapping.bucketCapacityBytes == 0); CHECK(metrics.mapping.gatherWorkers == 2);
        for (size_t i = 0; i < count; ++i) {
            CHECK(geometry.pointIndices[i] == i);
            CHECK(geometry.vertices[i].position == woby::renderPosition(woby::sourcePositions(*mesh.sourceData)[i]));
            CHECK(geometry.vertices[i].normal == std::array<float,3>{0,0,1});
        }
        REQUIRE(geometry.nodes.size() == 1); CHECK(geometry.nodes[0].name == "Points");
        CHECK(mesh.sourceData->indicesPending == deferred); CHECK(mesh.sourceData->indices.empty());
    }
    CHECK_FALSE(wrongThread.load());
    CHECK_THROWS_WITH((void)woby::loadObjMeshTextPrototype(text,[](const woby::ModelLoadProgress& value) {
        if (value.stage == woby::ModelLoadStage::buildingMesh && value.total && value.completed == value.total) {
            throw std::runtime_error("cancel cloud gather");
        }
    }),"cancel cloud gather");
}

TEST_CASE("OBJ point resolution polls cancellation before attribute gathering") {
    std::string text = "v 0 0 0\nv 1 0 0\nv 0 1 0\n";
    for (size_t i = 0; i < 40000; ++i) { text += "p 1\n"; }
    CHECK_THROWS_WITH((void)woby::loadObjMeshTextPrototype(text,[](const woby::ModelLoadProgress& value) {
        if (value.stage == woby::ModelLoadStage::buildingMesh && value.completed && value.completed < value.total) {
            throw std::runtime_error("cancel point IDs");
        }
    }),"cancel point IDs");
}

TEST_CASE("OBJ freeform-only input does not allocate an unused position lookup") {
    auto text = rationalTrimmed;
    const std::string primitives = "f 1/1/1 2/2/1 4/4/1\nl 1 3\np 2\n";
    const auto begin = text.find(primitives); REQUIRE(begin != std::string::npos);
    text.erase(begin,primitives.size());
    for (const bool deferred : {false,true}) {
        woby::ObjPrototypeOptions options; options.deferPreparation = deferred;
        woby::ObjPrototypeMetrics metrics;
        const auto mesh = woby::loadObjMeshTextPrototype(text,{},options,&metrics);
        REQUIRE(mesh.freeform); CHECK(mesh.freeform->patches.size() == 1);
        CHECK(metrics.mapping.staged); CHECK(metrics.mapping.primaryCapacityBytes == 0);
        CHECK(metrics.mapping.keyCapacityBytes == 0); CHECK(metrics.mapping.bucketCapacityBytes == 0);
        REQUIRE(mesh.sourceData); CHECK(mesh.sourceData->indicesPending == deferred);
        CHECK(mesh.sourceData->indices.size() == (deferred ? 0 : meshGeometry(mesh).indices.size()));
    }
}

TEST_CASE("Parallel OBJ gathering is byte exact and reports progress only on the caller") {
    std::string text; constexpr size_t count = 3*65536;
    text.reserve(count*26);
    for (size_t i = 0; i < count; ++i) { text += "v "+std::to_string(i%997)+" "+std::to_string(i/997)+" 0\n"; }
    text += "vt .125 .25\nvn 0 0 1\n";
    for (size_t i = 1; i < count; i += 3) {
        text += "f "+std::to_string(i)+"/1/1 "+std::to_string(i+1)+"/1/1 "+std::to_string(i+2)+"/1/1\n";
    }
    const auto caller = std::this_thread::get_id(); std::atomic<bool> wrongThread = false;
    const auto progress = [&](const woby::ModelLoadProgress& p) {
        if (p.stage == woby::ModelLoadStage::buildingMesh && std::this_thread::get_id() != caller) { wrongThread.store(true); }
    };
    woby::ObjPrototypeOptions options; options.gatherWorkers = 1; options.deferPreparation = true;
    woby::ObjPrototypeMetrics serialMetrics, parallelMetrics;
    const auto serial = woby::loadObjMeshTextPrototype(text,progress,options,&serialMetrics);
    options.gatherWorkers = 8;
    const auto parallel = woby::loadObjMeshTextPrototype(text,progress,options,&parallelMetrics);
    CHECK(serialMetrics.mapping.staged); CHECK(serialMetrics.mapping.gatherWorkers == 1);
    CHECK(parallelMetrics.mapping.gatherWorkers == 3); CHECK_FALSE(wrongThread.load());
    const auto& a = meshGeometry(serial); const auto& b = meshGeometry(parallel);
    REQUIRE(a.vertices.size() == b.vertices.size());
    CHECK(std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(woby::Vertex)) == 0);
    CHECK(a.indices == b.indices); CHECK(a.positionIndices == b.positionIndices);
    CHECK(woby::sourcePositions(*serial.sourceData) == woby::sourcePositions(*parallel.sourceData));
    CHECK(serial.bounds.min == parallel.bounds.min); CHECK(serial.bounds.max == parallel.bounds.max);
    size_t terminalCalls = 0;
    CHECK_THROWS_WITH((void)woby::loadObjMeshTextPrototype(text,[&](const woby::ModelLoadProgress& p) {
        if (p.stage == woby::ModelLoadStage::buildingMesh && p.total && p.completed == p.total && ++terminalCalls == 6) {
            throw std::runtime_error("cancel attribute gathering");
        }
    },options),"cancel attribute gathering");
    CHECK(terminalCalls == 6);
}

TEST_CASE("OBJ gathering validates worker limits before parsing") {
    woby::ObjPrototypeOptions options; options.gatherWorkers = 9;
    CHECK_THROWS_AS((void)woby::loadObjMeshTextPrototype("malformed",{},options),std::invalid_argument);
}

TEST_CASE("Parallel OBJ resolution keeps full meshes colors and mixed primitive identities") {
    std::string text;
    constexpr size_t positions = 65539, faces = 349527;
    text.reserve(32*1024*1024);
    for (size_t i = 0; i < positions; ++i) {
        text += "v "+std::to_string(i%997)+" "+std::to_string(i/997)+" 0 .25 .5 .75\n";
    }
    text += "vt .125 .25\nvt 1e40 .5\nvn 0 0 1\nvn 0 1 0\ng mixed\n";
    for (size_t face = 0; face < faces; ++face) {
        if (face == 180001) { text += "g mixed\n"; }
        text += "f";
        for (size_t corner = 0; corner < 3; ++corner) {
            const auto position = ((face*3+corner)*71)%65537+1;
            text += " "+std::to_string(position)+(face%9999 == 0 ? "/2/2" : "/1/1");
        }
        text += '\n';
    }
    text += "l 2 65539 3\np 1 65538\n";
    woby::ObjPrototypeOptions options;
    bool deferred = false;
    SUBCASE("immediate source indices") {}
    SUBCASE("deferred source indices") { deferred = true; }
    options.deferPreparation = deferred;
    woby::ObjPrototypeMetrics serialMetrics, parallelMetrics;
    woby::Mesh serial;
    {
        struct ReservedWorkers {
            unsigned previous = woby::analysisWorkerCount.exchange(woby::analysisWorkerLimit());
            ~ReservedWorkers() { woby::analysisWorkerCount.store(previous); }
        } reserved;
        serial = woby::loadObjMeshTextPrototype(text,{},options,&serialMetrics);
    }
    const auto parallel = woby::loadObjMeshTextPrototype(text,{},options,&parallelMetrics);
    CHECK(serialMetrics.mapping.resolveWorkers == 1);
    if (woby::analysisWorkerLimit() >= 3) {
        CHECK(parallelMetrics.mapping.resolveWorkers >= 4);
        CHECK(parallelMetrics.mapping.resolveScratchBytes > 0);
    } else { CHECK(parallelMetrics.mapping.resolveWorkers == 1); }
    sameMesh(serial,parallel);
    const auto& a = woby::meshGeometry(serial); const auto& b = woby::meshGeometry(parallel);
    CHECK(a.positionIndices == b.positionIndices);
    CHECK(std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(woby::Vertex)) == 0);
}

namespace {
template<bool Precise> void checkResolvedCorners(bool deferred, bool finiteUvs, bool parallel = false) {
    std::vector<woby::Coordinate> positions(37), normals;
    std::vector<std::array<double,2>> uvs{{.125,.25},
        {std::numeric_limits<double>::quiet_NaN(),.5}, {std::numeric_limits<double>::infinity(),.5},
        {-std::numeric_limits<double>::infinity(),0}, {1e40,.5},
        {std::numeric_limits<float>::max(),.5}, {-0.0,-0.0}};
    if (finiteUvs) { for (auto& uv : uvs) { uv = {.125,.25}; } }
    rapidobj::GeometryBuffers buffers{positions,uvs,normals};
    rapidobj::Result input;
    input.attributes.texcoords = rapidobj::Array<float>(uvs.size()*2);
    for (size_t i = 0; i < uvs.size(); ++i) {
        for (size_t axis = 0; axis < 2; ++axis) { input.attributes.texcoords[i*2+axis] = static_cast<float>(uvs[i][axis]); }
    }
    woby::SourceMeshData source; source.draftPositions.resize(positions.size());
    std::map<std::array<int,3>,uint32_t> firstUse;
    std::vector<uint32_t> expected, sourceIndices;
    std::vector<woby::MeshNode> nodes;
    // Empty shapes and both sides of the prefetch and progress boundaries.
    for (const size_t count : {size_t{0},size_t{1},size_t{31},size_t{32},size_t{33},
        size_t{63},size_t{64},size_t{65},size_t{16385},size_t{65535},size_t{65536},size_t{65537},size_t{2},size_t{0}}) {
        const auto shapeIndex = input.shapes.size();
        auto& shape = input.shapes.emplace_back();
        shape.mesh.indices = rapidobj::Array<rapidobj::Index>(count);
        if (shapeIndex%2) { shape.name = "group "+std::to_string(shapeIndex); }
        woby::MeshNode node; node.name = shape.name.empty() ? "shape "+std::to_string(shapeIndex+1) : shape.name;
        node.indexOffset = static_cast<uint32_t>(expected.size());
        node.indexCount = static_cast<uint32_t>(count); node.hasTexcoords = true;
        for (size_t i = 0; i < count; ++i) {
            const auto corner = expected.size();
            const int position = static_cast<int>((corner*17+corner/7)%positions.size());
            const int normal = corner%5 ? static_cast<int>(corner%3) : -1;
            const int uv = shapeIndex%3 == 2 ? 0
                : corner%13 ? static_cast<int>((corner*5+corner/19)%uvs.size()) : -1;
            shape.mesh.indices[i] = {position,uv,normal};
            const std::array<int,3> key{position,normal,uv};
            const auto [entry,inserted] = firstUse.try_emplace(key,static_cast<uint32_t>(firstUse.size()));
            (void)inserted;
            expected.push_back(entry->second); sourceIndices.push_back(static_cast<uint32_t>(position));
            const bool finite = uv >= 0 && std::isfinite(static_cast<float>(uvs[static_cast<size_t>(uv)][0]))
                && std::isfinite(static_cast<float>(uvs[static_cast<size_t>(uv)][1]));
            node.hasTexcoords = node.hasTexcoords && finite;
        }
        if (count) { nodes.push_back(std::move(node)); }
    }
    woby::obj_mapping::Table table; woby::MeshGeometry geometry;
    const auto caller = std::this_thread::get_id();
    size_t callbacks = 0;
    const auto progress = [&](const woby::ModelLoadProgress& p) {
        CHECK(std::this_thread::get_id() == caller); CHECK(p.completed <= p.total); ++callbacks;
    };
    if (parallel) {
        woby::obj_mapping::resolveParallelFaces<Precise>(input,table,geometry,source,&buffers,progress,expected.size(),deferred,8);
    } else {
        woby::obj_mapping::resolveFaces<Precise>(input,table,geometry,source,&buffers,progress,expected.size(),deferred);
    }
    CHECK(callbacks > 0);
    CHECK(geometry.indices == expected); CHECK(table.next == firstUse.size());
    CHECK(source.indices == (deferred ? std::vector<uint32_t>{} : sourceIndices));
    REQUIRE(geometry.nodes.size() == nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        CHECK(geometry.nodes[i].name == nodes[i].name);
        CHECK(geometry.nodes[i].indexOffset == nodes[i].indexOffset);
        CHECK(geometry.nodes[i].indexCount == nodes[i].indexCount);
        CHECK(geometry.nodes[i].hasTexcoords == nodes[i].hasTexcoords);
    }
}

TEST_CASE("Parallel OBJ discovery preserves ordered IDs seams and group UV flags") {
    bool finiteUvs = false;
    SUBCASE("non-finite UV values") {}
    SUBCASE("finite UV values and missing corners") { finiteUvs = true; }
    for (const bool deferred : {false,true}) {
        checkResolvedCorners<true>(deferred,finiteUvs,true);
        checkResolvedCorners<false>(deferred,finiteUvs,true);
    }
}

TEST_CASE("Parallel OBJ discovery joins workers on cancellation and invalid positions") {
    woby::SourceMeshData source; source.draftPositions.resize(65539);
    rapidobj::Result input; auto& shape = input.shapes.emplace_back();
    constexpr size_t count = 262147;
    shape.mesh.indices = rapidobj::Array<rapidobj::Index>(count);
    for (size_t i = 0; i < count; ++i) { shape.mesh.indices[i] = {static_cast<int>((i*71)%65537),-1,-1}; }
    size_t cancelCall = 0; bool cancelOutput = false, invalid = false;
    SUBCASE("negative position") { shape.mesh.indices[count-1].position_index = -1; invalid = true; }
    SUBCASE("position beyond source") { shape.mesh.indices[count-1].position_index = 65539; invalid = true; }
    SUBCASE("first callback") { cancelCall = 1; }
    SUBCASE("partition callback") { cancelCall = 8; }
    SUBCASE("discovery callback") { cancelCall = 64; }
    SUBCASE("index output callback") { cancelOutput = true; }
    const auto active = woby::analysisWorkerCount.load();
    const auto caller = std::this_thread::get_id();
    size_t callbacks = 0, completed = 0;
    woby::obj_mapping::Table table; woby::MeshGeometry geometry;
    const auto progress = [&](const woby::ModelLoadProgress& p) {
        CHECK(std::this_thread::get_id() == caller);
        CHECK(p.completed >= completed); completed = p.completed;
        CHECK(p.total == count+9);
        if (++callbacks == cancelCall || (cancelOutput && p.completed)) { throw std::runtime_error("cancel parallel IDs"); }
    };
    CHECK_THROWS_WITH(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,
        progress,count,false,8,1024,count+9),invalid ? "OBJ contains an invalid source position index." : "cancel parallel IDs");
    CHECK(woby::analysisWorkerCount.load() == active);
    // A failed operation must not leave helper capacity reserved.
    shape.mesh.indices[count-1].position_index = 0;
    table = {}; geometry = {};
    CHECK_NOTHROW(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},count,false,8));
    CHECK(woby::analysisWorkerCount.load() == active);
}

TEST_CASE("Parallel OBJ discovery handles empty input dense first uses and capacity limits") {
    woby::SourceMeshData source;
    rapidobj::Result input;
    woby::obj_mapping::Table table; woby::MeshGeometry geometry;
    CHECK_NOTHROW(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},0,true,8));
    CHECK(table.next == 0); CHECK(geometry.indices.empty()); CHECK(geometry.nodes.empty());
    CHECK_THROWS_AS(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},0,true,0),std::invalid_argument);
    CHECK_THROWS_AS(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},0,true,9),std::invalid_argument);
    CHECK_THROWS_AS(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},0,true,8,0),std::invalid_argument);
    CHECK_THROWS_WITH(woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},
        size_t(std::numeric_limits<woby::SceneBufferSize>::max())/sizeof(uint32_t)+1,true,8),
        "Scene exceeds the supported 32-bit GPU buffer size.");
    constexpr size_t count = 131075;
    source.draftPositions.resize(count+1);
    input.shapes.emplace_back().mesh.indices = rapidobj::Array<rapidobj::Index>(count);
    std::vector<uint32_t> expected(count);
    for (size_t i = 0; i < count; ++i) {
        input.shapes[0].mesh.indices[i] = {static_cast<int>(count-i),-1,-1};
        expected[i] = static_cast<uint32_t>(i);
    }
    for (const size_t workers : {size_t{1},size_t{8}}) {
        table = {}; geometry = {};
        woby::obj_mapping::resolveParallelFaces<false>(input,table,geometry,source,nullptr,{},count,true,workers);
        CHECK(geometry.indices == expected); CHECK(table.next == count); CHECK(table.seams.empty());
        CHECK(table.primary[0].id == woby::obj_mapping::empty);
        bool exact = true;
        for (size_t i = 0; i < count; ++i) { exact = exact && table.primary[count-i].id == (static_cast<uint32_t>(i)|woby::obj_mapping::invalidUv); }
        CHECK(exact);
    }
}
} // namespace

TEST_CASE("OBJ ID resolution preserves first use and UV flags across lookahead boundaries") {
    bool finiteUvs = false;
    SUBCASE("non-finite UV values") {}
    SUBCASE("finite UV values and missing corners") { finiteUvs = true; }
    for (const bool deferred : {false,true}) {
        CAPTURE(deferred);
        checkResolvedCorners<true>(deferred,finiteUvs);
        checkResolvedCorners<false>(deferred,finiteUvs);
    }
}

TEST_CASE("OBJ ID resolution rejects invalid positions and cancels on the caller") {
    woby::SourceMeshData source; source.draftPositions.resize(3);
    rapidobj::Result input; auto& shape = input.shapes.emplace_back();
    constexpr size_t count = 32770;
    shape.mesh.indices = rapidobj::Array<rapidobj::Index>(count);
    input.attributes.texcoords = rapidobj::Array<float>(2);
    input.attributes.texcoords[0] = std::numeric_limits<float>::infinity(); input.attributes.texcoords[1] = 0;
    for (auto& index : shape.mesh.indices) { index = {0,0,-1}; }
    bool cancel = false;
    SUBCASE("negative future position") { shape.mesh.indices[64].position_index = -1; }
    SUBCASE("out of range future position") { shape.mesh.indices[64].position_index = std::numeric_limits<int>::max(); }
    SUBCASE("caller cancellation") { cancel = true; }
    woby::obj_mapping::Table table; woby::MeshGeometry geometry;
    const auto caller = std::this_thread::get_id();
    size_t completed = 0;
    const auto progress = [&](const woby::ModelLoadProgress& p) {
        CHECK(std::this_thread::get_id() == caller); completed = p.completed;
        if (cancel && p.completed == 16384) { throw std::runtime_error("cancel ID resolution"); }
    };
    CHECK_THROWS_WITH(woby::obj_mapping::resolveFaces<false>(input,table,geometry,source,nullptr,progress,count,true),
        cancel ? "cancel ID resolution" : "OBJ contains an invalid source position index.");
    if (cancel) { CHECK(completed == 16384); }
}

TEST_CASE("OBJ UV validation checks the float conversion and bounds unused-record work") {
    std::vector<std::array<double,2>> precise(65537,{.125,-.25});
    std::vector<float> floats(precise.size()*2,.5f);
    bool finite = true;
    SUBCASE("finite values") {}
    SUBCASE("double values that overflow float") { precise.back()[1] = 1e40; finite = false; }
    SUBCASE("NaN values") { precise.back()[0] = std::numeric_limits<double>::quiet_NaN(); finite = false; }
    SUBCASE("infinite values") { precise.back()[1] = -std::numeric_limits<double>::infinity(); finite = false; }
    for (size_t i = 0; i < precise.size(); ++i) {
        for (size_t axis = 0; axis < 2; ++axis) { floats[i*2+axis] = static_cast<float>(precise[i][axis]); }
    }
    const auto caller = std::this_thread::get_id(); size_t callbacks = 0;
    const auto progress = [&](const woby::ModelLoadProgress& p) {
        CHECK(std::this_thread::get_id() == caller);
        CHECK(p.completed == 0); CHECK(p.total == precise.size()); ++callbacks;
    };
    CHECK(woby::obj_mapping::allUvsFinite<true>(precise.data(),nullptr,precise.size(),progress,precise.size()) == finite);
    CHECK(woby::obj_mapping::allUvsFinite<false>(nullptr,floats.data(),precise.size(),progress,precise.size()) == finite);
    CHECK(callbacks == 4);
    // Skip a scan that would visit more UV records than triangle corners.
    CHECK_FALSE(woby::obj_mapping::allUvsFinite<true>(nullptr,nullptr,precise.size(),{},precise.size()-1));
    CHECK_THROWS_WITH(woby::obj_mapping::allUvsFinite<true>(precise.data(),nullptr,precise.size(),
        [&](const woby::ModelLoadProgress&) { throw std::runtime_error("cancel UV validation"); },precise.size()),
        "cancel UV validation");
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
    CHECK(meshGeometry(triangle).vertices[0].texcoord == std::array<float, 2>{.25f, 1});
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
    REQUIRE(meshGeometry(valid).vertices.size() == 3);
    CHECK(meshGeometry(valid).vertices[0].texcoord == std::array<float, 2>{.25f, .5f});
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

TEST_CASE("OBJ source colors follow seams relative references lines and point clouds") {
    const std::string vertices = "v 0 0 0 1 0 0\nv 1 1 0 0 1 0\nv 2 0 0\nv 3 1 0 0 0 1\n";
    const std::string primitives = "vn 0 0 1\nvn 0 0 -1\nvt 0 0\nvt 1 1\n"
        "g faces\nf -4/1/1 -3/1/1 -2/1/1\nf 1/2/2 3/2/2 4/2/2\n"
        "g lines\nl -4 -3 -2 -1\ng points\np 4 2 3\n";
    const std::array<uint32_t, 4> expected{0xff0000ff, 0xff00ff00, 0, 0xffff0000};
    const PrototypeFixture fixture;
    for (const bool cloud : {false, true}) {
        const auto text = vertices + (cloud ? "" : primitives);
        for (const bool deferred : {false, true}) {
            woby::ObjPrototypeOptions options; options.chunkBytes = 31; options.workers = 4; options.deferPreparation = deferred;
            const auto mesh = woby::loadObjMeshTextPrototype(text, {}, options);
            const auto fromFile = woby::loadObjMeshPrototype(fixture.write(text), {}, options);
            sameMesh(mesh, fromFile);
            const auto& geometry = meshGeometry(mesh);
            REQUIRE(geometry.vertexColors.size() == geometry.vertices.size());
            for (size_t i = 0; i < geometry.vertices.size(); ++i) {
                const auto position = woby::originalPosition(woby::meshPosition(mesh, i), mesh.origin);
                REQUIRE(position[0] >= 0); REQUIRE(position[0] <= 3);
                CHECK(geometry.vertexColors[i] == expected[static_cast<size_t>(position[0])]);
            }
            if (!cloud) { CHECK(geometry.vertices.size() > 4); }
        }
    }
    CHECK(meshGeometry(woby::loadObjMeshTextPrototype("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n")).vertexColors.empty());
}

TEST_CASE("OBJ RGB clamps finite channels and generated freeform samples use part colors") {
    const auto colored = woby::loadObjMeshTextPrototype("v 0 0 0 -1 2 5e-1\nv 1 0 0 0 0 0\np 1 2\n"
        "cstype bezier\ndeg 1\ncurv 0 1 1 2\nparm u 0 1\nend\n");
    const auto& geometry = meshGeometry(colored);
    REQUIRE(geometry.vertexColors.size() == geometry.vertices.size());
    REQUIRE(geometry.vertexColors.size() > 2);
    CHECK(geometry.vertexColors[0] == 0xff80ff00);
    CHECK(geometry.vertexColors[1] == 0xff000000);
    CHECK(std::all_of(geometry.vertexColors.begin() + 2, geometry.vertexColors.end(), [](uint32_t value) { return value == 0; }));
}
