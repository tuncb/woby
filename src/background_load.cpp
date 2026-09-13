#include "background_load.h"

#include "file_discovery.h"
#include "model_load.h"
#include "performance_log.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <exception>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace woby {
namespace {

struct TimedModelLoad {
    ImportedModel imported;
    double parseMilliseconds = 0;
};

TimedModelLoad timedModelLoad(const std::filesystem::path& path, const std::string& importerId,
    const ImportCallbacks& callbacks)
{
    const auto start = PerformanceClock::now();
    auto imported = loadModel(path, importerId, callbacks);
    return {std::move(imported), millisecondsBetween(start, PerformanceClock::now())};
}

struct PrefetchedLoads {
    std::array<std::future<TimedModelLoad>, 4> pending;
};

template <typename PathAt>
void prefetchSmallModels(PrefetchedLoads& loads, size_t current, size_t total, const PathAt& pathAt)
{
    // Bounded look-ahead overlaps small-file I/O. Large OBJ files already use
    // RapidOBJ's own parallel parser; plugins keep their serial callback model.
    // At most four <= 1 MiB inputs are being parsed speculatively. Consumption,
    // progress/cancel callbacks, color assignment, and outcomes remain ordered.
    if (total < 8) { return; }
    static const auto hardware = std::thread::hardware_concurrency();
    const size_t concurrency = hardware > 2 ? std::min(size_t{4}, size_t(hardware - 2)) : 1;
    if (concurrency < 2) { return; }
    for (size_t i = current; i < total && i - current < concurrency; ++i) {
        auto& pending = loads.pending[i % loads.pending.size()];
        if (pending.valid()) { continue; }
        const auto path = pathAt(i);
        if (!path || (!isObjPath(*path) && !isStlPath(*path))) { break; }
        std::error_code error;
        const auto bytes = std::filesystem::file_size(*path, error);
        if (error || bytes > 1024 * 1024) { break; }
        try {
            pending = std::async(std::launch::async, [path = *path] { return timedModelLoad(path, {}, {}); });
        } catch (const std::system_error&) {
            break; // Resource exhaustion: leave this and later inputs serial.
        }
    }
}

bool canceled(const BackgroundLoadCancelCallback& shouldCancel)
{
    return shouldCancel && shouldCancel();
}

void reportProgress(
    const BackgroundLoadProgressCallback& progress,
    const std::filesystem::path& path,
    size_t completedCount,
    size_t totalCount,
    float fraction = 0.0f)
{
    if (!progress) {
        return;
    }

    BackgroundLoadProgress update;
    update.currentPath = path;
    update.completedCount = completedCount;
    update.totalCount = totalCount;
    update.currentFileFraction = fraction;
    progress(update);
}

void buildModelBatchStatus(ModelBatchCpuLoadResult& result)
{
    result.status = "Added " + std::to_string(result.addedCount) + " model file";
    if (result.addedCount != 1u) {
        result.status += "s";
    }
    if (result.skippedCount > 0u) {
        result.status += ", skipped " + std::to_string(result.skippedCount) + " non-model";
    }
    if (result.failedCount > 0u) {
        result.status += ", failed " + std::to_string(result.failedCount);
        if (!result.lastError.empty()) {
            result.status += ": " + result.lastError;
        }
    }
    if (result.canceled) {
        result.status += ", canceled";
    }
}

} // namespace

ModelBatchCpuLoadResult loadModelBatchCpu(
    const std::vector<std::filesystem::path>& modelPaths,
    size_t firstColorIndex,
    const BackgroundLoadProgressCallback& progress,
    const BackgroundLoadCancelCallback& shouldCancel)
{
    const auto start = PerformanceClock::now();
    ModelBatchCpuLoadResult result;
    result.requestedCount = modelPaths.size();
    result.files.reserve(modelPaths.size());
    result.outcomes.reserve(modelPaths.size());
    for (const auto& path : modelPaths) { result.outcomes.push_back({path, "not-started", {}}); }

    size_t colorIndex = firstColorIndex;
    PrefetchedLoads prefetched;
    for (size_t pathIndex = 0; pathIndex < modelPaths.size(); ++pathIndex) {
        const auto& modelPath = modelPaths[pathIndex];
        auto& outcome = result.outcomes[pathIndex];
        if (canceled(shouldCancel)) {
            result.canceled = true;
            break;
        }

        reportProgress(progress, modelPath, pathIndex, modelPaths.size());
        if (!isModelPath(modelPath)) {
            ++result.skippedCount;
            outcome.state = "skipped";
            continue;
        }

        const auto totalStart = PerformanceClock::now();
        try {
            prefetchSmallModels(prefetched, pathIndex, modelPaths.size(), [&](size_t index) {
                return std::optional<std::filesystem::path>{modelPaths[index]};
            });
            auto& pending = prefetched.pending[pathIndex % prefetched.pending.size()];
            auto parsed = pending.valid() ? pending.get() : timedModelLoad(modelPath, {}, {shouldCancel, [&](float fraction) {
                reportProgress(progress, modelPath, pathIndex, modelPaths.size(), fraction);
            }});
            auto& imported = parsed.imported;
            if (imported.canceled) {
                result.canceled = true;
                outcome.state = "canceled";
                break;
            }

            UiFileState file = createUiFileState(
                modelPath, std::move(imported.mesh), colorIndex, std::move(imported.importerId));
            colorIndex += file.groupSettings.size();
            spdlog::info(
                "perf model_cpu_load path=\"{}\" vertices={} triangles={} groups={} parse_ms={} total_ms={}",
                modelPath.string(),
                file.mesh.vertices.size(),
                file.mesh.indices.size() / 3u,
                file.groupSettings.size(),
                parsed.parseMilliseconds,
                millisecondsBetween(totalStart, PerformanceClock::now()));
            result.files.push_back(std::move(file));
            outcome.state = "loaded";
            ++result.addedCount;
        } catch (const std::exception& exception) {
            outcome.state = "failed";
            outcome.error = exception.what();
            ++result.failedCount;
            result.lastError = exception.what();
            spdlog::info(
                "perf model_cpu_load_failed path=\"{}\" duration_ms={} error=\"{}\"",
                modelPath.string(),
                millisecondsBetween(totalStart, PerformanceClock::now()),
                exception.what());
        }
    }

    buildModelBatchStatus(result);
    spdlog::info(
        "perf model_cpu_load_batch requested_count={} loaded_count={} skipped_count={} failed_count={} canceled={} duration_ms={}",
        result.requestedCount,
        result.files.size(),
        result.skippedCount,
        result.failedCount,
        result.canceled,
        millisecondsBetween(start, PerformanceClock::now()));
    return result;
}

SceneCpuLoadResult loadSceneCpu(
    const std::filesystem::path& scenePath,
    const BackgroundLoadProgressCallback& progress,
    const BackgroundLoadCancelCallback& shouldCancel)
{
    const auto totalStart = PerformanceClock::now();
    SceneCpuLoadResult result;
    result.scenePath = scenePath;

    const auto readStart = PerformanceClock::now();
    result.document = readSceneDocument(scenePath);
    const double readMilliseconds = millisecondsBetween(readStart, PerformanceClock::now());

    result.files.reserve(result.document.files.size());
    size_t colorIndex = 0;
    PrefetchedLoads prefetched;
    for (const auto& record : result.document.files) {
        if (canceled(shouldCancel)) {
            result.canceled = true;
            break;
        }

        const std::filesystem::path modelPath = sceneAbsolutePath(scenePath, record.path);
        reportProgress(progress, modelPath, result.files.size(), result.document.files.size());
        const auto loadStart = PerformanceClock::now();
        prefetchSmallModels(prefetched, result.files.size(), result.document.files.size(), [&](size_t index) {
            const auto& candidate = result.document.files[index];
            if (!candidate.importerId.empty()) { return std::optional<std::filesystem::path>{}; }
            return std::optional<std::filesystem::path>{sceneAbsolutePath(scenePath, candidate.path)};
        });
        auto& pending = prefetched.pending[result.files.size() % prefetched.pending.size()];
        auto parsed = pending.valid() ? pending.get() : timedModelLoad(modelPath, record.importerId, {shouldCancel, [&](float fraction) {
            reportProgress(progress, modelPath, result.files.size(), result.document.files.size(), fraction);
        }});
        auto& imported = parsed.imported;
        if (imported.canceled) {
            result.canceled = true;
            break;
        }
        UiFileState file = createUiFileState(modelPath, std::move(imported.mesh), colorIndex, std::move(imported.importerId));
        applySceneFileRecord(file, record);
        colorIndex += file.groupSettings.size();
        spdlog::info(
            "perf scene_model_cpu_load scene=\"{}\" path=\"{}\" vertices={} triangles={} groups={} duration_ms={}",
            scenePath.string(),
            modelPath.string(),
            file.mesh.vertices.size(),
            file.mesh.indices.size() / 3u,
            file.groupSettings.size(),
            millisecondsBetween(loadStart, PerformanceClock::now()));
        result.files.push_back(std::move(file));
    }

    result.canceled = result.canceled || canceled(shouldCancel);
    spdlog::info(
        "perf scene_cpu_load path=\"{}\" files={} read_ms={} canceled={} total_ms={}",
        scenePath.string(),
        result.files.size(),
        readMilliseconds,
        result.canceled,
        millisecondsBetween(totalStart, PerformanceClock::now()));
    return result;
}

} // namespace woby
