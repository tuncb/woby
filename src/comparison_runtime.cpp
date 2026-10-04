#include "comparison_runtime.h"
#include "comparison_gpu.h"
#include "ui_operations.h"
#include <algorithm>
#include <stdexcept>

namespace woby {
namespace {
uint64_t nextResultsRevision()
{
    // Called only by the main-thread runtime adapter, unique even after undo/recreation.
    static uint64_t revision = 0;
    return ++revision;
}
void recordResultPublication(ComparisonResultRuntime& results, uint32_t stages)
{
    results.revision = nextResultsRevision();
    for (size_t i = 0; i < results.stageRevisions.size(); ++i) {
        if (stages & (1u << i)) { results.stageRevisions[i] = results.revision; }
    }
}
}

uint64_t comparisonCurrentSignature(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id)
{
    const auto& queries = runtime.inspector.queries;
    return comparisonInspectorCacheCurrent(queries, state, id) ? queries.signature : comparisonGeometrySignature(state, id);
}

bool comparisonDetectorReady(const ComparisonRuntime& runtime, const ComparisonSettings& settings,
    uint64_t signature, DiagnosticCategory category, bool requireGpu)
{
    return comparisonDetectorStatus(runtime.results.value, category).phase == IntersectionPhase::complete
        && comparisonStagesReady(runtime, settings, signature, comparisonDiagnosticStage(category), requireGpu);
}

bool comparisonStagesReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    uint32_t stages, bool requireGpu)
{
    return comparisonStagesReady(runtime, comparisonSettings(state, id), comparisonCurrentSignature(runtime, state, id), stages, requireGpu);
}

bool comparisonStagesReady(const ComparisonRuntime& runtime, const ComparisonSettings& settings,
    uint64_t signature, uint32_t stages, bool requireGpu)
{
    if (!signature || runtime.results.signature != signature || runtime.results.cache.signature != signature
        || (runtime.results.cache.completed & stages) != stages
        || (requireGpu && (runtime.gpu.uploadedStages & stages) != stages)) { return false; }
    if (requireGpu) {
        for (size_t i = 0; i < runtime.results.stageRevisions.size(); ++i) {
            if ((stages & (1u << i)) && runtime.results.stageRevisions[i] != runtime.gpu.stageRevisions[i]) { return false; }
        }
    }
    if ((stages & (comparisonTopology | comparisonIntersections)) && runtime.results.cache.topologyMode != settings.topologyMode) { return false; }
    if ((stages & comparisonDegenerates) && !sameDegenerateThresholds(runtime.results.cache.degenerates, settings.degenerates)) { return false; }
    if ((stages & comparisonTopology) && (runtime.results.value.original.topology.inspection.holeSizeRatioTolerance != settings.topologyInspection.holeSizeRatioTolerance
        || runtime.results.value.repaired.topology.inspection.holeSizeRatioTolerance != settings.topologyInspection.holeSizeRatioTolerance
        || runtime.results.value.original.topology.inspection.finMaxAreaRatio != settings.topologyInspection.finMaxAreaRatio
        || runtime.results.value.repaired.topology.inspection.finMaxAreaRatio != settings.topologyInspection.finMaxAreaRatio)) { return false; }
    if ((stages & comparisonIntersections) && (runtime.jobs.intersection.limits != settings.intersections.limits || runtime.results.value.original.intersections.phase != IntersectionPhase::complete)) { return false; }
    return true;
}

bool comparisonDetectorReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    DiagnosticCategory category, bool requireGpu)
{
    return comparisonDetectorReady(runtime, comparisonSettings(state, id), comparisonCurrentSignature(runtime, state, id), category, requireGpu);
}

bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id, bool fullResults)
{
    const auto& queries = runtime.inspector.queries;
    if (comparisonInspectorCacheCurrent(queries, state, id)) {
        const bool both = queries.inputs[0].summary.enabledPartCount && queries.inputs[1].summary.enabledPartCount;
        return comparisonResultsReady(runtime, state, id, queries.signature, both, fullResults);
    }
    const bool both = enabledComparisonPartCount(state, ComparisonSide::a, id) != 0
        && enabledComparisonPartCount(state, ComparisonSide::b, id) != 0;
    return comparisonResultsReady(runtime, state, id, comparisonGeometrySignature(state, id), both, fullResults);
}

bool comparisonResultsReady(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id,
    uint64_t signature, bool both, bool fullResults)
{
    const auto settings = comparisonSettings(state, id);
    const auto required = requestedComparisonStages(settings, both, fullResults) & ~(comparisonDetectors | comparisonIntersections);
    // A failed display upload must remain visible to the UI and RPC.
    if ((runtime.jobs.failedStages & required) || !comparisonStagesReady(runtime, settings, signature, required) || (!fullResults && !runtime.gpu.ready)) { return false; }
    const auto* comparison = findComparison(state, id);
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        const auto category = static_cast<DiagnosticCategory>(i);
        const auto phase = runtime.results.value.detectors[i].phase;
        if (comparison && comparison->detectorRequests[i].revision != runtime.jobs.consumedDetectorRequests[i]) { return false; }
        if (phase == IntersectionPhase::queued || phase == IntersectionPhase::running) { return false; }
        if (diagnosticAutoUpdate(settings, category) && phase != IntersectionPhase::canceled
            && phase != IntersectionPhase::failed && !(phase == IntersectionPhase::complete
                && comparisonStagesReady(runtime, settings, signature, comparisonDiagnosticStage(category)))) { return false; }
    }
    return true;
}

ComparisonSettings readyComparisonSettings(const ComparisonRuntime& runtime, const UiState& state, SceneObjectId id)
{
    const auto signature = comparisonCurrentSignature(runtime, state, id);
    const auto& queries = runtime.inspector.queries;
    auto settings = comparisonInspectorCacheCurrent(queries, state, id)
        ? effectiveComparisonSettings(state, id, queries.inputs[0].summary.enabledPartCount != 0,
            queries.inputs[1].summary.enabledPartCount != 0, signature)
        : effectiveComparisonSettings(state, id);
    const auto requested = comparisonSettings(state, id);
    const auto ready = [&](uint32_t stage) { return comparisonStagesReady(runtime, requested, signature, stage, true); };
    if ((settings.mode == ComparisonMode::distance && !ready(comparisonDistance))
        || (settings.mode == ComparisonMode::surfaceQuality && (!ready(comparisonQuality) || runtime.gpu.uploadedQualityMetric != settings.quality.metric))) {
        const bool original = settings.mode == ComparisonMode::distance ? settings.distanceOnOriginal : settings.quality.onOriginal;
        settings.mode = original ? ComparisonMode::original : ComparisonMode::repaired;
    }
    const auto detectorReady = [&](DiagnosticCategory category) { return comparisonDetectorReady(runtime, requested, signature, category, true); };
    settings.showBoundaries &= detectorReady(DiagnosticCategory::boundary);
    settings.showNonManifold &= detectorReady(DiagnosticCategory::nonManifold);
    settings.showWinding &= detectorReady(DiagnosticCategory::winding);
    settings.topologyInspection.nonManifoldVertices = detectorReady(DiagnosticCategory::nonManifoldVertices);
    settings.topologyInspection.holes = detectorReady(DiagnosticCategory::holes);
    settings.topologyInspection.fins = detectorReady(DiagnosticCategory::fins);
    settings.topologyInspection.showFins &= settings.topologyInspection.fins;
    settings.duplicates.points = detectorReady(DiagnosticCategory::duplicatePoints);
    settings.duplicates.triangles = detectorReady(DiagnosticCategory::duplicateTriangles);
    settings.degenerates.enabled = detectorReady(DiagnosticCategory::degenerateTriangles);
    settings.topologyInspection.showHoles &= settings.topologyInspection.holes;
    settings.topologyInspection.showNonManifoldVertices &= settings.topologyInspection.nonManifoldVertices;
    settings.duplicates.showPoints &= settings.duplicates.points;
    settings.duplicates.showTriangles &= settings.duplicates.triangles;
    settings.degenerates.show &= settings.degenerates.enabled;
    settings.intersections.show &= ready(comparisonIntersections);
    return settings;
}

static void updateComparisonRuntimeImpl(ComparisonRuntime& runtime, UiState& state, SceneObjectId id, bool allowStart)
{
    const auto settings = comparisonSettings(state, id);
    const auto* comparison = findComparison(state, id);
    if (!comparison) { return; }
    const uint64_t wanted = runtime.inspector.queries.signature;
    auto& job = runtime.jobs.intersection;
    const auto phase = [&](IntersectionPhase value, const std::string& error = std::string{}) {
        for (auto* surface : {&runtime.results.value.original, &runtime.results.value.repaired}) {
            surface->intersections.phase = value; surface->intersections.error = error;
        }
    };
    const auto invalidateGpu = [&](uint32_t stages) {
        destroyStage(runtime.gpu.original, stages); destroyStage(runtime.gpu.repaired, stages);
        runtime.gpu.uploadedStages &= ~stages;
    };
    const auto invalidateIntersection = [&] {
        job.stop.request_stop(); ++job.revision; job.requested = false; job.canceled = false;
        runtime.results.cache.completed &= ~comparisonIntersections;
        invalidateGpu(comparisonIntersections);
        for (auto* surface : {&runtime.results.value.original, &runtime.results.value.repaired}) {
            surface->intersections.phase = surface->intersections.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
            surface->intersections.error.clear();
            surface->intersectionBounds.clear(); surface->intersectionEdges.clear();
        }
    };
    if (job.limits != settings.intersections.limits) {
        job.limits = settings.intersections.limits;
        invalidateIntersection();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (resetComparisonCache(runtime.results.cache, wanted)) {
        const bool reuseUv = wanted && settings.type == AnalysisType::uvQuality && runtime.sources.inputs
            && runtime.sources.preparationSignature == runtime.inspector.queries.preparationSignature
            && ((runtime.gpu.uploadedStages & comparisonSource) || runtime.sources.refreshUvQuality);
        runtime.jobs.preparationStop.request_stop();
        runtime.sources.prepared.reset();
        runtime.jobs.stop.request_stop(); invalidateIntersection();
        invalidateComparisonDetectors(runtime.results.value, comparisonDetectors);
        auto detectors = std::move(runtime.results.value.detectors);
        auto a = std::move(runtime.results.value.original.intersections), b = std::move(runtime.results.value.repaired.intersections);
        if (!reuseUv) {
            destroySurface(runtime.gpu.original); destroySurface(runtime.gpu.repaired);
            runtime.results.value = {};
            runtime.sources.inputs.reset();
        }
        runtime.sources.refreshUvQuality = reuseUv;
        runtime.results.value.original.intersections = std::move(a); runtime.results.value.repaired.intersections = std::move(b);
        runtime.results.value.detectors = std::move(detectors);
        runtime.gpu.uploadedStages = runtime.jobs.failedStages = 0;
        runtime.jobs.retryDetectorsSeparately = false;
        runtime.jobs.allocationFailed = false;
        runtime.results.signature = runtime.jobs.attemptedSignature = 0; runtime.jobs.error.clear();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (runtime.jobs.preparationWorker.valid() && runtime.jobs.preparationWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto prepared = runtime.jobs.preparationWorker.get();
            if (!runtime.jobs.preparationStop.stop_requested() && runtime.jobs.preparationSignature == wanted) {
                runtime.sources.prepared = std::move(prepared);
                runtime.sources.inputs = runtime.sources.prepared->meshes;
                runtime.sources.preparationSignature = runtime.jobs.preparationGeometrySignature;
                if (runtime.sources.prepared->buffers[0].qualityOnly) {
                    runtime.results.value.original.source.uvQuality = runtime.sources.prepared->buffers[0].uvQuality;
                    runtime.results.value.repaired.source.uvQuality = runtime.sources.prepared->buffers[1].uvQuality;
                    runtime.results.cache.completed |= comparisonSource;
                    runtime.results.signature = wanted;
                    recordResultPublication(runtime.results, comparisonSource);
                }
            }
        } catch (const std::bad_alloc&) {
            if (!runtime.jobs.preparationStop.stop_requested() && runtime.jobs.preparationSignature == wanted) { throw; }
        } catch (const std::exception& error) {
            if (!runtime.jobs.preparationStop.stop_requested() && runtime.jobs.preparationSignature == wanted) {
                runtime.jobs.error = error.what();
                runtime.jobs.attemptedSignature = wanted;
                runtime.jobs.failedStages |= comparisonSource;
            }
        }
    }
    if (resetComparisonTopologyCache(runtime.results.cache, settings.topologyMode)) {
        if (runtime.jobs.workerStages & comparisonTopology) { runtime.jobs.stop.request_stop(); }
        invalidateIntersection(); invalidateGpu(comparisonTopology);
        invalidateComparisonDetectors(runtime.results.value, comparisonTopology);
        runtime.jobs.failedStages &= ~comparisonTopology; runtime.jobs.attemptedSignature = 0; runtime.jobs.error.clear();
        resetComparisonDiagnosticFocus(state, id);
    }
    if (resetComparisonDegenerateCache(runtime.results.cache, settings.degenerates)) {
        if (runtime.jobs.workerStages & comparisonDegenerates) { runtime.jobs.stop.request_stop(); }
        invalidateGpu(comparisonDegenerates); runtime.jobs.failedStages &= ~comparisonDegenerates;
        invalidateComparisonDetectors(runtime.results.value, comparisonDegenerates);
        runtime.jobs.attemptedSignature = 0; runtime.jobs.error.clear(); resetComparisonDiagnosticFocus(state, id);
    }
    if (runtime.jobs.holeSizeRatioTolerance != settings.topologyInspection.holeSizeRatioTolerance) {
        runtime.jobs.holeSizeRatioTolerance = settings.topologyInspection.holeSizeRatioTolerance;
        auto& status = runtime.results.value.detectors[static_cast<size_t>(DiagnosticCategory::holes)];
        status.phase = status.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
        status.error.clear();
    }
    if (runtime.jobs.finMaxAreaRatio != settings.topologyInspection.finMaxAreaRatio) {
        runtime.jobs.finMaxAreaRatio = settings.topologyInspection.finMaxAreaRatio;
        auto& status = runtime.results.value.detectors[static_cast<size_t>(DiagnosticCategory::fins)];
        status.phase = status.hasResult ? IntersectionPhase::outdated : IntersectionPhase::notChecked;
        status.error.clear();
    }
    uint32_t queuedStages = 0;
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        auto& status = runtime.results.value.detectors[i];
        const auto& request = comparison->detectorRequests[i];
        if (request.revision != runtime.jobs.consumedDetectorRequests[i]) {
            runtime.jobs.consumedDetectorRequests[i] = request.revision;
            if (request.revision != 0) {
                status.phase = request.cancel ? IntersectionPhase::canceled : wanted ? IntersectionPhase::queued : IntersectionPhase::notChecked;
                status.error.clear();
            }
        }
        if (wanted && (settings.enabled || runtime.jobs.fullResultsRequested)
            && diagnosticAutoUpdate(settings, static_cast<DiagnosticCategory>(i))
            && (status.phase == IntersectionPhase::notChecked || status.phase == IntersectionPhase::outdated)) {
            status.phase = IntersectionPhase::queued;
        }
        if (status.phase == IntersectionPhase::queued) { queuedStages |= comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)); }
    }
    const auto workerParticipates = [&](size_t i) {
        return (runtime.jobs.workerDetectors & (1u << i)) && runtime.results.value.detectors[i].phase == IntersectionPhase::running
            && runtime.jobs.workerDetectorRequests[i] == runtime.jobs.consumedDetectorRequests[i];
    };
    if (runtime.jobs.worker.valid() && runtime.jobs.workerDetectors) {
        bool any = false;
        for (size_t i = 0; i < backgroundDetectorCount; ++i) { any |= workerParticipates(i); }
        if (!any) { runtime.jobs.stop.request_stop(); }
    }
    const auto requeueCanceledWorker = [&] {
        // A threshold/mode edit can stop a batch containing other detectors.
        // Retry its remaining participants; explicit cancellations/new requests
        // have already changed phase/revision and must not be overwritten.
        for (size_t i = 0; i < backgroundDetectorCount; ++i) {
            if (workerParticipates(i)) {
                runtime.results.value.detectors[i].phase = IntersectionPhase::queued;
                queuedStages |= comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i));
            }
        }
    };
    if (comparison->intersectionRequestRevision != job.consumedRequest) {
        job.consumedRequest = comparison->intersectionRequestRevision;
        if (job.consumedRequest != 0) {
            job.stop.request_stop(); ++job.revision;
            job.requested = !comparison->cancelIntersections && wanted != 0;
            job.canceled = comparison->cancelIntersections;
            runtime.results.cache.completed &= ~comparisonIntersections; runtime.jobs.failedStages &= ~comparisonIntersections;
            invalidateGpu(comparisonIntersections);
            phase(job.canceled ? IntersectionPhase::canceled : job.requested ? IntersectionPhase::queued : IntersectionPhase::notChecked);
        }
    }
    if (settings.intersections.autoUpdate != job.autoUpdate) {
        job.autoUpdate = settings.intersections.autoUpdate;
    }
    if (runtime.jobs.worker.valid() && runtime.jobs.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto update = runtime.jobs.worker.get();
            if (!runtime.jobs.stop.stop_requested()) {
                auto previous = runtime.results.value.detectors;
                uint32_t participants = 0;
                for (size_t i = 0; i < backgroundDetectorCount; ++i) { if (workerParticipates(i)) { participants |= 1u << i; } }
                if (applyComparisonStages(runtime.results.value, runtime.results.cache, std::move(update), runtime.jobs.workerSignature, runtime.jobs.workerStages)) {
                    recordResultPublication(runtime.results, runtime.jobs.workerStages);
                    invalidateGpu(runtime.jobs.workerStages);
                    runtime.results.signature = wanted; runtime.jobs.failedStages &= ~runtime.jobs.workerStages;
                    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                        if (!(participants & (1u << i))) { runtime.results.value.detectors[i] = std::move(previous[i]); }
                    }
                    if (!runtime.jobs.failedStages) { runtime.jobs.error.clear(); }
                }
            } else { requeueCanceledWorker(); }
        } catch (const std::bad_alloc&) {
            if (!runtime.jobs.stop.stop_requested() && wanted == runtime.jobs.workerSignature) { throw; }
            requeueCanceledWorker();
        } catch (const std::exception& error) {
            if (!runtime.jobs.stop.stop_requested() && wanted == runtime.jobs.workerSignature) {
                const auto detectors = runtime.jobs.workerStages & comparisonDetectors;
                if (detectors && (detectors & (detectors - 1))) {
                    // Identify the failing stage without discarding independent
                    // results or labeling every detector in the batch failed.
                    runtime.jobs.retryDetectorsSeparately = true;
                    requeueCanceledWorker();
                } else {
                    runtime.jobs.error = error.what(); runtime.jobs.failedStages |= runtime.jobs.workerStages;
                    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                        if (workerParticipates(i)) { runtime.results.value.detectors[i].phase = IntersectionPhase::failed; runtime.results.value.detectors[i].error = error.what(); }
                    }
                }
            } else { runtime.jobs.attemptedSignature = 0; requeueCanceledWorker(); }
        }
    }
    if (job.worker.valid() && job.worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto update = job.worker.get();
            if (!job.stop.stop_requested() && job.workerRevision == job.revision && job.workerSignature == wanted
                && applyComparisonStages(runtime.results.value, runtime.results.cache, std::move(update), job.workerSignature, comparisonIntersections)) {
                runtime.results.signature = wanted; recordResultPublication(runtime.results, comparisonIntersections);
                invalidateGpu(comparisonIntersections);
            }
        } catch (const std::bad_alloc&) {
            if (!job.stop.stop_requested() && job.workerRevision == job.revision && job.workerSignature == wanted) { throw; }
        } catch (const std::exception& error) {
            if (!job.stop.stop_requested() && job.workerRevision == job.revision && job.workerSignature == wanted) { phase(IntersectionPhase::failed, error.what()); }
        }
    }
    if (wanted && settings.intersections.autoUpdate && !job.canceled && !job.worker.valid()
        && !(runtime.results.cache.completed & comparisonIntersections) && runtime.results.value.original.intersections.phase != IntersectionPhase::failed) {
        job.requested = true;
    }
    if (job.requested) { phase(IntersectionPhase::queued); }
    auto resultSettings = settings;
    resultSettings.duplicates.points = resultSettings.duplicates.triangles = true;
    resultSettings.degenerates.enabled = true;
    resultSettings.topologyInspection.nonManifoldVertices = resultSettings.topologyInspection.holes = resultSettings.topologyInspection.fins = true;
    setComparisonDuplicateEnabled(runtime.results.value, resultSettings.duplicates);
    setComparisonDegenerateSettings(runtime.results.value, resultSettings.degenerates);
    setComparisonIntersectionSettings(runtime.results.value, settings.intersections);
    if (setComparisonTopologyInspectionSettings(runtime.results.value, resultSettings.topologyInspection)) { recordResultPublication(runtime.results, comparisonTopology); invalidateGpu(comparisonTopology); }
    // Hole and fin thresholds filter the current graph; neither needs a rebuild.
    // Only queued requests complete here, preserving manual/outdated/canceled
    // states and the geometry/mode invalidation checked above.
    for (const auto category : {DiagnosticCategory::holes, DiagnosticCategory::fins}) {
        auto& status = runtime.results.value.detectors[static_cast<size_t>(category)];
        if (status.phase == IntersectionPhase::queued && (runtime.results.cache.completed & comparisonTopology)
            && !(runtime.jobs.failedStages & comparisonTopology)) {
            status.phase = IntersectionPhase::complete; status.hasResult = true;
        }
    }
    queuedStages = 0;
    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
        if (runtime.results.value.detectors[i].phase == IntersectionPhase::queued) {
            queuedStages |= comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i));
        }
    }
    // Filtering may change counts from the worker's default thresholds.
    auto& holes = runtime.results.value.detectors[static_cast<size_t>(DiagnosticCategory::holes)];
    if (holes.phase == IntersectionPhase::complete) {
        holes.knownCounts = {runtime.results.value.original.topology.holes.size(), runtime.results.value.repaired.topology.holes.size()};
    }
    auto& fins = runtime.results.value.detectors[static_cast<size_t>(DiagnosticCategory::fins)];
    if (fins.phase == IntersectionPhase::complete) {
        fins.knownCounts = {runtime.results.value.original.topology.fins.size(), runtime.results.value.repaired.topology.fins.size()};
    }
    const bool active = settings.enabled || runtime.jobs.fullResultsRequested || job.requested || queuedStages;
    // Begin CPU work before staging the previous result on the GPU.
    // Source upload and detector computation use independent snapshots.
    const auto schedule = [&] {
        const bool both = runtime.inspector.queries.inputs[0].summary.enabledPartCount && runtime.inspector.queries.inputs[1].summary.enabledPartCount;
        const auto requested = requestedComparisonStages(settings, both, runtime.jobs.fullResultsRequested) & ~(comparisonDetectors | comparisonIntersections);
        const auto missing = (requested & ~runtime.results.cache.completed & ~runtime.jobs.failedStages) | queuedStages;
        if (!wanted || !allowStart || !active || (!missing && !job.requested)) { return; }
        try {
            if (runtime.sources.refreshUvQuality && (missing & comparisonSource)) {
                if (!runtime.jobs.preparationWorker.valid()) {
                    const std::array qualities{runtime.results.value.original.source.uvQuality,
                        runtime.results.value.repaired.source.uvQuality};
                    runtime.jobs.preparationStop = std::stop_source{};
                    runtime.jobs.preparationSignature = runtime.jobs.attemptedSignature = wanted;
                    runtime.jobs.preparationGeometrySignature = runtime.inspector.queries.preparationSignature;
                    runtime.jobs.preparationWorker = std::async(std::launch::async,
                        [inputs = runtime.sources.inputs, qualities, settings, stop = runtime.jobs.preparationStop.get_token()] {
                            return std::make_shared<const PreparedComparisonInputs>(refreshUvComparisonInputs(inputs, qualities, settings, stop));
                        });
                }
                return;
            }
            if (!runtime.sources.inputs) {
                if (isUvAnalysis(settings.type)) {
                    if (!runtime.jobs.preparationWorker.valid()) {
                        auto snapshot = snapshotComparisonInputs(state, id);
                        runtime.jobs.preparationStop = std::stop_source{};
                        runtime.jobs.preparationSignature = runtime.jobs.attemptedSignature = wanted;
                        runtime.jobs.preparationGeometrySignature = runtime.inspector.queries.preparationSignature;
                        runtime.jobs.preparationWorker = std::async(std::launch::async,
                            [snapshot = std::move(snapshot), stop = runtime.jobs.preparationStop.get_token()]() -> std::shared_ptr<const PreparedComparisonInputs> {
                                return std::make_shared<PreparedComparisonInputs>(prepareUvComparisonInputs(snapshot, stop));
                            });
                    }
                    return;
                }
                auto inputs = std::make_shared<std::array<Mesh, 2>>();
                if (runtime.inspector.queries.inputs[0].summary.enabledPartCount) { (*inputs)[0] = comparisonWorldMesh(state, ComparisonSide::a, id); }
                if (runtime.inspector.queries.inputs[1].summary.enabledPartCount) { (*inputs)[1] = comparisonWorldMesh(state, ComparisonSide::b, id); }
                runtime.sources.inputs = std::move(inputs);
            }
            if (missing && !runtime.jobs.worker.valid()) {
                runtime.jobs.attemptedSignature = runtime.jobs.workerSignature = wanted;
                auto stages = nextComparisonStage(missing);
                if (runtime.jobs.retryDetectorsSeparately && (stages & comparisonDetectors)) {
                    stages &= (~stages + 1); // Retry only the first requested stage.
                }
                runtime.jobs.attemptedStages = runtime.jobs.workerStages = stages;
                runtime.jobs.workerDetectors = 0;
                for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                    auto& status = runtime.results.value.detectors[i];
                    if (status.phase == IntersectionPhase::queued && (comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)) & runtime.jobs.workerStages)) {
                        status.phase = IntersectionPhase::running;
                        runtime.jobs.workerDetectors |= 1u << i;
                        runtime.jobs.workerDetectorRequests[i] = runtime.jobs.consumedDetectorRequests[i];
                    }
                }
                runtime.jobs.stop = std::stop_source{};
                const std::array qualities{
                    runtime.sources.prepared ? runtime.sources.prepared->buffers[0].uvQuality : nullptr,
                    runtime.sources.prepared ? runtime.sources.prepared->buffers[1].uvQuality : nullptr};
                runtime.jobs.worker = std::async(std::launch::async, [inputs = runtime.sources.inputs, stage = runtime.jobs.workerStages,
                    qualities, degenerates = settings.degenerates, mode = settings.topologyMode, stop = runtime.jobs.stop.get_token()] {
                    auto result = computeComparisonStages((*inputs)[0], (*inputs)[1], stage, stop, degenerates, mode);
                    if (stage & comparisonSource) {
                        if (qualities[0]) { result.original.source.uvQuality = qualities[0]; }
                        if (qualities[1]) { result.repaired.source.uvQuality = qualities[1]; }
                    }
                    return result;
                });
            } else if (job.requested && !runtime.jobs.worker.valid() && !job.worker.valid()) {
                job.requested = false; job.workerSignature = wanted; job.workerRevision = job.revision;
                job.stop = std::stop_source{}; job.started = std::chrono::steady_clock::now(); phase(IntersectionPhase::running);
                job.worker = std::async(std::launch::async, [inputs = runtime.sources.inputs, mode = settings.topologyMode, limits = settings.intersections.limits, stop = job.stop.get_token()] {
                    return computeComparisonStages((*inputs)[0], (*inputs)[1], comparisonIntersections, stop, {}, mode, limits);
                });
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            if (job.requested) { job.requested = false; phase(IntersectionPhase::failed, error.what()); }
            else {
                runtime.jobs.error = error.what(); runtime.jobs.failedStages |= missing;
                for (auto& status : runtime.results.value.detectors) {
                    if (status.phase == IntersectionPhase::queued || status.phase == IntersectionPhase::running) {
                        status.phase = IntersectionPhase::failed; status.error = error.what();
                    }
                }
            }
        }
    };
    schedule();
    if (wanted && active) {
        const auto pending = runtime.results.cache.completed & ~runtime.gpu.uploadedStages & ~runtime.jobs.failedStages;
        for (const auto stage : {comparisonSource, comparisonTopology, comparisonDuplicatePoints, comparisonDuplicateTriangles,
            comparisonDegenerates, comparisonQuality, comparisonDistance, comparisonIntersections}) {
            if (!(pending & stage)) { continue; }
            try {
                uploadSurface(runtime.gpu.original, runtime.results.value.original, stage, runtime.sources.prepared ? &runtime.sources.prepared->buffers[0] : nullptr);
                uploadSurface(runtime.gpu.repaired, runtime.results.value.repaired, stage, runtime.sources.prepared ? &runtime.sources.prepared->buffers[1] : nullptr);
                runtime.gpu.uploadedStages |= stage;
                for (size_t i = 0; i < runtime.gpu.stageRevisions.size(); ++i) {
                    if (stage & (1u << i)) { runtime.gpu.stageRevisions[i] = runtime.results.stageRevisions[i]; }
                }
                if (stage & comparisonSource) {
                    runtime.sources.prepared.reset();
                    runtime.sources.refreshUvQuality = false;
                }
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& error) {
                invalidateGpu(stage); runtime.jobs.failedStages |= stage; runtime.jobs.attemptedSignature = wanted;
                if (stage & comparisonSource) { runtime.sources.refreshUvQuality = false; }
                if (stage == comparisonIntersections) { phase(IntersectionPhase::failed, error.what()); }
                else {
                    runtime.jobs.error = error.what();
                    for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                        if ((comparisonDiagnosticStage(static_cast<DiagnosticCategory>(i)) & stage)
                            && runtime.results.value.detectors[i].phase == IntersectionPhase::complete) {
                            runtime.results.value.detectors[i].phase = IntersectionPhase::failed;
                            runtime.results.value.detectors[i].error = error.what();
                        }
                    }
                }
            }
        }
        if ((runtime.results.cache.completed & comparisonQuality) && settings.mode == ComparisonMode::surfaceQuality
            && !(runtime.jobs.failedStages & comparisonQuality) && (runtime.gpu.uploadedQualityMetric != settings.quality.metric
                || (!runtime.results.value.original.source.indices.empty() && !woby::graphics::isValid(runtime.gpu.original.quality))
                || (!runtime.results.value.repaired.source.indices.empty() && !woby::graphics::isValid(runtime.gpu.repaired.quality)))) {
            try {
                const auto& distribution = runtime.results.value.qualityDistributions.at(static_cast<size_t>(settings.quality.metric));
                uploadQuality(runtime.gpu.original, runtime.results.value.original, settings.quality.metric, distribution);
                uploadQuality(runtime.gpu.repaired, runtime.results.value.repaired, settings.quality.metric, distribution);
                runtime.gpu.uploadedQualityMetric = settings.quality.metric;
            } catch (const std::bad_alloc&) { throw; }
            catch (const std::exception& error) { runtime.jobs.failedStages |= comparisonQuality; runtime.jobs.error = error.what(); }
        }
    }
    runtime.gpu.ready = wanted && settings.enabled && (runtime.results.cache.completed & comparisonSource) && (runtime.gpu.uploadedStages & comparisonSource);

}

static void updateComparisonRuntime(ComparisonRuntime& runtime, UiState& state, SceneObjectId id, bool allowStart)
{
    if (runtime.jobs.allocationFailed) {
        // Drain stopped workers without blocking the UI or publishing partial work.
        const auto drain = [](auto& worker) {
            if (!worker.valid()) { return true; }
            if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { return false; }
            try { (void)worker.get(); } catch (...) { }
            return true;
        };
        const bool prepared = drain(runtime.jobs.preparationWorker);
        const bool computed = drain(runtime.jobs.worker);
        const bool intersected = drain(runtime.jobs.intersection.worker);
        if (!prepared || !computed || !intersected) { return; }
        const auto* comparison = findComparison(state, id);
        bool retry = runtime.jobs.error.empty() || runtime.jobs.attemptedSignature != comparisonCurrentSignature(runtime, state, id);
        if (comparison) {
            for (size_t i = 0; i < backgroundDetectorCount; ++i) {
                retry |= comparison->detectorRequests[i].revision != runtime.jobs.consumedDetectorRequests[i]
                    && !comparison->detectorRequests[i].cancel;
            }
            retry |= comparison->intersectionRequestRevision != runtime.jobs.intersection.consumedRequest
                && !comparison->cancelIntersections;
        }
        if (!retry) { return; }
        runtime.jobs.allocationFailed = false;
        runtime.jobs.failedStages = 0;
        runtime.jobs.error.clear();
        for (auto& status : runtime.results.value.detectors) { status.phase = IntersectionPhase::notChecked; status.error.clear(); }
        for (auto* surface : {&runtime.results.value.original, &runtime.results.value.repaired}) {
            surface->intersections.phase = IntersectionPhase::notChecked;
            surface->intersections.error.clear();
        }
    }
    try {
        updateComparisonRuntimeImpl(runtime, state, id, allowStart);
    } catch (const std::bad_alloc&) {
        runtime.jobs.stop.request_stop();
        runtime.jobs.preparationStop.request_stop();
        runtime.jobs.intersection.stop.request_stop();
        runtime.jobs.intersection.requested = false;
        runtime.sources.prepared.reset(); runtime.sources.inputs.reset();
        runtime.sources.refreshUvQuality = false;
        // Release large retained buffers before constructing the error message.
        runtime.results.value = {};
        destroySurface(runtime.gpu.original); destroySurface(runtime.gpu.repaired);
        runtime.results.cache.completed = runtime.gpu.uploadedStages = 0;
        runtime.gpu.ready = false;
        runtime.jobs.retryDetectorsSeparately = false;
        runtime.jobs.allocationFailed = true;
        runtime.jobs.failedStages = comparisonSource | comparisonDetectors | comparisonQuality | comparisonDistance | comparisonIntersections;
        runtime.jobs.attemptedSignature = runtime.results.cache.signature;
        runtime.jobs.error = "Memory allocation failed. Analysis stopped; free memory and retry.";
        for (auto& status : runtime.results.value.detectors) { status.phase = IntersectionPhase::failed; status.error = runtime.jobs.error; }
        for (auto* surface : {&runtime.results.value.original, &runtime.results.value.repaired}) {
            surface->intersections.phase = IntersectionPhase::failed;
            surface->intersections.error = runtime.jobs.error;
        }
        runtime.results.revision = nextResultsRevision();
    }
}

static void destroyComparisonRuntime(ComparisonRuntime &runtime)
{
    runtime.jobs.preparationStop.request_stop();
    if (runtime.jobs.preparationWorker.valid()) { runtime.jobs.preparationWorker.wait(); }
    runtime.jobs.intersection.stop.request_stop();
    if (runtime.jobs.intersection.worker.valid()) { runtime.jobs.intersection.worker.wait(); }
    if (runtime.jobs.worker.valid())
    {
        runtime.jobs.stop.request_stop();
        runtime.jobs.worker.wait();
    }
    destroySurface(runtime.gpu.original);
    destroySurface(runtime.gpu.repaired);
}

void updateComparisonRuntimes(ComparisonRuntimes& runtimes, UiState& state)
{
    if (runtimes.owner && (runtimes.owner != &state || runtimes.generation != state.sceneGeneration)) {
        // Cancel every job first; shared graphics programs belong to the application, not the document.
        for (auto& [id, runtime] : runtimes.objects) {
            (void)id;
            runtime.jobs.preparationStop.request_stop(); runtime.jobs.stop.request_stop(); runtime.jobs.intersection.stop.request_stop();
        }
        for (auto& [id, runtime] : runtimes.objects) { (void)id; destroyComparisonRuntime(runtime); }
        runtimes.objects.clear();
    }
    runtimes.owner = &state;
    runtimes.generation = state.sceneGeneration;
    for (auto it = runtimes.objects.begin(); it != runtimes.objects.end();) {
        if (!findComparison(state, it->first)) {
            destroyComparisonRuntime(it->second);
            it = runtimes.objects.erase(it);
        } else { ++it; }
    }
    for (const auto& comparison : state.comparisons) {
        auto& runtime = runtimes.objects[comparison.objectId];
        size_t active = 0;
        for (const auto& item : runtimes.objects) {
            active += item.second.jobs.preparationWorker.valid();
            active += item.second.jobs.worker.valid(); active += item.second.jobs.intersection.worker.valid();
        }
        updateComparisonInspectorCache(runtime.inspector.queries, state, comparison.objectId);
        updateComparisonRuntime(runtime, state, comparison.objectId, active < 2);
        for (size_t index = 0; index < diagnosticCategoryCount; ++index) {
            const auto category = static_cast<DiagnosticCategory>(index);
            const auto phase = comparisonDetectorStatus(runtime.results.value, category).phase;
            const auto stage = comparisonDiagnosticStage(category);
            const bool current = comparisonStagesReady(runtime, comparison.settings, runtime.inspector.queries.signature, stage);
            runtime.inspector.diagnosticSummaries[index] = {
                diagnosticSummary(runtime.results.value.original, category, phase, current),
                diagnosticSummary(runtime.results.value.repaired, category, phase, current)};
        }
        validateComparisonDiagnosticFocus(state, runtime.results.value,
            runtime.gpu.ready ? runtime.results.signature : 0, comparison.objectId, runtime.inspector.queries.signature);
        validateUvFindingFocus(state, runtime.results.value.original.source,
            runtime.gpu.ready ? runtime.results.signature : 0, comparison.objectId);
    }
}

void destroyComparisonRuntimes(ComparisonRuntimes& runtimes)
{
    for (auto& [id, runtime] : runtimes.objects) { (void)id; runtime.jobs.preparationStop.request_stop(); runtime.jobs.stop.request_stop(); }
    for (auto& [id, runtime] : runtimes.objects) { (void)id; destroyComparisonRuntime(runtime); }
    runtimes.objects.clear();
    if (woby::graphics::isValid(runtimes.program)) { woby::graphics::destroy(runtimes.program); runtimes.program = WOBY_GPU_INVALID_HANDLE; }
    if (woby::graphics::isValid(runtimes.parameters)) { woby::graphics::destroy(runtimes.parameters); runtimes.parameters = WOBY_GPU_INVALID_HANDLE; }
}

bool comparisonsReadyForScreenshot(const UiState& state, const ComparisonRuntimes& runtimes)
{
    bool ready = true;
    for (const auto& comparison : state.comparisons) {
        if (!comparison.settings.enabled) { continue; }
        if (!canInspectComparison(state, comparison.objectId)) {
            std::string issues;
            for (const auto side : {ComparisonSide::a, ComparisonSide::b}) {
                const auto input = comparisonInputSummary(state, side, comparison.objectId);
                if (!input.issue.empty()) { issues += " " + input.issue; }
            }
            throw std::runtime_error(comparison.name + ":" + issues);
        }
        const auto signature = comparisonGeometrySignature(state, comparison.objectId);
        const auto it = runtimes.objects.find(comparison.objectId);
        if (it == runtimes.objects.end()) { ready = false; continue; }
        const auto& runtime = it->second;
        if (!runtime.jobs.error.empty() && runtime.jobs.attemptedSignature == signature
            && !comparisonResultsReady(runtime, state, comparison.objectId)) {
            throw std::runtime_error(comparison.name + ": " + runtime.jobs.error);
        }
        ready = ready && comparisonResultsReady(runtime, state, comparison.objectId);
        const auto phase = runtime.results.value.original.intersections.phase;
        if (comparison.settings.intersections.show && (runtime.jobs.intersection.requested
            || phase == IntersectionPhase::queued || phase == IntersectionPhase::running
            || findComparison(state, comparison.objectId)->intersectionRequestRevision != runtime.jobs.intersection.consumedRequest)) { ready = false; }
        if (comparison.settings.mode == ComparisonMode::surfaceQuality) {
            ready = ready && runtime.gpu.uploadedQualityMetric == comparison.settings.quality.metric &&
                (runtime.results.value.original.source.indices.empty() || woby::graphics::isValid(runtime.gpu.original.quality)) &&
                (runtime.results.value.repaired.source.indices.empty() || woby::graphics::isValid(runtime.gpu.repaired.quality));
        }
    }
    return ready;
}

} // namespace woby
