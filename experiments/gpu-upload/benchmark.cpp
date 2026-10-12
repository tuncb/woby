#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint64_t MiB = 1024 * 1024;
void check(const char* error) { if (error) { throw std::runtime_error(error); } }
struct DeviceOwner {
    gpu::Device* value = nullptr;
    ~DeviceOwner() { if (value) { gpu::destroy_device(value); } }
};
struct HeapOwner {
    gpu::GpuHeap value;
    ~HeapOwner() { gpu::destroy_gpu_heap(value); }
};
struct ReadbackCommands {
    gpu::CommandPool* pool = nullptr;
    gpu::TimelineSemaphore* semaphore = nullptr;
    bool submitted = false;
    ~ReadbackCommands() {
        if (submitted) { gpu::wait_timeline({semaphore, 1}); }
        if (pool) { gpu::destroy_command_pool(pool); }
        if (semaphore) { gpu::destroy_timeline_semaphore(semaphore); }
    }
};
double milliseconds(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }
void verify(gpu::Device* device, const HeapOwner& target, const std::vector<uint32_t>& source, gpu::TimelinePoint uploaded) {
    constexpr uint64_t sampleBytes = 4096;
    const uint64_t bytes = source.size() * sizeof(uint32_t);
    const std::array<uint64_t, 5> offsets{0, 4 * MiB, bytes / 2, bytes - 4 * MiB, bytes - sampleBytes};
    HeapOwner result;
    result.value = gpu::create_gpu_heap(device, offsets.size() * sampleBytes, gpu::MemoryType::readback);
    if (!result.value.owner) { check(result.value.error); }
    ReadbackCommands commands;
    commands.pool = gpu::create_command_pool(device);
    commands.semaphore = gpu::create_timeline_semaphore(device);
    if (!commands.pool || !commands.semaphore) { throw std::runtime_error("Cannot allocate readback commands"); }
    auto* copy = gpu::begin_commands(commands.pool);
    if (!copy) { check(gpu::command_pool_error(commands.pool)); }
    for (size_t i = 0; i < offsets.size(); ++i) {
        gpu::copy_memory(copy, {target.value.range.gpu + offsets[i], sampleBytes}, {result.value.range.gpu + i * sampleBytes, sampleBytes});
    }
    gpu::barrier(copy, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    check(gpu::end_commands(copy));
    gpu::CommandBuffer* const copies[]{copy};
    const gpu::TimelinePoint waits[]{uploaded};
    const auto submitted = gpu::submit(device, {.commands = copies, .waits = waits, .completion = {commands.semaphore, 1}});
    commands.submitted = submitted.submitted;
    check(submitted.error);
    gpu::wait_timeline({commands.semaphore, 1});
    for (size_t i = 0; i < offsets.size(); ++i) {
        if (std::memcmp(result.value.range.cpu + i * sampleBytes,
            reinterpret_cast<const gpu::byte*>(source.data()) + offsets[i], sampleBytes) != 0) {
            throw std::runtime_error("Upload sample differs from source");
        }
    }
}
}

int main() {
    try {
        std::cout << std::unitbuf;
        DeviceOwner device;
        gpu::DeviceDesc options;
        options.desired_queue_count = 2;
        options.timestamp_query_count = 0;
        device.value = gpu::create_device(options).device;
        if (!device.value) { throw std::runtime_error("Cannot create GPU device"); }
        const auto& caps = gpu::get_device_caps(device.value);
        const auto queueIndex = caps.general_queue_count > 1 ? 1u : 0u;
        constexpr uint64_t payload = 512 * MiB;
        std::vector<uint32_t> source(static_cast<size_t>(payload / sizeof(uint32_t)));
        for (size_t i = 0; i < source.size(); ++i) { source[i] = static_cast<uint32_t>(i) ^ 0xa193b2c5u; }
        HeapOwner target;
        target.value = gpu::create_gpu_heap(device.value, payload, gpu::MemoryType::gpu_only);
        if (!target.value.owner) { check(target.value.error); }
        std::cerr << caps.device_name << "; queues=" << caps.general_queue_count << "; upload_queue=" << queueIndex << '\n';
        std::cout << "round,mode,capacity_mib,payload_mib,chunk_mib,allocation_ms,upload_ms,submissions,waits,peak_mib,verified\n";
        const std::array<uint64_t, 7> capacities{4, 8, 16, 32, 64, 128, 256};
        for (size_t round = 0; round < 6; ++round) {
            for (size_t ordinal = 0; ordinal < capacities.size() * 2; ++ordinal) {
                const bool overlap = ordinal % 2 != 0;
                const size_t position = ordinal / 2;
                const auto index = round % 2 ? capacities.size() - 1 - position : position;
                const uint64_t capacity = capacities[(index + round) % capacities.size()] * MiB;
                const auto allocated = Clock::now();
                const auto created = gpu::create_utility<gpu::UploadQueue>(device.value, capacity, queueIndex, 2u);
                std::unique_ptr<gpu::UploadQueue> uploader(created.value);
                if (!uploader) { check(created.error); }
                const auto allocationMs = milliseconds(allocated);
                const auto start = Clock::now();
                const auto chunk = overlap ? std::min(4 * MiB, capacity / 2) : 4 * MiB;
                uint64_t batchBytes = 0;
                for (uint64_t offset = 0; offset < payload; offset += chunk) {
                    uploader->upload_buffer({target.value.range.gpu + offset, chunk},
                        {reinterpret_cast<const gpu::byte*>(source.data()) + offset, chunk});
                    check(uploader->error);
                    batchBytes += chunk;
                    if (overlap && batchBytes >= capacity / 2) { uploader->flush(); batchBytes = 0; }
                }
                const auto uploaded = uploader->flush();
                uploader->wait();
                check(uploader->error);
                const auto uploadMs = milliseconds(start);
                const auto stats = uploader->stats();
                verify(device.value, target, source, uploaded);
                std::cout << round << ',' << (overlap ? "overlap" : "burst") << ',' << capacity / MiB << ',' << payload / MiB << ',' << chunk / MiB << ',' << allocationMs << ','
                    << uploadMs << ',' << stats.submissions << ',' << stats.waits << ',' << stats.peak_bytes / MiB << ",true\n";
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
