#include "cross_section_runtime.h"
#include "annotation_work.h"
#include "scene_queries.h"
#include <map>

namespace woby {
struct SectionRequest {
    SceneObjectId id = 0;
    CrossSectionSettings settings;
    std::vector<SceneObjectId> sources;
    std::optional<SceneObjectId> analysis;
    std::vector<ComparisonSide> sides;
    bool operator==(const SectionRequest&) const = default;
};
struct CrossSectionRuntime {
    const UiState* owner = nullptr;
    uint64_t generation = 0, geometry = 0, analysis = 0, presentation = 0;
    std::vector<SectionRequest> requests;
    std::map<SceneObjectId, std::shared_ptr<const CrossSectionResult>> results;
    std::shared_ptr<std::map<SceneObjectId, std::shared_ptr<const CrossSectionResult>>> pending;
    std::shared_ptr<AnnotationWork> work;
    bool published = false;
    AnnotationExecutor executor;
};
bool updateCrossSections(SceneQueryRuntime& runtime, const UiState& state)
{
    std::vector<SectionRequest> requests;
    bool analysisSections = false;
    for (const auto& item : state.annotations) {
        if (!item.section) {
            continue;
        }
        auto settings = *item.section;
        settings.lift = 0;
        settings.showPlane = false;
        settings.showConnections = false;
        settings.faceOn = false;
        settings.sourceVisible.clear();
        requests.push_back({item.objectId, std::move(settings), item.targetIds, item.sectionAnalysis, item.sectionSides});
        analysisSections |= item.sectionAnalysis.has_value();
    }
    if (!runtime.sections && requests.empty()) {
        return false;
    }
    if (!runtime.sections) {
        runtime.sections = std::make_shared<CrossSectionRuntime>();
    }
    auto& cache = *runtime.sections;
    bool changed = false;
    if (cache.owner != &state || cache.generation != state.sceneGeneration ||
        cache.geometry != state.revisions.geometry || cache.requests != requests
        || (analysisSections && (cache.analysis != state.revisions.analysis || cache.presentation != state.revisions.presentation))) {
        cancelAnnotationWork(cache.work);
        cache.owner = &state;
        cache.generation = state.sceneGeneration;
        cache.geometry = state.revisions.geometry;
        cache.analysis = state.revisions.analysis; cache.presentation = state.revisions.presentation;
        cache.requests = requests;
        cache.results.clear();
        cache.published = false;
        changed = true;
        cache.pending = std::make_shared<std::map<SceneObjectId, std::shared_ptr<const CrossSectionResult>>>();
        if (requests.empty()) {
            cache.work.reset();
            return true;
        }
        std::map<SceneObjectId, AnnotationPartsSnapshot> snapshots;
        for (const auto& request : requests) {
            // Zero is reserved for ordinary scene sources. Missing analyses use
            // an empty owned snapshot and cannot fall back to the source scene.
            const auto key = request.analysis.value_or(0);
            if (request.analysis && !key) { continue; }
            if (snapshots.contains(key)) { continue; }
            std::vector<ScenePickPart> parts;
            if (request.analysis) { parts = comparisonSectionParts(state, key); }
            else { resolveSceneParts(runtime, state, parts, true); }
            snapshots.emplace(key, snapshotAnnotationParts(parts));
        }
        auto output = cache.pending;
        cache.work = std::make_shared<AnnotationWork>();
        cache.work->compute = [requests = std::move(requests), snapshots = std::move(snapshots),
                               output](std::stop_token stop) {
            for (const auto& request : requests) {
                if (stop.stop_requested()) {
                    return;
                }
                std::span<const ScenePickPart> parts;
                if (!request.analysis || *request.analysis) {
                    if (const auto it = snapshots.find(request.analysis.value_or(0)); it != snapshots.end()) { parts = it->second.parts; }
                }
                auto result = cutCrossSection(request.settings, request.sources, parts, stop, request.sides);
                if (stop.stop_requested()) {
                    return;
                }
                output->emplace(request.id, std::make_shared<const CrossSectionResult>(std::move(result)));
            }
        };
        submitAnnotationWork(cache.executor, cache.work);
    }
    if (cache.work && !cache.published && cache.work->done.load(std::memory_order_acquire)) {
        cache.published = true;
        if (!cache.work->cancel.stop_requested() && cache.work->error.empty()) {
            cache.results = *cache.pending;
        }
        changed = true;
    }
    return changed;
}
std::shared_ptr<const CrossSectionResult> crossSectionResult(const SceneQueryRuntime& runtime, SceneObjectId id)
{
    if (!runtime.sections) {
        return {};
    }
    const auto it = runtime.sections->results.find(id);
    return it == runtime.sections->results.end() ? nullptr : it->second;
}
std::string crossSectionError(const SceneQueryRuntime& runtime)
{
    if (!runtime.sections || !runtime.sections->published || !runtime.sections->work) {
        return {};
    }
    return runtime.sections->work->error;
}
} // namespace woby
