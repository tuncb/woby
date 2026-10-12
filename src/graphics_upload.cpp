#include "graphics_upload.h"
#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#if !defined(__APPLE__)
#include <vulkan/vulkan.h>
#endif

namespace woby::graphics {
namespace {
void check(const char* error) { if (error) { throw std::runtime_error(error); } }
void validate(const std::vector<BufferUploadSource>& sources) {
    for (const auto& source : sources) {
        if (!source.owner || !source.data || !source.bytes || !source.stride || source.bytes % source.stride || source.bytes % 4) {
            throw std::invalid_argument("Invalid owned GPU upload source.");
        }
    }
}
void complete(BufferUpload& job) {
    {
        const std::lock_guard lock(job.completionMutex);
        job.done.store(true, std::memory_order_release);
    }
    job.completionChanged.notify_all();
}
void execute(BufferUploadRuntime& runtime, BufferUpload& job) {
    const auto start = std::chrono::steady_clock::now();
    try {
        if (job.prepare && !job.canceled.load(std::memory_order_acquire)) {
            job.sources = job.prepare(job.canceled);
            validate(job.sources);
        }
        if (!runtime.noop && !job.canceled.load(std::memory_order_acquire) && !job.sources.empty() && !runtime.uploader) {
            const auto created = gpu::create_utility<gpu::UploadQueue>(runtime.device, runtime.capacity, runtime.queueIndex, 2u);
            runtime.uploader.reset(created.value);
            if (!runtime.uploader) { check(created.error); }
            runtime.uploader->submit_context = runtime.submissionMutex;
            runtime.uploader->submit_function = submitUploadCommands;
        }
        const auto before = runtime.uploader ? runtime.uploader->stats() : gpu::UploadQueueStats{};
        const auto batchLimit = runtime.capacity / 2;
        uint64_t batchBytes = 0;
        job.buffers.reserve(job.sources.size());
        for (const auto& source : job.sources) {
            if (job.canceled.load(std::memory_order_acquire)) { break; }
            auto buffer = createNativeBuffer(runtime.device, runtime.noop, source.bytes, source.stride, source.index32);
            buffer->index = source.index;
            job.buffers.push_back(buffer);
            for (uint32_t offset = 0; offset < source.bytes;) {
                if (job.canceled.load(std::memory_order_acquire)) { break; }
                const auto chunk = static_cast<uint32_t>(std::min<uint64_t>(4u * 1024 * 1024, batchLimit));
                const auto count = std::min(source.bytes - offset, chunk);
                if (!runtime.noop) {
                    runtime.uploader->upload_buffer({buffer->heap.range.gpu + offset, count},
                        {static_cast<const gpu::byte*>(source.data) + offset, count});
                    check(runtime.uploader->error);
                    batchBytes += count;
                    if (batchBytes >= batchLimit) { runtime.uploader->flush(); check(runtime.uploader->error); batchBytes = 0; }
                }
                offset += count;
                job.uploadedBytes.fetch_add(count, std::memory_order_relaxed);
            }
        }
        if (runtime.uploader) {
            job.completion = runtime.uploader->flush();
            runtime.uploader->wait();
            check(runtime.uploader->error);
            job.completionOwner = runtime.uploader;
            const auto after = runtime.uploader->stats();
            job.stats.submissions = after.submissions - before.submissions;
            job.stats.waits = after.waits - before.waits;
            job.stats.stagingBytes = after.capacity;
            job.stats.peakStagingBytes = after.peak_bytes;
        }
    } catch (const std::exception& error) {
        // A failed/canceled job still owns every destination until submitted copies finish.
        if (runtime.uploader) { runtime.uploader->wait(); }
        if (runtime.uploader && runtime.uploader->error) { runtime.uploader.reset(); }
        job.error = error.what();
    } catch (...) {
        if (runtime.uploader) { runtime.uploader->wait(); }
        if (runtime.uploader && runtime.uploader->error) { runtime.uploader.reset(); }
        job.error = "GPU upload failed.";
    }
    job.stats.uploadedBytes = job.uploadedBytes.load(std::memory_order_relaxed);
    job.stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    job.sources.clear(); // Release CPU snapshot owners on the worker.
    job.prepare = {};
    if (job.canceled.load(std::memory_order_acquire) || !job.error.empty()) {
        job.buffers.clear(); job.completion = {}; job.completionOwner.reset();
    }
    complete(job);
}
}

std::shared_ptr<Buffer> createNativeBuffer(gpu::Device* device, bool noop, uint32_t bytes, uint16_t stride, bool index32) {
    if (!bytes) { throw std::runtime_error("Cannot create an empty GPU buffer"); }
    if (!stride || bytes % stride) { throw std::runtime_error("GPU buffer size must match its element stride"); }
    auto result = std::shared_ptr<Buffer>(new Buffer, [](Buffer* value) {
        gpu::destroy_gpu_heap(value->heap);
        delete value;
    });
    result->bytes = bytes; result->stride = stride; result->index32 = index32;
    if (!noop) {
        const auto heapBytes = (uint64_t(bytes) + 15) & ~uint64_t{15};
#if defined(__APPLE__)
        result->heap = gpu::create_gpu_heap(device, heapBytes, gpu::MemoryType::gpu_only);
#else
        gpu::HeapAllocationFailure failure;
        result->heap = gpu::try_create_gpu_heap(device, heapBytes, gpu::MemoryType::gpu_only, failure);
        if (!result->heap.owner) {
            const char* reason = failure.api_result == VK_ERROR_OUT_OF_DEVICE_MEMORY ? " (out of device memory)"
                : failure.api_result == VK_ERROR_OUT_OF_HOST_MEMORY ? " (out of host memory)"
                : failure.api_result == VK_ERROR_DEVICE_LOST ? " (device lost)" : "";
            throw std::runtime_error("Cannot allocate GPU geometry: requested " + std::to_string(bytes)
                + " bytes; " + (failure.operation ? failure.operation : "unknown operation")
                + " returned VkResult " + std::to_string(failure.api_result) + reason
                + " (allocation " + std::to_string(failure.allocation_bytes)
                + " bytes, memory type " + std::to_string(failure.memory_type) + ").");
        }
#endif
        if (!result->heap.owner) { throw std::runtime_error("Cannot allocate GPU geometry"); }
    }
    return result;
}

gpu::SubmitResult submitUploadCommands(void* context, gpu::Device* device, const gpu::SubmitDesc& submission, uint32_t queue) noexcept {
    const std::lock_guard lock(*static_cast<std::mutex*>(context));
    return gpu::submit(device, submission, queue);
}

void startBufferUploadWorker(BufferUploadRuntime& runtime) {
    runtime.worker = std::jthread([&runtime](std::stop_token stop) {
        while (true) {
            std::shared_ptr<BufferUpload> job;
            {
                std::unique_lock lock(runtime.mutex);
                runtime.changed.wait(lock, stop, [&] { return !runtime.pending.empty(); });
                if (runtime.pending.empty()) { break; }
                job = std::move(runtime.pending.front()); runtime.pending.pop_front();
                runtime.active = job;
                if (stop.stop_requested()) { job->canceled.store(true, std::memory_order_release); }
            }
            execute(runtime, *job);
            const std::lock_guard lock(runtime.mutex);
            runtime.active.reset();
        }
    });
}

void stopBufferUploadWorker(BufferUploadRuntime& runtime) {
    {
        const std::lock_guard lock(runtime.mutex);
        if (runtime.active) { cancelBufferUpload(runtime.active); }
        for (const auto& job : runtime.pending) { cancelBufferUpload(job); }
        runtime.worker.request_stop();
    }
    runtime.changed.notify_all();
    if (runtime.worker.joinable()) { runtime.worker.join(); }
}

void releaseBufferUploadOwners(BufferUploadRuntime& runtime) {
    for (const auto& weak : runtime.jobs) {
        if (const auto job = weak.lock()) { job->buffers.clear(); job->completion = {}; job->completionOwner.reset(); }
    }
    runtime.jobs.clear();
}

std::shared_ptr<BufferUpload> enqueueBufferUpload(BufferUploadRuntime& runtime, std::vector<BufferUploadSource> sources) {
    validate(sources);
    auto job = std::make_shared<BufferUpload>();
    job->sources = std::move(sources); job->generation = runtime.generation;
    {
        const std::lock_guard lock(runtime.mutex);
        if (runtime.worker.get_stop_token().stop_requested()) { throw std::runtime_error("GPU uploader is stopping."); }
        std::erase_if(runtime.jobs, [](const auto& weak) { return weak.expired(); });
        runtime.jobs.push_back(job);
        runtime.pending.push_back(job);
    }
    runtime.changed.notify_one();
    return job;
}
std::shared_ptr<BufferUpload> enqueueBufferUpload(BufferUploadRuntime& runtime, BufferUploadSourceFactory prepare) {
    if (!prepare) { throw std::invalid_argument("Missing GPU upload preparation."); }
    auto job = std::make_shared<BufferUpload>();
    job->prepare = std::move(prepare); job->generation = runtime.generation;
    {
        const std::lock_guard lock(runtime.mutex);
        if (runtime.worker.get_stop_token().stop_requested()) { throw std::runtime_error("GPU uploader is stopping."); }
        std::erase_if(runtime.jobs, [](const auto& weak) { return weak.expired(); });
        runtime.jobs.push_back(job); runtime.pending.push_back(job);
    }
    runtime.changed.notify_one();
    return job;
}

bool bufferUploadComplete(const std::shared_ptr<BufferUpload>& job) { return job && job->done.load(std::memory_order_acquire); }
void cancelBufferUpload(const std::shared_ptr<BufferUpload>& job) {
    if (job) { job->canceled.store(true, std::memory_order_release); job->canceled.notify_all(); }
}
void waitBufferUpload(const std::shared_ptr<BufferUpload>& job) {
    if (!job) { throw std::invalid_argument("Missing GPU upload."); }
    std::unique_lock lock(job->completionMutex);
    job->completionChanged.wait(lock, [&] { return job->done.load(std::memory_order_acquire); });
}
BufferUploadStats bufferUploadStats(const std::shared_ptr<BufferUpload>& job) {
    if (!job) { return {}; }
    if (bufferUploadComplete(job)) { return job->stats; }
    BufferUploadStats result; result.uploadedBytes = job->uploadedBytes.load(std::memory_order_relaxed); return result;
}
}
