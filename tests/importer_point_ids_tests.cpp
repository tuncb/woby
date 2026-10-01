#include "automation_registry.h"
#include "importer_host.h"
#include "mesh_topology.h"
#include "model_load.h"

#include <doctest/doctest.h>
#include <fstream>

namespace {
struct PointIdFixture {
    std::filesystem::path root = std::filesystem::absolute(std::filesystem::temp_directory_path())
        / ("woby-point-ids-" + woby::automationRandomHex(8));
    PointIdFixture() {
        std::filesystem::create_directory(root);
        woby::unloadImporters();
        woby::loadImporter(WOBY_TEST_POINT_IDS_IMPORTER);
    }
    ~PointIdFixture() { woby::unloadImporters(); std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    std::filesystem::path file(const std::string& name) const {
        const auto path = root / (name + ".wpoint"); std::ofstream(path) << "fixture"; return path;
    }
    woby::Mesh mesh(const std::string& name = "shared") const { return woby::importModel(file(name), {}, {}).mesh; }
};

woby::DuplicateInput inputFor(const woby::Mesh& mesh)
{
    woby::DuplicateInput input;
    woby::DuplicateSource source;
    source.fileId = 1; source.name = "shared.wpoint"; source.data = mesh.sourceData; source.wholeFile = true;
    for (size_t i = 0; i < mesh.nodes.size(); ++i) {
        woby::SourcePartInstance part;
        part.partId = i + 1;
        part.firstIndex = mesh.nodes[i].indexOffset; part.indexCount = mesh.nodes[i].indexCount;
        part.transform = source.unusedPointTransform;
        source.parts.push_back(part);
    }
    input.sources.push_back(std::move(source));
    return input;
}
}

TEST_CASE("Importer point IDs suppress shared contacts while preserving closed volumes and render attributes")
{
    PointIdFixture f;
    const auto mesh = f.mesh();
    REQUIRE(mesh.sourceData);
    CHECK(mesh.sourceData->originalPointIds == std::vector<uint64_t>{0,UINT64_MAX,42,99,0,UINT64_MAX,42,100});
    REQUIRE(mesh.vertices.size() == 8);
    CHECK(mesh.sourceData->indices == mesh.indices);
    CHECK(mesh.vertices[0].normal == std::array<float,3>{0,0,-1});
    CHECK(mesh.vertices[4].normal == std::array<float,3>{0,0,1});
    CHECK(mesh.vertices[4].texcoord == std::array<float,2>{1,1});
    auto input = inputFor(mesh);
    const auto duplicates = woby::inspectDuplicates(input);
    CHECK(duplicates.points.duplicateCount == 0);
    CHECK(duplicates.points.findings.empty());
    CHECK(duplicates.triangles.duplicateCount == 0);
    const auto topology = woby::buildMeshTopology(input.sources);
    REQUIRE(topology.sources.size() == 1);
    CHECK(topology.sources[0].components.size() == 2);
    CHECK(topology.sources[0].vertices.size() == 8);
    CHECK(topology.boundaries.empty()); CHECK(topology.nonManifoldEdges.empty());
    CHECK(topology.nonManifoldVertices.empty()); CHECK(topology.windingEdges.empty());
    // Moving independent groups and rebasing retain source identity.
    input.sources[0].parts[1].transform[12] = 10;
    CHECK(woby::inspectDuplicates(input).points.duplicateCount == 0);
    auto rebased = mesh;
    woby::rebaseMesh(rebased, {10,20,30});
    CHECK(rebased.sourceData->originalPointIds == mesh.sourceData->originalPointIds);
    CHECK(woby::inspectDuplicates(inputFor(rebased)).points.duplicateCount == 0);
}

TEST_CASE("Importer point IDs retain real duplicates source scopes selection and every display occurrence")
{
    PointIdFixture f;
    auto input = inputFor(f.mesh("duplicate"));
    input.sources[0].parts[1].transform[12] = 10;
    auto result = woby::inspectDuplicates(input);
    CHECK(result.points.duplicateCount == 1);
    REQUIRE(result.points.findings.size() == 1);
    const auto& finding = result.points.findings[0];
    REQUIRE(finding.members.size() == 2);
    CHECK(finding.members[0].id == 0); CHECK(finding.members[1].id == 8);
    CHECK(finding.geometry.size() == 2); // Includes the suppressed alias in the moved group.
    CHECK(result.triangles.duplicateCount == 0);
    auto other = input.sources[0]; other.fileId = 2; input.sources.push_back(other);
    CHECK(woby::inspectDuplicates(input).points.duplicateCount == 2);
    input.sources.pop_back();
    input.sources[0].wholeFile = false; // Excludes the unused genuine duplicate.
    CHECK(woby::inspectDuplicates(input).points.duplicateCount == 0);
    // The selected alias must still count if the first vertex with its ID is unselected.
    auto data = std::make_shared<woby::SourceMeshData>(*input.sources[0].data);
    data->indices.insert(data->indices.end(), {8,5,6});
    input.sources[0].data = data;
    input.sources[0].parts.erase(input.sources[0].parts.begin());
    input.sources[0].parts[0].indexCount += 3;
    result = woby::inspectDuplicates(input);
    CHECK(result.points.duplicateCount == 1);
    REQUIRE(result.points.findings.size() == 1);
    REQUIRE(result.points.findings[0].members.size() == 2);
    CHECK(result.points.findings[0].members[0].id == 4);
    CHECK(result.points.findings[0].members[1].id == 8);
    // Explicitly distinct source IDs remain duplicates even across different groups.
    CHECK(woby::inspectDuplicates(inputFor(f.mesh("distinct"))).points.duplicateCount == 3);
}

TEST_CASE("Importer point IDs are optional and do not change duplicate triangle identity")
{
    PointIdFixture f;
    const auto absent = f.mesh("absent");
    CHECK(absent.sourceData->originalPointIds.empty());
    CHECK(woby::inspectDuplicates(inputFor(absent)).points.duplicateCount == 3);
    auto input = inputFor(f.mesh());
    auto data = std::make_shared<woby::SourceMeshData>(*input.sources[0].data);
    data->indices.insert(data->indices.end(), {0,1,2}); // Real reversed duplicate of the upper contact face.
    input.sources[0].data = data;
    input.sources[0].parts[1].indexCount += 3;
    const auto duplicates = woby::inspectDuplicates(input);
    CHECK(duplicates.points.duplicateCount == 0);
    CHECK(duplicates.triangles.duplicateCount == 1);
    data->originalPointIds.pop_back();
    CHECK_THROWS_AS((void)woby::inspectDuplicates(input), std::invalid_argument);
}

TEST_CASE("Importer point ID validation rejects malformed metadata and releases failures cancellation and callback errors")
{
    PointIdFixture f;
    for (const auto* name : {"short_ids", "wrong_count", "null_ids", "different_position", "failure"}) {
        CHECK_THROWS_AS((void)f.mesh(name), std::runtime_error);
        REQUIRE(f.mesh().sourceData->originalPointIds.size() == 8);
    }
    bool canceled = false;
    woby::ImportCallbacks callbacks;
    callbacks.progress = [&](float) { canceled = true; }; callbacks.canceled = [&] { return canceled; };
    CHECK(woby::importModel(f.file("canceled"), {}, callbacks).canceled);
    REQUIRE(f.mesh().sourceData->originalPointIds.size() == 8);
    callbacks = {};
    callbacks.progress = [](float) { throw std::runtime_error("Progress failed."); };
    CHECK_THROWS_AS((void)woby::importModel(f.file("progress_error"), {}, callbacks), std::runtime_error);
    CHECK(f.mesh().sourceData->originalPointIds.size() == 8);
}

TEST_CASE("Importer point IDs cover line vertices and validate the combined byte limit before dereferencing")
{
    const WobyImportVertex vertices[] = {{{0,0,0},{},{}},{{1,0,0},{},{}},{{0,0,0},{},{}}};
    const uint32_t indices[] = {0,1,1,2};
    const uint64_t ids[] = {7,8,7};
    WobyImportResult result{}; result.struct_size = sizeof(result);
    result.vertices = vertices; result.vertex_count = 3;
    WobyImportLines lines{sizeof(WobyImportLines),indices,4,nullptr,0};
    WobyImportPointIds pointIds{sizeof(WobyImportPointIds),ids,3};
    const auto mesh = woby::copyImportedMesh(result, nullptr, &lines, &pointIds);
    CHECK(mesh.lineIndices == std::vector<uint32_t>{0,1,1,2});
    CHECK(mesh.sourceData->originalPointIds == std::vector<uint64_t>{7,8,7});
    // Vertex bytes alone fit in 4 GiB; adding one uint64 per vertex exceeds it.
    result.vertex_count = static_cast<uint32_t>((4ull * 1024 * 1024 * 1024 - sizeof(indices)) / sizeof(WobyImportVertex));
    pointIds.vertex_count = result.vertex_count;
    CHECK_THROWS_WITH_AS((void)woby::copyImportedMesh(result, nullptr, &lines, &pointIds),
        doctest::Contains("size limits"), std::runtime_error);
}
