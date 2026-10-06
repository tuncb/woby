// Compile the actual patched utility sources with exceptions and RTTI disabled.
// Only the native driver calls and allocation-failure decisions are simulated.
#include "utility_allocation_probe.h"
#include <NoGraphicsAPIUtility/utility_allocation.hpp>

#if defined(__cpp_exceptions) || defined(_CPPUNWIND) || defined(__cpp_rtti) || defined(_CPPRTTI)
#error Graphics utility fixture must compile without exceptions or RTTI
#endif

UtilityAllocationProbe utilityProbe;

namespace gpu
{
struct Device {};
struct GpuHeapOwner {};
struct TextureHeapOwner {};
struct TimelineSemaphore {};
struct CommandPool {};
struct CommandBuffer {};
struct Texture {};

namespace
{
Device testDevice;
GpuHeapOwner heapOwner;
TextureHeapOwner textureHeapOwner;
TimelineSemaphore timeline;
CommandPool pool;
CommandBuffer commands;
Texture texture;
alignas(16) byte storage[256]{};
}

namespace detail
{
bool fail_utility_allocation() noexcept { return ++utilityProbe.step == utilityProbe.failAt; }
void utility_array_created() noexcept { ++utilityProbe.arrays; }
void utility_array_destroyed() noexcept { --utilityProbe.arrays; }
}

const DeviceCaps& get_device_caps(const Device*) noexcept
{
    static const DeviceCaps caps = [] {
        DeviceCaps result{};
        result.queue_count = 1;
        result.general_queue_count = 1;
        result.texture_heap_alignment = 16;
        return result;
    }();
    return caps;
}
GpuHeap create_gpu_heap(Device*, uint64 bytes, MemoryType) noexcept
{
    if (detail::fail_utility_allocation()) return {.error = "Injected GPU heap failure."};
    ++utilityProbe.heaps;
    return {.range = {storage, storage, bytes}, .owner = &heapOwner};
}
void destroy_gpu_heap(const GpuHeap& heap) noexcept { if (heap.owner) --utilityProbe.heaps; }
TimelineSemaphore* create_timeline_semaphore(Device*, uint64) noexcept
{
    if (detail::fail_utility_allocation()) return nullptr;
    ++utilityProbe.timelines;
    return &timeline;
}
void destroy_timeline_semaphore(TimelineSemaphore* value) noexcept { if (value) --utilityProbe.timelines; }
CommandPool* create_command_pool(Device*, uint32) noexcept
{
    if (detail::fail_utility_allocation()) return nullptr;
    ++utilityProbe.pools;
    return &pool;
}
void destroy_command_pool(CommandPool* value) noexcept { if (value) --utilityProbe.pools; }
const char* command_pool_error(const CommandPool*) noexcept { return "Injected command failure."; }
const char* reset_command_pool(CommandPool*) noexcept
{
    return ++utilityProbe.commandStep == utilityProbe.failCommandAt ? "Injected reset failure." : nullptr;
}
CommandBuffer* begin_commands(CommandPool*) noexcept
{
    return ++utilityProbe.commandStep == utilityProbe.failCommandAt ? nullptr : &commands;
}
const char* end_commands(CommandBuffer*) noexcept
{
    return ++utilityProbe.commandStep == utilityProbe.failCommandAt ? "Injected recording failure." : nullptr;
}
SizeAlign get_texture_size_align(Device*, const TextureDesc& desc) noexcept { return {uint64{desc.extent.x} * 16, 16}; }
Texture* create_texture(CommandBuffer*, const TextureDesc&, const TextureHeap&, uint64 offset) noexcept
{
    ++utilityProbe.textureAttempts;
    utilityProbe.lastTextureOffset = offset;
    if (detail::fail_utility_allocation()) return nullptr;
    ++utilityProbe.textures;
    return &texture;
}
void destroy_texture(Texture* value) noexcept { if (value) --utilityProbe.textures; }
uint64 timeline_completed_value(const TimelineSemaphore*) noexcept { return ~uint64{0}; }
void wait_timeline(TimelinePoint point) noexcept {
    utilityProbe.waitedValue = point.value;
    if (point.value > utilityProbe.submittedValue) ++utilityProbe.unexpectedCalls;
}
void read_timestamps(CommandPool*) noexcept {}
void barrier(CommandBuffer*, Stage, Access, Stage, Access) noexcept {}
SubmitResult submit(Device*, const SubmitDesc& desc, uint32) noexcept {
    if (++utilityProbe.commandStep == utilityProbe.failCommandAt) return {"Injected submission failure."};
    utilityProbe.submittedValue = desc.completion.value;
    ++utilityProbe.submissions;
    return {nullptr, true};
}
void copy_memory(CommandBuffer* value, GpuRange, GpuRange) noexcept {
    if (!value) ++utilityProbe.unexpectedCalls;
    ++utilityProbe.copies;
}
void copy_memory_to_texture(CommandBuffer* value, GpuRange, Texture*, const TextureCopyDesc&) noexcept {
    if (!value) ++utilityProbe.unexpectedCalls;
    ++utilityProbe.copies;
}
void write_timestamp(CommandBuffer*, uint64*, Stage) noexcept { ++utilityProbe.unexpectedCalls; }
} // namespace gpu

gpu::Device* utilityTestDevice() noexcept { return &gpu::testDevice; }
gpu::TextureHeap utilityTestTextureHeap() noexcept { return {.size = 64, .owner = &gpu::textureHeapOwner}; }
gpu::TimelineSemaphore* utilityTestTimeline() noexcept { return &gpu::timeline; }

#include <heap_allocator.cpp>
#include <texture_allocator.cpp>
#include <delete_queue.cpp>
#include <upload_queue.cpp>
#include <texture_upload.cpp>
