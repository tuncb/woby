# Checked Vulkan geometry allocations. Keep the dependency's other entry points
# unchanged; callers of this path own both the error and any successful heap.
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[==[void destroy_gpu_heap(const GpuHeap& heap) noexcept;]==]
[==[void destroy_gpu_heap(const GpuHeap& heap) noexcept;

#if !defined(__APPLE__)
struct HeapAllocationFailure
{
    const char* operation = nullptr;
    int32 api_result = 0;
    uint64 allocation_bytes = 0;
    uint32 memory_type = ~uint32{0};
};
[[nodiscard]] GpuHeap try_create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory,
                                         HeapAllocationFailure& failure) noexcept;
#if defined(WOBY_GPU_ALLOCATION_TESTS)
// One-shot fault injection, confined to builds with native GPU tests enabled.
void fail_heap_allocation_for_test(const char* operation, int32 result, uint32 skip = 0) noexcept;
#endif
#endif]==])

replace_exact(src/NoGraphicsAPI.cpp "#include <stdlib.h>" "#include <stdlib.h>\n#include <new>")
replace_exact(src/NoGraphicsAPI.cpp
[==[    void create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0) const noexcept]==]
[==[    void create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0,
                               HeapAllocationFailure* failure = nullptr) const noexcept]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        detail::BackingBuffer result{};
        const VkBufferCreateInfo buffer_info{]==]
[==[        detail::BackingBuffer result{};
        if (failure) *failure = {.allocation_bytes = size};
        const auto checked = [&](VkResult status, const char* operation) {
            if (status == VK_SUCCESS) return true;
            if (!failure) require_vk(status);
            failure->operation = operation;
            failure->api_result = static_cast<int32>(status);
            // Destroy the bound buffer before freeing its memory. An allocation
            // failure can occur at any step, including after a successful bind.
            if (result.mapped) vkUnmapMemory(device, result.memory);
            if (result.buffer) vkDestroyBuffer(device, result.buffer, nullptr);
            if (result.memory) vkFreeMemory(device, result.memory, nullptr);
            return false;
        };
        const VkBufferCreateInfo buffer_info{]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));]==]
[==[        // Vulkan leaves output parameters undefined on error. Publish only
        // successful outputs to result, whose members the cleanup path owns.
        VkBuffer buffer{};
        if (!checked(WOBY_HEAP_CALL("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, nullptr, &buffer),
            memset(&buffer, 0xcd, sizeof(buffer))), "vkCreateBuffer")) return;
        result.buffer = buffer;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        assert(has_memory_type);
        if (!has_memory_type)
            abort();]==]
[==[        if (!checked(has_memory_type ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT, "find_memory_type")) return;
        if (failure) {
            failure->allocation_bytes = requirements.size;
            failure->memory_type = memory_type;
        }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        require_vk(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory));
        require_vk(vkBindBufferMemory(device, result.buffer, result.memory, 0));]==]
[==[        VkDeviceMemory allocation{};
        if (!checked(WOBY_HEAP_CALL("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, nullptr, &allocation),
            memset(&allocation, 0xcd, sizeof(allocation))), "vkAllocateMemory")) return;
        result.memory = allocation;
        if (!checked(WOBY_HEAP_CALL("vkBindBufferMemory", vkBindBufferMemory(device, result.buffer, result.memory, 0),
            (void)0), "vkBindBufferMemory")) return;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[            require_vk(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));]==]
[==[            void* mapped = nullptr;
            if (!checked(WOBY_HEAP_CALL("vkMapMemory", vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                memset(&mapped, 0xcd, sizeof(mapped))), "vkMapMemory")) return;
            result.mapped = mapped;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    [[nodiscard]] GpuHeap allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept;]==]
[==[    [[nodiscard]] GpuHeap allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure = nullptr) noexcept;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept]==]
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure) noexcept]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    GpuHeapOwner* heap = new GpuHeapOwner{.state = this};
    create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided);]==]
[==[    GpuHeapOwner* heap = new (std::nothrow) GpuHeapOwner{.state = this};
    if (!heap) {
        if (!failure) abort_vk_failure(VK_ERROR_OUT_OF_HOST_MEMORY);
        *failure = {.operation = "GpuHeapOwner", .api_result = VK_ERROR_OUT_OF_HOST_MEMORY, .allocation_bytes = size};
        return {};
    }
    create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided, failure);
    if (failure && failure->operation) {
        delete heap;
        return {};
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[void destroy_gpu_heap(const GpuHeap& heap) noexcept]==]
[==[GpuHeap try_create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory, HeapAllocationFailure& failure) noexcept
{
    assert(device);
    failure = {};
    return device->allocate_gpu_heap(byte_count, memory, &failure);
}

void destroy_gpu_heap(const GpuHeap& heap) noexcept]==])

# Inject before a Vulkan call, so failed calls create no resource. The same
# cleanup path then handles real driver errors and deterministic regressions.
# Poison unsuccessful outputs to verify they are never destroyed or unmapped.
replace_exact(src/NoGraphicsAPI.cpp
[==[namespace gpu
{

namespace
{]==]
[==[namespace gpu
{

#if defined(WOBY_GPU_ALLOCATION_TESTS)
namespace {
thread_local const char* heap_failure_operation = nullptr;
thread_local int32 heap_failure_result = 0;
thread_local uint32 heap_failure_skip = 0;
VkResult injected_heap_result(const char* operation) noexcept
{
    if (!heap_failure_operation || strcmp(operation, heap_failure_operation) != 0) return VK_SUCCESS;
    if (heap_failure_skip) { --heap_failure_skip; return VK_SUCCESS; }
    heap_failure_operation = nullptr;
    return static_cast<VkResult>(heap_failure_result);
}
}
void fail_heap_allocation_for_test(const char* operation, int32 result, uint32 skip) noexcept
{
    heap_failure_operation = operation;
    heap_failure_result = result;
    heap_failure_skip = skip;
}
#define WOBY_HEAP_CALL(name, call, poison) ([&] { const auto injected = injected_heap_result(name); if (injected != VK_SUCCESS) { poison; return injected; } return (call); }())
#else
#define WOBY_HEAP_CALL(name, call, poison) (call)
#endif

namespace
{]==])
