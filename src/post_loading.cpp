#include "post_loading.h"
#include "simd_bounds.h"
#include "mesh_duplicates.h"
#include "surface_annotation.h"
#include "ui_operations.h"
#include "parallel_work.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace woby {
PostLoadRuntime::~PostLoadRuntime() { cancelPostLoading(*this); }
namespace {
struct PreparationCanceled {};
void finishBox(Bounds& box)
{
    for (size_t k = 0; k < 3; ++k) { box.center[k] = (box.min[k] + box.max[k]) * .5f; }
    float radiusSquared = 0;
    for (size_t corner = 0; corner < 8; ++corner) {
        float distance = 0;
        for (size_t k = 0; k < 3; ++k) {
            const float offset = ((corner & (size_t{1} << k)) ? box.max[k] : box.min[k]) - box.center[k];
            distance += offset * offset;
        }
        radiusSquared = std::max(radiusSquared, distance);
    }
    box.radius = std::max(.001f, std::sqrt(radiusSquared));
}
void retireJob(PostLoadRuntime& runtime)
{
    if (runtime.job) {
        runtime.job->worker.request_stop();
        runtime.retired.push_back(std::move(runtime.job));
    }
    if (runtime.upload) { abortGpuMeshUpload(*runtime.upload); runtime.upload.reset(); }
}
}

std::optional<PostLoadData> preparePostLoadData(const Mesh& mesh, const std::function<bool()>& canceled)
{
    std::mutex cancelMutex;
    bool stopped = false;
    const auto stop = [&] {
        std::lock_guard lock(cancelMutex);
        if (!stopped && canceled) { stopped = canceled(); }
        return stopped;
    };
    if (stop()) { return {}; }
    PostLoadData result;
    const auto& geometry = meshGeometry(mesh);
    const auto prepareModel = [&] {
        if (mesh.sourceData && mesh.sourceData->indicesPending) {
            auto source = std::make_shared<SourceMeshData>();
            source->provenance = mesh.sourceData->provenance;
            source->positions = mesh.sourceData->positions;
            source->originalPointIds = mesh.sourceData->originalPointIds;
            source->firstGeneratedPosition = mesh.sourceData->firstGeneratedPosition;
            auto indices = mapSourceTriangleIndices(mesh, stop);
            if (!indices) { throw PreparationCanceled{}; }
            source->indices = std::move(*indices);
            result.source = std::move(source);
        } else { result.source = mesh.sourceData; }
        const auto originalBounds = prepareOriginalMeshBounds(mesh, stop);
        if (!originalBounds) { throw PreparationCanceled{}; }
        result.originalBounds = *originalBounds;
        auto points = prepareSceneMesh(mesh, gpuMeshPoints, stop);
        if (!points) { throw PreparationCanceled{}; }
        const auto vertices = points->pointCloud ? std::span<const uint32_t>{points->pointCloud->sourceVertices}
            : std::span<const uint32_t>{points->pointVertexIndices};
        result.groups.reserve(points->nodeRanges.size());
        for (const auto& range : points->nodeRanges) {
            if (stop()) { throw PreparationCanceled{}; }
            PostLoadGroupBounds bounds;
            if (!range.pointIndexCount || range.pointIndexCount == geometry.vertices.size()) {
                bounds.local = mesh.bounds;
                if (range.pointIndexCount) { finishBox(bounds.local); }
                bounds.original = range.pointIndexCount ? result.originalBounds : std::array<Coordinate, 2>{};
            } else {
                auto localReduction = floatBoundsReduction({}, {});
                auto groupOriginalReduction = doubleBoundsReduction({});
                for (size_t i = 0; i < range.pointIndexCount; ++i) {
                    if (i % 65536 == 0 && stop()) { throw PreparationCanceled{}; }
                    const auto vertex = vertices[range.pointIndexOffset + i];
                    const auto& local = geometry.vertices[vertex].position;
                    const auto original = originalPosition(meshPosition(mesh, vertex), mesh.origin);
                    if (!i) {
                        localReduction = floatBoundsReduction(local, local);
                        groupOriginalReduction = doubleBoundsReduction(original);
                    } else {
                        expand(localReduction, local);
                        expand(groupOriginalReduction, original);
                    }
                }
                const auto localBox = boundsResult(localReduction);
                bounds.local.min = localBox[0]; bounds.local.max = localBox[1];
                bounds.original = boundsResult(groupOriginalReduction);
                finishBox(bounds.local);
            }
            result.groups.push_back(bounds);
        }
        result.points = std::move(*points);
    };
    // Annotations read the original geometry, not the prepared source or bounds.
    // Run both tasks from the start. They write separate result members, and all
    // nested work shares the helper budget. Join both before publishing or uploading.
    try {
        parallelWorkBatches(geometry.indices.empty() ? 1 : 2, 1, [&] {
            if (stop()) { throw PreparationCanceled{}; }
        }, [&](size_t begin, size_t end) {
            for (size_t task = begin; task < end; ++task) {
                if (task == 0) { prepareModel(); }
                else { result.annotations = buildAnnotationMeshCache(mesh, stop); }
            }
        }, 2);
    } catch (const PreparationCanceled&) { return {}; }
    catch (...) {
        if (stop()) { return {}; }
        throw;
    }
    if (stop()) { return {}; }
    return result;
}

PostLoadIdentity postLoadIdentity(const UiState& state, const UiFileState& file)
{
    return {&state, state.sceneGeneration, file.mesh.contentRevision, file.objectId, file.mesh.storage};
}
bool postLoadCurrent(const PostLoadIdentity& identity, const UiState& state)
{
    if (identity.owner != &state || identity.generation != state.sceneGeneration) { return false; }
    return std::any_of(state.files.begin(), state.files.end(), [&](const auto& file) {
        return file.objectId == identity.fileId && file.mesh.contentRevision == identity.revision
            && file.mesh.storage == identity.geometry && !file.mesh.draft;
    });
}

void notePostLoadFrame(PostLoadRuntime& runtime, const UiState& state, uint64_t token)
{
    if (!token) { return; }
    for (const auto& file : state.files) {
        if (file.postLoadPhase != PostLoadPhase::waitingForFrame) { continue; }
        const auto identity = postLoadIdentity(state, file);
        if (std::none_of(runtime.frames.begin(), runtime.frames.end(), [&](const auto& frame) {
            return frame.identity.fileId == file.objectId && postLoadCurrent(frame.identity, state);
        })) { runtime.frames.push_back({identity, token}); }
    }
}

void updatePostLoading(PostLoadRuntime& runtime, UiState& state, std::vector<LoadedModelRuntime>& models,
    const graphics::VertexLayout& layout, uint64_t completedFrame)
{
    std::erase_if(runtime.retired, [](const auto& job) { return job->done.load(std::memory_order_acquire); });
    std::erase_if(runtime.frames, [&](const auto& frame) {
        return !postLoadCurrent(frame.identity, state) || std::any_of(state.files.begin(), state.files.end(),
            [&](const auto& file) { return file.objectId == frame.identity.fileId && postLoadReady(file); });
    });
    if (runtime.job && !postLoadCurrent(runtime.job->identity, state)) { retireJob(runtime); }
    for (const auto& file : state.files) {
        const bool orphaned = (file.postLoadPhase == PostLoadPhase::preparing || file.postLoadPhase == PostLoadPhase::uploading)
            && (!runtime.job || runtime.job->identity.fileId != file.objectId);
        if (orphaned || (postLoadReady(file) && file.mesh.preparationDeferred)) {
            setPostLoadPhase(state, file.objectId, PostLoadPhase::waitingForFrame);
        }
    }
    if (runtime.job) {
        if (!runtime.job->done.load(std::memory_order_acquire)) { return; }
        auto& job = *runtime.job;
        const auto found = std::find_if(state.files.begin(), state.files.end(), [&](const auto& file) { return file.objectId == job.identity.fileId; });
        const auto index = static_cast<size_t>(found - state.files.begin());
        if (index >= models.size()) { return; }
        try {
            if (!job.error.empty()) { throw std::runtime_error(job.error); }
            if (!job.result) { throw std::runtime_error("Model preparation was canceled."); }
            if (!runtime.upload) {
                const bool includeVertices = !graphics::isValid(models[index].gpuMesh.vertexBuffer) && !job.result->points.pointCloud;
                runtime.upload = beginGpuPointUpload(std::move(job.result->points), found->mesh, layout, includeVertices);
                setPostLoadPhase(state, job.identity.fileId, PostLoadPhase::uploading);
                return;
            }
            if (pollGpuMeshUpload(*runtime.upload)) {
                commitGpuPointUpload(models[index].gpuMesh, *runtime.upload);
                models[index].requestedFeatures = 0;
                publishPostLoadData(state, job.identity, std::move(*job.result));
                ++runtime.completions;
                runtime.upload.reset();
                runtime.job.reset();
            }
        } catch (const std::exception& error) {
            setPostLoadPhase(state, job.identity.fileId, PostLoadPhase::failed, error.what());
            retireJob(runtime);
        }
        return;
    }
    // One active model job bounds memory use. Its two tasks share the helper budget.
    // Retired jobs own their inputs and must finish before another model starts.
    if (!runtime.retired.empty()) { return; }
    for (const auto& frame : runtime.frames) {
        if (frame.token > completedFrame) { continue; }
        const auto found = std::find_if(state.files.begin(), state.files.end(), [&](const auto& file) {
            return file.objectId == frame.identity.fileId && file.postLoadPhase == PostLoadPhase::waitingForFrame;
        });
        if (found == state.files.end()) { continue; }
        auto job = std::make_unique<PostLoadJob>();
        job->identity = frame.identity;
        try {
            job->worker = std::jthread([task = job.get(), mesh = found->mesh](std::stop_token stop) {
                try { task->result = preparePostLoadData(mesh, [&] { return stop.stop_requested(); }); }
                catch (const std::exception& error) { task->error = error.what(); }
                catch (...) { task->error = "Model preparation failed."; }
                task->done.store(true, std::memory_order_release);
            });
            setPostLoadPhase(state, found->objectId, PostLoadPhase::preparing);
            runtime.job = std::move(job);
        } catch (const std::exception& error) { setPostLoadPhase(state, found->objectId, PostLoadPhase::failed, error.what()); }
        return;
    }
}

void cancelPostLoading(PostLoadRuntime& runtime)
{
    retireJob(runtime);
    runtime.retired.clear();
    runtime.frames.clear();
}

void updatePostLoadNotice(PostLoadRuntime& runtime, const UiState& state, double now)
{
    if (runtime.noticeOwner != &state || runtime.noticeGeneration != state.sceneGeneration) {
        runtime.noticeOwner = &state; runtime.noticeGeneration = state.sceneGeneration;
        runtime.noticePending = runtime.noticeReady = false;
        runtime.noticeCompletions = runtime.completions;
    }
    const bool pending = std::any_of(state.files.begin(), state.files.end(), [](const auto& file) {
        return !postLoadReady(file) && file.postLoadPhase != PostLoadPhase::failed;
    });
    if (pending) { runtime.noticeReady = false; }
    else if (runtime.noticePending && !state.files.empty() && scenePostLoadReady(state)
        && runtime.completions > runtime.noticeCompletions) {
        runtime.readyAt = now; runtime.noticeReady = true;
    }
    runtime.noticePending = pending;
    runtime.noticeCompletions = runtime.completions;
}

std::string_view postLoadNotice(const PostLoadRuntime& runtime, const UiState& state, double now)
{
    if (const auto error = postLoadError(state); !error.empty()) { return error; }
    if (runtime.noticePending) { return "Preparing model..."; }
    if (runtime.noticeReady && now - runtime.readyAt < 2.0) { return "Ready"; }
    return {};
}

std::string_view postLoadError(const UiState& state)
{
    for (const auto& file : state.files) {
        if (file.postLoadPhase == PostLoadPhase::failed) { return file.postLoadError; }
    }
    return {};
}
} // namespace woby
