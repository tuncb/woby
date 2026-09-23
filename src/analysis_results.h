#pragma once

#include "mesh_comparison.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <filesystem>
#include <future>
#include <ostream>

namespace woby {

// Collection paths use zero-based array indexes, e.g. /findings/0/members.
// Only the requested page is materialized; nested arrays retain summary limits.
nlohmann::json analysisResultPage(const MeshComparison& result, ComparisonSide side,
    const std::string& detector, const std::string& collection, size_t offset, size_t limit);
nlohmann::json analysisDetectorSummary(const MeshComparison& result, ComparisonSide side,
    const std::string& detector);
// Streams all retained diagnostic arrays, including nested members, without a JSON DOM.
void writeAnalysisResults(std::ostream& stream, const MeshComparison& result, nlohmann::json summary,
    std::stop_token stop = {}, std::atomic<size_t>* written = nullptr);

struct AnalysisExportRuntime {
    std::stop_source stop;
    std::atomic<size_t> written{0};
    std::string target;
    std::filesystem::path path;
    nlohmann::json outcome;
    // Destroy/join the future before releasing any state captured by its worker.
    std::future<nlohmann::json> worker;
};
void startAnalysisExport(AnalysisExportRuntime& runtime, const MeshComparison& result,
    nlohmann::json summary, const std::string& target, const std::filesystem::path& path);
nlohmann::json analysisExportStatus(AnalysisExportRuntime& runtime);
void cancelAnalysisExport(AnalysisExportRuntime& runtime);
void stopAnalysisExport(AnalysisExportRuntime& runtime);

} // namespace woby
