#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string_view>

namespace {
struct AllocationProbe {
    int step = 0, failAt = 0, buffers = 0, memory = 0, mappings = 0;
    VkResult failure = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkResult result() { return ++step == failAt ? failure : VK_SUCCESS; }
} probe;
VKAPI_ATTR VkResult VKAPI_CALL createBuffer(VkDevice, const VkBufferCreateInfo*, const VkAllocationCallbacks*, VkBuffer* output) {
    const auto result = probe.result();
    if (result == VK_SUCCESS) { *output = reinterpret_cast<VkBuffer>(uintptr_t{1}); ++probe.buffers; }
    return result;
}
VKAPI_ATTR void VKAPI_CALL bufferRequirements(VkDevice, VkBuffer, VkMemoryRequirements* output) {
    *output = {4096, 16, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL allocateMemory(VkDevice, const VkMemoryAllocateInfo*, const VkAllocationCallbacks*, VkDeviceMemory* output) {
    const auto result = probe.result();
    if (result == VK_SUCCESS) { *output = reinterpret_cast<VkDeviceMemory>(uintptr_t{2}); ++probe.memory; }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL bindMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize) { return probe.result(); }
VKAPI_ATTR VkResult VKAPI_CALL mapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void** output) {
    const auto result = probe.result();
    if (result == VK_SUCCESS) { *output = reinterpret_cast<void*>(uintptr_t{4096}); ++probe.mappings; }
    return result;
}
VKAPI_ATTR void VKAPI_CALL unmapMemory(VkDevice, VkDeviceMemory) { --probe.mappings; }
VKAPI_ATTR void VKAPI_CALL destroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { --probe.buffers; }
VKAPI_ATTR void VKAPI_CALL freeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { --probe.memory; }
VKAPI_ATTR VkDeviceAddress VKAPI_CALL bufferAddress(VkDevice, const VkBufferDeviceAddressInfo*) { return 4096; }
VKAPI_ATTR VkResult VKAPI_CALL createSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*) { return probe.failure; }
VKAPI_ATTR VkResult VKAPI_CALL createCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*) { return probe.failure; }
VKAPI_ATTR VkResult VKAPI_CALL createComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*) { return probe.failure; }
VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo*, const VkAllocationCallbacks*, VkPipeline*) { return probe.failure; }
VKAPI_ATTR VkResult VKAPI_CALL createImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* output) {
    const auto result = probe.result();
    if (result == VK_SUCCESS) { *output = reinterpret_cast<VkImage>(uintptr_t{3}); ++probe.buffers; }
    return result;
}
VKAPI_ATTR VkResult VKAPI_CALL bindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) { return probe.result(); }
VKAPI_ATTR void VKAPI_CALL destroyImage(VkDevice, VkImage image, const VkAllocationCallbacks*) { if (image) --probe.buffers; }
VKAPI_ATTR VkResult VKAPI_CALL createImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*) { return probe.failure; }
}

// Compile the pinned, patched backend in isolation. No driver or large physical
// allocation is needed to exercise every step of its real allocation transaction.
#define vkCreateBuffer createBuffer
#define vkGetBufferMemoryRequirements bufferRequirements
#define vkAllocateMemory allocateMemory
#define vkBindBufferMemory bindMemory
#define vkMapMemory mapMemory
#define vkUnmapMemory unmapMemory
#define vkDestroyBuffer destroyBuffer
#define vkFreeMemory freeMemory
#define vkGetBufferDeviceAddress bufferAddress
#define vkCreateSemaphore createSemaphore
#define vkCreateCommandPool createCommandPool
#define vkCreateComputePipelines createComputePipelines
#define vkCreateGraphicsPipelines createGraphicsPipelines
#define vkCreateImage createImage
#define vkBindImageMemory bindImageMemory
#define vkDestroyImage destroyImage
#define vkCreateImageView createImageView
#include <NoGraphicsAPI.cpp>
#undef vkCreateBuffer
#undef vkGetBufferMemoryRequirements
#undef vkAllocateMemory
#undef vkBindBufferMemory
#undef vkMapMemory
#undef vkUnmapMemory
#undef vkDestroyBuffer
#undef vkFreeMemory
#undef vkGetBufferDeviceAddress
#undef vkCreateSemaphore
#undef vkCreateCommandPool
#undef vkCreateComputePipelines
#undef vkCreateGraphicsPipelines
#undef vkCreateImage
#undef vkBindImageMemory
#undef vkDestroyImage
#undef vkCreateImageView

TEST_CASE("Native buffer allocation reports failures and unwinds each completed step") {
    gpu::Device device{};
    device.memory_properties.memoryTypeCount = 1;
    device.memory_properties.memoryTypes[0] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0};
    device.memory_properties.memoryHeapCount = 1;
    device.memory_properties.memoryHeaps[0] = {1024 * 1024, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_MEMORY_MAP_FAILED, VK_ERROR_TOO_MANY_OBJECTS}) {
        for (int step = 1; step <= 4; ++step) {
            probe = {}; probe.failAt = step; probe.failure = failure;
            const auto heap = gpu::create_gpu_heap(&device, 4096, gpu::MemoryType::cpu_visible);
            CHECK(heap.owner == nullptr);
            CHECK(heap.range.cpu == nullptr);
            CHECK(std::string_view(heap.error) == gpu::allocation_error(failure));
            CHECK(probe.step == step);
            CHECK(probe.buffers == 0);
            CHECK(probe.memory == 0);
            CHECK(probe.mappings == 0);
            gpu::destroy_gpu_heap(heap);
        }
    }
    probe = {};
    const auto heap = gpu::create_gpu_heap(&device, 4096, gpu::MemoryType::cpu_visible);
    REQUIRE(heap.owner != nullptr);
    CHECK(probe.buffers == 1);
    CHECK(probe.memory == 1);
    CHECK(probe.mappings == 1);
    gpu::destroy_gpu_heap(heap);
    CHECK(probe.buffers == 0);
    CHECK(probe.memory == 0);
    CHECK(probe.mappings == 0);

    probe = {};
    device.memory_properties.memoryTypeCount = 0;
    const auto unavailable = gpu::create_gpu_heap(&device, 4096);
    CHECK(unavailable.owner == nullptr);
    CHECK(std::string_view(unavailable.error) == "No compatible GPU memory heap can hold this buffer.");
    CHECK(probe.buffers == 0);
}

TEST_CASE("Native texture heap allocation reports CPU and GPU exhaustion") {
    gpu::Device device{};
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        probe = {}; probe.failAt = 1; probe.failure = failure;
        const auto heap = gpu::create_texture_heap(&device, 4096);
        CHECK(heap.owner == nullptr);
        CHECK(std::string_view(heap.error) == gpu::allocation_error(failure));
        CHECK(probe.memory == 0);
        gpu::destroy_texture_heap(heap);
        CHECK(gpu::error_from_vk(failure) == gpu::Error::driver_error);
    }
}

TEST_CASE("Native resource creation returns failure instead of aborting") {
    gpu::Device device{};
    gpu::detail::Queue queue{};
    device.queue_count = 1; device.queues = &queue;
    // Device owns queues only after actual initialization; disarm that ownership
    // before leaving this stack-only test fixture.
    struct ResetQueues { gpu::Device& device; ~ResetQueues() { device.queues = nullptr; device.queue_count = 0; } } reset{device};
    const uint32_t spirv[] = {0x07230203u, 0, 0, 0, 0};
    const gpu::ShaderStage shader{{reinterpret_cast<const gpu::byte*>(spirv), sizeof(spirv)}, "main"};
    for (const auto failure : {VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY}) {
        probe = {}; probe.failure = failure;
        CHECK(gpu::create_timeline_semaphore(&device) == nullptr);
        CHECK(gpu::create_command_pool(&device) == nullptr);
        CHECK(gpu::create_compute_pso(&device, shader) == nullptr);
        CHECK(gpu::create_graphics_pso(&device, {.vertex = shader}) == nullptr);
        gpu::Texture texture{.state = &device};
        CHECK(gpu::create_render_view(&texture) == nullptr);
        gpu::CommandBuffer commands{.state = &device};
        gpu::TextureHeapOwner owner{.state = &device};
        for (int step = 1; step <= 2; ++step) {
            probe.step = 0; probe.failAt = step;
            CHECK(gpu::create_texture(&commands, {.extent = {1, 1, 1}}, {.size = 4096, .owner = &owner}, 0) == nullptr);
            CHECK(probe.buffers == 0);
        }
    }
}
