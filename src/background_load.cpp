#include "background_load.h"

#include "file_discovery.h"
#include "obj_mesh.h"
#include "performance_log.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <stdexcept>
#include <utility>

namespace woby {
namespace {

bool canceled(const BackgroundLoadCancelCallback& shouldCancel)
{
    return shouldCancel && shouldCancel();
}

void reportProgress(
    const BackgroundLoadProgressCallback& progress,
    const std::filesystem::path& path,
    size_t completedCount,
    size_t totalCount)
{
    if (!progress) {
        return;
    }

    BackgroundLoadProgress update;
    update.currentPath = path;
    update.completedCount = completedCount;
    update.totalCount = totalCount;
    progress(update);
}

void buildObjBatchStatus(ObjBatchCpuLoadResult& result)
{
    result.status = "Added " + std::to_string(result.addedCount) + " OBJ file";
    if (result.addedCount != 1u) {
        result.status += "s";
    }
    if (result.skippedCount > 0u) {
        result.status += ", skipped " + std::to_string(result.skippedCount) + " non-OBJ";
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

ObjBatchCpuLoadResult loadObjBatchCpu(
    const std::vector<std::filesystem::path>& modelPaths,
    size_t firstColorIndex,
    const BackgroundLoadProgressCallback& progress,
    const BackgroundLoadCancelCallback& shouldCancel)
{
    const auto start = PerformanceClock::now();
    ObjBatchCpuLoadResult result;
    result.requestedCount = modelPaths.size();
    result.files.reserve(modelPaths.size());

    size_t colorIndex = firstColorIndex;
    for (size_t pathIndex = 0; pathIndex < modelPaths.size(); ++pathIndex) {
        const auto& modelPath = modelPaths[pathIndex];
        if (canceled(shouldCancel)) {
            result.canceled = true;
            break;
        }

        reportProgress(progress, modelPath, pathIndex, modelPaths.size());
        if (!isObjPath(modelPath)) {
            ++result.skippedCount;
            continue;
        }

        const auto totalStart = PerformanceClock::now();
        try {
            const auto parseStart = PerformanceClock::now();
            ObjMesh mesh = loadObjMesh(modelPath);
            const double parseMilliseconds = millisecondsBetween(parseStart, PerformanceClock::now());

            UiFileState file = createUiFileState(modelPath, std::move(mesh), colorIndex);
            colorIndex += file.groupSettings.size();
            spdlog::info(
                "perf obj_cpu_load path=\"{}\" vertices={} triangles={} groups={} parse_ms={} total_ms={}",
                modelPath.string(),
                file.mesh.vertices.size(),
                file.mesh.indices.size() / 3u,
                file.groupSettings.size(),
                parseMilliseconds,
                millisecondsBetween(totalStart, PerformanceClock::now()));
            result.files.push_back(std::move(file));
            ++result.addedCount;
        } catch (const std::exception& exception) {
            ++result.failedCount;
            result.lastError = exception.what();
            spdlog::info(
                "perf obj_cpu_load_failed path=\"{}\" duration_ms={} error=\"{}\"",
                modelPath.string(),
                millisecondsBetween(totalStart, PerformanceClock::now()),
                exception.what());
        }
    }

    buildObjBatchStatus(result);
    spdlog::info(
        "perf obj_cpu_load_batch requested_count={} loaded_count={} skipped_count={} failed_count={} canceled={} duration_ms={}",
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
    for (const auto& record : result.document.files) {
        if (canceled(shouldCancel)) {
            result.canceled = true;
            break;
        }

        const std::filesystem::path modelPath = sceneAbsolutePath(scenePath, record.path);
        reportProgress(progress, modelPath, result.files.size(), result.document.files.size());
        const auto loadStart = PerformanceClock::now();
        ObjMesh mesh = loadObjMesh(modelPath);
        UiFileState file = createUiFileState(modelPath, std::move(mesh), colorIndex);
        applySceneFileRecord(file, record);
        colorIndex += file.groupSettings.size();
        spdlog::info(
            "perf scene_obj_cpu_load scene=\"{}\" path=\"{}\" vertices={} triangles={} groups={} duration_ms={}",
            scenePath.string(),
            modelPath.string(),
            file.mesh.vertices.size(),
            file.mesh.indices.size() / 3u,
            file.groupSettings.size(),
            millisecondsBetween(loadStart, PerformanceClock::now()));
        result.files.push_back(std::move(file));
    }

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
