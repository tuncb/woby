#include "analysis_results.h"
#include "control_scene.h"
#include <doctest/doctest.h>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>

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
    t.availableSources = 1; t.sources.resize(1);
    t.sources[0].fileId = 1; t.sources[0].vertices.resize(121);
    for (auto& v : t.sources[0].vertices) {
        for (size_t i = 0; i < 130; ++i) { v.references.push_back({2, i}); }
    }
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
    std::ifstream input(path); const auto saved = Json::parse(input); input.close();
    CHECK(saved["aToB"]["detectors"]["duplicate_points"]["findings"].size() == 123);
    CHECK_THROWS(startAnalysisExport(f.runtime, r, controlComparisonResults(r, .05, false), "analysis", path));
    CHECK(std::distance(std::filesystem::directory_iterator(f.root), std::filesystem::directory_iterator{}) == 1);
    CHECK_THROWS(startAnalysisExport(f.runtime, r, {}, "analysis", f.root / "missing" / "result.json"));
    CHECK_THROWS(startAnalysisExport(f.runtime, r, {}, "analysis", "relative.json"));
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
