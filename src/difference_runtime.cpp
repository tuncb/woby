#include "difference_runtime.h"
#include "comparison_gpu.h"
#include "control_protocol.h"
#include "math_utils.h"
#include "scene_pick.h"
#include "ui_operations.h"
#include <nlohmann/json.hpp>

namespace woby
{
void destroyDifferenceRuntime(DifferenceRuntime &r)
{
    r.stop.request_stop();
    if (r.worker.valid()) {
        r.worker.wait();
    }
    for (auto &g : r.gpu) {
        destroySurface(g);
    }
    if (r.exportWorker.valid()) {
        r.exportWorker.wait();
    }
}
void updateDifferenceRuntime(DifferenceRuntime &r, const UiState &state, SceneObjectId id, uint64_t signature)
{
    const auto *item = findComparison(state, id);
    if (!item) {
        return;
    }
    if (r.exportWorker.valid() && r.exportWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        r.exportError = r.exportWorker.get();
        r.exportState = r.exportError.empty() ? "complete" : "failed";
    }
    const auto request = item->differenceRequest;
    if (!item->settings.difference.created) {
        r.stop.request_stop();
        r.phase = "not_created";
        r.initialized = false;
        r.result.reset();
        r.error.clear();
        r.uploaded = false;
        for (auto &gpu : r.gpu) {
            destroySurface(gpu);
        }
    }
    const bool changed = request.revision != r.consumedRequest;
    if (changed) {
        r.stop.request_stop();
        r.consumedRequest = request.revision;
        r.phase = !item->settings.difference.created ? "not_created" : request.cancel ? "canceled" : "queued";
        r.error.clear();
    }
    if (item->settings.difference.created && !r.initialized) {
        r.initialized = true;
        r.phase = request.cancel ? "canceled" : "queued";
    }
    if (r.worker.valid() && (r.workerSignature != signature || !item->settings.difference.created)) {
        r.stop.request_stop();
        if (r.phase == "running") {
            r.phase = "outdated";
        }
    }
    if (r.worker.valid() && r.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const bool current = !r.stop.stop_requested() && signature == r.workerSignature &&
                             r.workerRequest == request.revision && item->settings.difference.created;
        try {
            auto result = r.worker.get();
            if (current) {
                r.result = std::move(result);
                r.signature = signature;
                r.phase = "ready";
                r.error.clear();
                r.uploaded = false;
                for (auto &g : r.gpu) {
                    destroySurface(g);
                }
            }
        } catch (const std::exception &e) {
            if (current) {
                r.phase = "failed";
                r.error = e.what();
            }
        }
    }
    if (r.phase == "ready" && r.signature != signature) {
        r.phase = "outdated";
    }
    if (r.phase == "queued" && !r.worker.valid()) {
        try {
            auto snapshot = snapshotComparisonInputs(state, id);
            r.stop = std::stop_source{};
            auto stop = r.stop.get_token();
            r.workerSignature = signature;
            r.workerRequest = request.revision;
            r.phase = "running";
            r.worker = std::async(std::launch::async, [snapshot = std::move(snapshot), stop] {
                const auto a = comparisonWorldMesh(snapshot, ComparisonSide::a, stop),
                           b = comparisonWorldMesh(snapshot, ComparisonSide::b, stop);
                return std::make_shared<const DifferenceMesh>(buildDifferenceMesh(a, b, stop));
            });
        } catch (const std::exception &e) {
            r.phase = "failed";
            r.error = e.what();
        }
    }
    // No backend work in unit tests or before graphics initialization.
}
nlohmann::json differenceStatus(const DifferenceRuntime &r, const UiState &state, SceneObjectId id)
{
    using Json = nlohmann::json;
    const auto *item = findComparison(state, id);
    if (!item) {
        throw std::invalid_argument("Difference mesh requires an analysis ID.");
    }
    const bool current = r.phase == "ready" && r.signature == comparisonGeometrySignature(state, id);
    const auto &s = item->settings.difference;
    Json result = {{"state", r.phase == "ready" && !current ? "outdated" : r.phase},
                   {"error", r.error},
                   {"current", current},
                   {"settings",
                    {{"created", s.created},
                     {"visible", s.visible},
                     {"showAOnly", s.showA},
                     {"showBOnly", s.showB},
                     {"showWalls", s.showWalls},
                     {"opacity", s.opacity}}},
                   {"volumeA", nullptr},
                   {"volumeB", nullptr},
                   {"totalVolume", nullptr},
                   {"netVolume", nullptr},
                   {"exportState", r.exportState},
                   {"exportError", r.exportError}};
    if (current && r.result) {
        const auto &v = *r.result;
        result["kind"] = v.constructed ? "constructed_gap" : "solid_difference";
        result["message"] = v.message;
        result["totalVolume"] = v.volumes[0] + v.volumes[1];
        if (!v.constructed) {
            result["volumeA"] = v.volumes[0];
            result["volumeB"] = v.volumes[1];
            result["netVolume"] = v.volumes[1] - v.volumes[0];
        }
        result["triangles"] = Json::array();
        for (const auto &m : v.parts) {
            result["triangles"].push_back(meshGeometry(m).indices.size() / 3);
        }
        result["closureTriangles"] = v.closureTriangles;
        result["removedCollapsedTriangles"] = {{"a", v.removedCollapsedTriangles[0]},
                                                {"b", v.removedCollapsedTriangles[1]}};
    }
    return result;
}
void uploadDifferenceMesh(DifferenceRuntime &r)
{
    if (r.phase != "ready" || !r.result || r.uploaded) {
        return;
    }
    for (size_t i = 0; i < 3; ++i) {
        SurfaceComparison s;
        s.source = r.result->parts[i];
        PreparedComparisonSource p;
        uploadSurface(r.gpu[i], s, comparisonSource, &p);
    }
    r.uploaded = true;
}
void requestDifferenceExport(DifferenceRuntime &r, const std::filesystem::path &path)
{
    if (r.phase != "ready" || !r.result) {
        throw std::runtime_error("Rebuild the difference mesh before export.");
    }
    if (r.exportWorker.valid()) {
        throw std::runtime_error("Difference export is already running.");
    }
    r.exportState = "running";
    r.exportError.clear();
    r.exportWorker = std::async(std::launch::async, [result = r.result, path] {
        try {
            exportDifferenceMesh(*result, path);
            return std::string{};
        } catch (const std::exception &e) {
            return std::string(e.what());
        }
    });
}
nlohmann::json controlDifference(UiState &state, DifferenceRuntime &r, const ControlOperation &command)
{
    using A = ControlAction;
    const auto id = command.objectId;
    const auto *item = findComparison(state, id);
    if (!item || isUvAnalysis(item->settings.type)) {
        throw std::invalid_argument("Difference mesh requires a mesh comparison ID.");
    }
    auto settings = item->settings;
    auto &d = settings.difference;
    if (command.action == A::differenceCreate || command.action == A::differenceRebuild) {
        requestDifferenceMesh(state, id);
    } else if (command.action == A::differenceCancel) {
        requestDifferenceMesh(state, id, true);
    } else if (command.action == A::differenceDelete) {
        requestDifferenceMesh(state, id, true);
        d = {};
        setComparisonSettings(state, settings, id);
    } else if (command.action == A::differenceSet) {
        if (!d.created) {
            throw std::invalid_argument("Create the difference mesh first.");
        }
        if (command.visible) {
            d.visible = *command.visible;
        }
        if (command.showAOnly) {
            d.showA = *command.showAOnly;
        }
        if (command.showBOnly) {
            d.showB = *command.showBOnly;
        }
        if (command.showWalls) {
            d.showWalls = *command.showWalls;
        }
        if (command.opacity) {
            d.opacity = *command.opacity;
        }
        setComparisonSettings(state, settings, id);
    }
    updateDifferenceRuntime(r, state, id, comparisonGeometrySignature(state, id));
    if (command.action == A::differenceExport) {
        if (!differenceStatus(r, state, id).value("current", false)) {
            throw std::invalid_argument("Rebuild the difference mesh before export.");
        }
        requestDifferenceExport(r, command.path);
    }
    return {{"difference", differenceStatus(r, state, id)}};
}
} // namespace woby
