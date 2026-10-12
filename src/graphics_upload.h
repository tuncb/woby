#pragma once

#include "graphics.h"
#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace woby::graphics {
// Native owners have no registry IDs. Only the main thread registers them.
struct Buffer {
    gpu::GpuHeap heap{};
    uint32_t bytes = 0;
    uint16_t stride = 0;
    bool index32 = false;
    bool index = false;
};

struct BufferUpload {
    std::vector<BufferUploadSource> sources;
    BufferUploadSourceFactory prepare;
    std::vector<std::shared_ptr<Buffer>> buffers;
    std::atomic<uint64_t> uploadedBytes = 0;
    std::atomic<bool> canceled = false, done = false;
    std::mutex completionMutex;
    std::condition_variable completionChanged;
    BufferUploadStats stats;
    std::string error;
    gpu::TimelinePoint completion{};
    std::shared_ptr<gpu::UploadQueue> completionOwner;
    uint64_t generation = 0;
    bool adopted = false;
};

struct BufferUploadRuntime {
    gpu::Device* device = nullptr;
    std::mutex* submissionMutex = nullptr;
    uint64_t capacity = 0, generation = 0;
    uint32_t queueIndex = 0;
    bool noop = false;
    std::shared_ptr<gpu::UploadQueue> uploader;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<std::shared_ptr<BufferUpload>> pending;
    std::vector<std::weak_ptr<BufferUpload>> jobs;
    std::shared_ptr<BufferUpload> active;
    std::jthread worker; // Last: stop/join before worker inputs are destroyed.
};

std::shared_ptr<Buffer> createNativeBuffer(gpu::Device* device, bool noop, uint32_t bytes, uint16_t stride, bool index32);
gpu::SubmitResult submitUploadCommands(void* context, gpu::Device* device, const gpu::SubmitDesc& submission, uint32_t queue) noexcept;
void startBufferUploadWorker(BufferUploadRuntime& runtime);
void stopBufferUploadWorker(BufferUploadRuntime& runtime);
void releaseBufferUploadOwners(BufferUploadRuntime& runtime);
std::shared_ptr<BufferUpload> enqueueBufferUpload(BufferUploadRuntime& runtime, std::vector<BufferUploadSource> sources);
std::shared_ptr<BufferUpload> enqueueBufferUpload(BufferUploadRuntime& runtime, BufferUploadSourceFactory prepare);
}
