#include "analysis_results.h"
#include <chrono>
#include <fstream>
#include <iostream>

// Fixed retained findings isolate export from loading/detection. A boundary edge
// exercises nested face/point references and coordinates; duplicates exercise
// large member lists. No wall-clock assertions belong in the unit suite.
int analysisExportBenchmark(size_t count, size_t repetitions, const std::filesystem::path& directory)
{
    using namespace woby;
    using Clock = std::chrono::steady_clock;
    if (!directory.is_absolute() || !std::filesystem::is_directory(directory)) {
        throw std::invalid_argument("Export benchmark needs an absolute existing output directory.");
    }
    MeshComparison result;
    for (auto& status : result.detectors) { status.phase = IntersectionPhase::complete; }
    auto& topology = result.original.topology;
    auto sources = std::make_shared<std::vector<SourceTopology>>(1);
    auto& source = sources->front(); source.fileId = 42; source.source = "quoted\"model.obj";
    auto incidence = std::make_shared<TopologyIncidence>();
    incidence->pointReferences = {{7, 0}, {7, 1}};
    incidence->edgeUses = {{0, true}};
    source.vertices.resize(2);
    for (size_t i = 0; i < 2; ++i) {
        source.vertices[i].position = {double(i), 1.25, -2.5};
        source.vertices[i].references = std::span<const TopologyPointReference>(incidence->pointReferences).subspan(i, 1);
    }
    source.faces.push_back({{42, 7, 0}});
    source.edges.push_back({{0, 1}, incidence->edgeUses});
    source.incidence = std::move(incidence);
    topology.sources = *sources; topology.sourceStorage = std::move(sources);
    topology.availableSources = 1;
    topology.boundaries.resize(count, {0, 0});
    auto& duplicates = result.original.duplicates.points;
    duplicates.availableSources = 1;
    duplicates.findings.resize(count / 10 + 1);
    for (auto& finding : duplicates.findings) {
        finding.fileId = 42; finding.source = source.source;
        for (size_t i = 0; i < 20; ++i) { finding.members.push_back({i, i % 2 == 0}); }
        duplicates.duplicateCount += 19;
    }
    const nlohmann::json summary = {{"aToB", nlohmann::json::object()}, {"bToA", nullptr}};
    for (size_t run = 0; run < repetitions; ++run) {
        const auto path = directory / ("export-" + std::to_string(run) + ".json");
        if (std::filesystem::exists(path)) { throw std::invalid_argument("Use a fresh benchmark directory."); }
#ifdef _WIN32
        std::ofstream sink("NUL", std::ios::binary);
#else
        std::ofstream sink("/dev/null", std::ios::binary);
#endif
        sink.exceptions(std::ios::badbit | std::ios::failbit);
        const auto start = Clock::now();
        std::atomic<size_t> entries{0};
        writeAnalysisResults(sink, result, summary, {}, &entries); sink.close();
        const double nullMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        AnalysisExportRuntime runtime;
        const auto jobStart = Clock::now();
        startAnalysisExport(runtime, result, summary, "benchmark", path);
        const double startMs = std::chrono::duration<double, std::milli>(Clock::now() - jobStart).count();
        runtime.worker.wait();
        const double totalMs = std::chrono::duration<double, std::milli>(Clock::now() - jobStart).count();
        auto status = analysisExportStatus(runtime);
        if (status["state"] != "complete") { throw std::runtime_error(status.dump()); }
        if (status["entriesWritten"] != entries.load()) { throw std::runtime_error("Entry counts differ between sinks."); }
        status.update({{"run", run}, {"boundaryFindings", count}, {"nullSinkMs", nullMs},
            {"startMs", startMs}, {"totalMs", totalMs}, {"bytes", std::filesystem::file_size(path)}});
        std::cout << status.dump() << std::endl;
    }
    return 0;
}
