#include "analysis_results.h"
#include "control_scene.h"
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

namespace {
using namespace woby;
using Json = nlohmann::json;
MeshComparison data()
{
    MeshComparison r;
    r.original.source.indices = {0, 1, 2};
    for (auto& status : r.detectors) { status.phase = IntersectionPhase::complete; }
    auto& duplicates = r.original.duplicates.points;
    duplicates.availableSources = 1;
    for (size_t i = 0; i < 123; ++i) {
        DuplicateFinding f; f.fileId = i + 1; f.source = "quoted\"source.obj";
        for (size_t j = 0; j < 137; ++j) { f.members.push_back({i * 1000 + j, false}); }
        duplicates.findings.push_back(std::move(f)); duplicates.duplicateCount += 136;
    }
    auto& t = r.original.topology;
    t.availableSources = 1;
    auto sources = std::make_shared<std::vector<SourceTopology>>(1);
    (*sources)[0].fileId = 1; (*sources)[0].vertices.resize(121);
    auto incidence = std::make_shared<TopologyIncidence>();
    incidence->pointReferences.reserve(121 * 130);
    for (auto& v : (*sources)[0].vertices) {
        const auto first = incidence->pointReferences.size();
        for (size_t i = 0; i < 130; ++i) { incidence->pointReferences.push_back({2, i}); }
        v.references = std::span<const TopologyPointReference>(incidence->pointReferences).subspan(first, 130);
    }
    (*sources)[0].incidence = std::move(incidence);
    t.sources = *sources;
    t.sourceStorage = std::move(sources);
    TopologyBoundary b;
    for (size_t i = 0; i < 121; ++i) { b.vertices.push_back(i); }
    t.boundaryRegions.push_back(std::move(b)); t.holes.push_back(0);
    return r;
}
struct ExportFixture {
    std::filesystem::path root;
    AnalysisExportRuntime runtime;
    ExportFixture() {
        static std::atomic<unsigned> sequence{0};
        root = std::filesystem::temp_directory_path() / ("woby-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        std::filesystem::create_directories(root);
    }
    ~ExportFixture() {
        stopAnalysisExport(runtime);
        std::error_code ignored; std::filesystem::remove_all(root, ignored);
    }
};
}

TEST_CASE("analysis pages reach every finding and nested member beyond summary limits")
{
    const auto r = data();
    const auto summary = controlComparisonResults(r, .05);
    CHECK(summary["aToB"]["detectors"]["duplicate_points"]["findings"].size() == 100);
    auto page = analysisResultPage(r, ComparisonSide::a, "duplicate_points", "/findings", 100, 20);
    CHECK(page["total"] == 123); CHECK(page["nextOffset"] == 120);
    CHECK(page["items"][0]["sourceId"] == "101");
    CHECK(page["items"][0]["membersTruncated"] == true);
    page = analysisResultPage(r, ComparisonSide::a, "duplicate_points", "/findings/122/members", 100, 100);
    CHECK(page["total"] == 137); CHECK(page["items"].size() == 37); CHECK(page["nextOffset"].is_null());
    CHECK(page["items"][0]["id"] == 122101);
    page = analysisResultPage(r, ComparisonSide::a, "holes", "/findings/0/vertices/120/pointReferences", 100, 100);
    CHECK(page["total"] == 130); CHECK(page["items"].size() == 30); CHECK(page["items"][0]["pointId"] == 101);
    page = analysisResultPage(r, ComparisonSide::a, "holes", "/boundaryRegions", 0, 1);
    CHECK(page["items"][0]["vertices"][0]["pointReferences"].size() == 1);
    CHECK(analysisResultPage(r, ComparisonSide::a, "holes", "/findings", std::numeric_limits<size_t>::max(), 100)["items"].empty());
    for (const auto* path : {"findings", "/absent", "/findings/123/members", "/findings/-1/members", "/findings/0", "/findings//members"}) {
        CHECK_THROWS(analysisResultPage(r, ComparisonSide::a, "duplicate_points", path, 0, 100));
    }
    CHECK_THROWS(analysisResultPage(r, ComparisonSide::a, "holes", "/findings", 0, 0));
    CHECK_THROWS(analysisResultPage(r, ComparisonSide::a, "holes", "/findings", 0, 101));
}

TEST_CASE("full analysis JSON export preserves all nested entries and partial detection status")
{
    auto r = data();
    r.original.intersections.phase = IntersectionPhase::complete;
    r.original.intersections.truncated = true; r.original.intersections.truncationReason = "pair_limit";
    r.original.intersections.findings.resize(101);
    r.original.intersections.limits = {101, 0};
    std::ostringstream stream;
    writeAnalysisResults(stream, r, controlComparisonResults(r, .05, false));
    const auto output = Json::parse(stream.str());
    const auto& detectors = output["aToB"]["detectors"];
    CHECK(output["bToA"].is_null());
    CHECK(detectors["duplicate_points"]["findings"].size() == 123);
    CHECK(detectors["duplicate_points"]["findings"][122]["members"].size() == 137);
    CHECK(detectors["duplicate_points"]["findings"][122]["membersTruncated"] == false);
    CHECK(detectors["duplicate_points"]["findingsTruncated"] == false);
    CHECK(detectors["holes"]["findings"][0]["vertices"].size() == 121);
    CHECK(detectors["holes"]["findings"][0]["vertices"][120]["pointReferences"].size() == 130);
    CHECK(detectors["holes"]["findings"][0]["vertices"][120]["pointReferencesTruncated"] == false);
    CHECK(detectors["self_intersections"]["findings"].size() == 101);
    CHECK(detectors["self_intersections"]["count"].is_null());
    CHECK(detectors["self_intersections"]["detectionTruncated"] == true);
    CHECK(detectors["self_intersections"]["truncationReason"] == "pair_limit");
    CHECK(detectors["self_intersections"]["pairLimit"] == 101);
    CHECK(detectors["self_intersections"]["candidateLimit"] == 0);
    std::stop_source stop; stop.request_stop();
    CHECK_THROWS(writeAnalysisResults(stream, r, controlComparisonResults(r, .05, false), stop.get_token()));
    std::ostringstream broken; broken.setstate(std::ios::badbit);
    CHECK_THROWS(writeAnalysisResults(broken, r, controlComparisonResults(r, .05, false)));
}

TEST_CASE("analysis export jobs publish a consistent snapshot and never overwrite files")
{
    ExportFixture f;
    auto r = data();
    const auto path = f.root / "full results.json";
    startAnalysisExport(f.runtime, r, controlComparisonResults(r, .05, false), "analysis", path);
    r.original.duplicates.points.findings.clear();
    f.runtime.worker.wait();
    auto status = analysisExportStatus(f.runtime);
    CHECK(status["state"] == "complete"); CHECK(status["entriesWritten"].get<size_t>() > 123);
    CHECK(status["bytesWritten"] == std::filesystem::file_size(path));
    for (const auto* stage : {"snapshotMs", "serializationMs", "streamWriteMs", "writeMs", "publishMs", "totalMs"}) {
        CHECK(status["timings"][stage].get<double>() >= 0);
    }
    CHECK(status["timings"]["writeMs"].get<double>() <= status["timings"]["totalMs"].get<double>());
    std::ifstream input(path); const auto saved = Json::parse(input); input.close();
    CHECK(saved["aToB"]["detectors"]["duplicate_points"]["findings"].size() == 123);
    CHECK_THROWS(startAnalysisExport(f.runtime, r, controlComparisonResults(r, .05, false), "analysis", path));
    CHECK(std::distance(std::filesystem::directory_iterator(f.root), std::filesystem::directory_iterator{}) == 1);
    CHECK_THROWS(startAnalysisExport(f.runtime, r, {}, "analysis", f.root / "missing" / "result.json"));
    CHECK_THROWS(startAnalysisExport(f.runtime, r, {}, "analysis", "relative.json"));
}

TEST_CASE("direct export matches the established schema for every detector on both sides")
{
    MeshComparison r;
    for (auto& status : r.detectors) { status.phase = IntersectionPhase::complete; }
    auto mesh = std::make_shared<SourceMeshData>();
    mesh->points = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    mesh->indices = {0, 1, 2};
    SourcePartInstance part;
    part.partId = std::numeric_limits<uint64_t>::max(); part.indexCount = 3;
    part.transform = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::string escaped = "quote\" slash\\ controls\b\f\n\r\t";
    escaped.append("\0\x01", 2); escaped += "\xc3\xa9\xf0\x9f\x98\x80";
    DuplicateSource source{std::numeric_limits<uint64_t>::max(), escaped, mesh, true, {part}};
    auto& t = r.original.topology;
    t = buildMeshTopology({source});
    REQUIRE_FALSE(t.boundaries.empty());
    REQUIRE_FALSE(t.boundaryRegions.empty());
    t.coordinateOrigin = {1e100, -1e-100, -0.0};
    // Seed each retained record shape; detection itself is covered separately.
    t.nonManifoldEdges = {t.boundaries[0]}; t.windingEdges = {t.boundaries[0]};
    t.windingFaces = {t.sources[0].faces[0].reference};
    t.nonManifoldVertices = {{0, 0, 2}};
    t.holes = {0};
    t.boundaryRegions[0].ratioAvailable = true; t.boundaryRegions[0].sizeRatio = .125;
    TopologyFinPatch patch;
    patch.faces = {0}; patch.physicalBoundaryEdges = {0, 1}; patch.cutBoundaryEdges = {2};
    patch.area = .5; patch.denominatorArea = 1; patch.areaRatio = .5;
    patch.boundary = FinBoundaryKind::simpleLoop; patch.boundaryComponents = 1;
    t.finPatches = {patch}; t.fins = {0};
    DuplicateFinding duplicate;
    duplicate.fileId = source.fileId; duplicate.source = escaped;
    duplicate.members = {{std::numeric_limits<size_t>::max() - 1, true}, {1, false}};
    r.original.duplicates.points.findings = {duplicate};
    r.original.duplicates.triangles.findings = {duplicate};
    DegenerateFinding degenerate;
    degenerate.fileId = source.fileId; degenerate.partId = part.partId;
    degenerate.source = escaped; degenerate.triangleId = std::numeric_limits<size_t>::max() - 1;
    degenerate.reasons = {true, false, true, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
    r.original.degenerates.findings = {degenerate};
    degenerate.reasons = {false, true, false, -0.0, std::numeric_limits<double>::max()};
    r.original.degenerates.findings.push_back(degenerate);
    auto& intersections = r.original.intersections;
    intersections.phase = IntersectionPhase::complete;
    IntersectionFinding intersection;
    intersection.source = escaped; intersection.faces = {t.windingFaces[0], t.windingFaces[0]};
    intersections.findings = {intersection};
    r.repaired = r.original;
    const Json summary = {{"aToB", {{"custom\"key", escaped}}}, {"bToA", Json::object()}, {"other\"key", escaped}};
    for (const bool unavailable : {false, true}) {
        r.original.topology.unavailableSources = unavailable ? 1 : 0;
        r.original.intersections.truncated = unavailable;
        r.original.intersections.truncationReason = unavailable ? "pair_limit" : "";
        std::ostringstream stream;
        std::atomic<size_t> entries{0}; AnalysisExportWriteMetrics metrics;
        writeAnalysisResults(stream, r, summary, {}, &entries, &metrics);
        const auto text = stream.str(); const auto output = Json::parse(text);
        CHECK(metrics.bytesWritten == text.size());
        CHECK(entries.load() > 0);
        CHECK(output["other\"key"] == escaped);
        CHECK(output["aToB"]["custom\"key"] == escaped);
        CHECK(output["allRetainedResults"] == true);
        CHECK(output["detectionComplete"] == !unavailable);
        for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
            const auto& detectors = output[side == ComparisonSide::a ? "aToB" : "bToA"]["detectors"];
            for (const auto* detector : diagnosticCategoryKeys) {
                CAPTURE(detector); CAPTURE(unavailable);
                CHECK(detectors[detector] == analysisDetectorSummary(r, side, detector));
            }
        }
    }
}

TEST_CASE("direct export preserves disabled and unfinished detector metadata")
{
    auto r = data();
    const Json summary = {{"aToB", Json::object()}, {"bToA", nullptr}};
    for (const auto phase : {IntersectionPhase::notChecked, IntersectionPhase::queued, IntersectionPhase::running,
            IntersectionPhase::outdated, IntersectionPhase::canceled, IntersectionPhase::failed}) {
        for (auto& status : r.detectors) { status.phase = phase; status.hasResult = true; status.knownCounts = {3, 4}; status.error = "test"; }
        r.original.intersections.phase = phase; r.original.intersections.hasResult = true;
        std::ostringstream stream; writeAnalysisResults(stream, r, summary);
        const auto output = Json::parse(stream.str());
        CHECK(output["detectionComplete"] == false);
        for (const auto* detector : diagnosticCategoryKeys) {
            CHECK(output["aToB"]["detectors"][detector] == analysisDetectorSummary(r, ComparisonSide::a, detector));
        }
    }
    for (auto& status : r.detectors) { status.phase = IntersectionPhase::complete; }
    r.original.duplicates.points.enabled = false; r.original.duplicates.triangles.enabled = false;
    r.original.degenerates.settings.enabled = false;
    r.original.topology.inspection = {false, false, false, false, .05f, false, false, 1};
    std::ostringstream stream; writeAnalysisResults(stream, r, summary);
    const auto output = Json::parse(stream.str());
    for (const auto* detector : diagnosticCategoryKeys) {
        CHECK(output["aToB"]["detectors"][detector] == analysisDetectorSummary(r, ComparisonSide::a, detector));
    }
}

TEST_CASE("export cancellation after progress removes partially written output")
{
    ExportFixture f;
    auto r = data();
    r.original.duplicates.points.findings[0].members.resize(1000000);
    const auto path = f.root / "partial.json";
    startAnalysisExport(f.runtime, r, {{"aToB", Json::object()}}, "analysis", path);
    while (f.runtime.written.load(std::memory_order_relaxed) < 4096
        && f.runtime.worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { std::this_thread::yield(); }
    cancelAnalysisExport(f.runtime); f.runtime.worker.wait();
    const auto status = analysisExportStatus(f.runtime);
    CHECK(status["entriesWritten"].get<size_t>() > 0);
    CHECK(status["bytesWritten"].get<size_t>() >= 65536);
    CHECK(status["state"] == "canceled");
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(std::filesystem::is_empty(f.root));
}

TEST_CASE("export serialization failure cleans staging and permits a retry")
{
    ExportFixture f;
    auto r = data();
    r.original.duplicates.points.findings.back().source = "\xff";
    const auto path = f.root / "invalid-utf8.json";
    startAnalysisExport(f.runtime, r, {{"aToB", Json::object()}}, "analysis", path);
    f.runtime.worker.wait();
    const auto status = analysisExportStatus(f.runtime);
    CHECK(status["state"] == "failed");
    CHECK(status["bytesWritten"].get<size_t>() > 65536);
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(std::filesystem::is_empty(f.root));
    r.original.duplicates.points.findings.back().source = "valid.obj";
    startAnalysisExport(f.runtime, r, {{"aToB", Json::object()}}, "analysis", path);
    f.runtime.worker.wait();
    CHECK(analysisExportStatus(f.runtime)["state"] == "complete");
}

TEST_CASE("canceled analysis export cleans its temporary output")
{
    ExportFixture f;
    const auto r = data();
    const auto path = f.root / "canceled.json";
    startAnalysisExport(f.runtime, r, controlComparisonResults(r, .05, false), "analysis", path);
    cancelAnalysisExport(f.runtime); f.runtime.worker.wait();
    CHECK(analysisExportStatus(f.runtime)["state"] == "canceled");
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(std::filesystem::is_empty(f.root));
}

TEST_CASE("concurrent analysis exports cannot replace another completed export")
{
    ExportFixture f;
    const auto r = data();
    const auto path = f.root / "race.json";
    const auto writer = [&] {
        AnalysisExportRuntime runtime;
        try {
            startAnalysisExport(runtime, r, controlComparisonResults(r, .05, false), "analysis", path);
            runtime.worker.wait();
            return analysisExportStatus(runtime)["state"] == "complete" ? 1 : 0;
        } catch (const std::invalid_argument&) { return 0; }
    };
    auto first = std::async(std::launch::async, writer);
    auto second = std::async(std::launch::async, writer);
    CHECK(first.get() + second.get() == 1);
    std::ifstream input(path); const auto saved = Json::parse(input); input.close();
    CHECK(saved["aToB"]["detectors"]["duplicate_points"]["findings"].size() == 123);
    CHECK(std::distance(std::filesystem::directory_iterator(f.root), std::filesystem::directory_iterator{}) == 1);
}
