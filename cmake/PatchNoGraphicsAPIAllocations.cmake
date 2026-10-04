# Resource creation must return failure to the application, not abort on OOM.
# Keep the dependency's noexcept ABI: error strings are static and allocation-free.
set(header include/NoGraphicsAPI/NoGraphicsAPI.hpp)
set(source src/NoGraphicsAPI.cpp)
replace_exact(${header} "    const GpuHeapOwner* owner = nullptr;"
    "    const GpuHeapOwner* owner = nullptr;\n    const char* error = \"GPU buffer allocation failed.\";")
replace_exact(${header} "    const TextureHeapOwner* owner = nullptr;"
    "    const TextureHeapOwner* owner = nullptr;\n    const char* error = \"GPU texture memory allocation failed.\";")
replace_exact(${source} "#include <stdlib.h>" "#include <stdlib.h>\n#include <new>")
replace_exact(${source} "case VK_ERROR_TOO_MANY_OBJECTS: abort();"
    "case VK_ERROR_TOO_MANY_OBJECTS: return Error::driver_error;")
replace_exact(${source} "[[noreturn]] void abort_vk_failure" [=[
// Static messages also work while the driver is out of host memory.
const char* allocation_error(VkResult result) noexcept
{
    switch (result) {
    case VK_SUCCESS: return nullptr;
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "Insufficient CPU memory for a Vulkan resource.";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "Insufficient GPU memory for a Vulkan resource.";
    case VK_ERROR_TOO_MANY_OBJECTS: return "Vulkan resource allocation count exhausted.";
    case VK_ERROR_DEVICE_LOST: return "GPU device lost while allocating a resource.";
    case VK_ERROR_MEMORY_MAP_FAILED: return "Cannot map GPU memory into the CPU address space.";
    default: return "Vulkan resource allocation failed (driver error).";
    }
}

[[noreturn]] void abort_vk_failure]=])
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
# Accept an already-patched #92 dependency tree as well as fresh source.
replace_exact(src/NoGraphicsAPI.cpp
[==[    void create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0) const noexcept]==]
[==[    void create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0,
                               HeapAllocationFailure* failure = nullptr) const noexcept]==]
[==[    const char* create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0,
                               HeapAllocationFailure* failure = nullptr) const noexcept]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    void create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0,
                               HeapAllocationFailure* failure = nullptr) const noexcept]==]
[==[    const char* create_backing_buffer(detail::BackingBuffer& output, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
                               VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0,
                               HeapAllocationFailure* failure = nullptr) const noexcept]==])
# Accept an already-patched #92 dependency tree as well as fresh source.
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
        const VkBufferCreateInfo buffer_info{]==]
[==[        detail::BackingBuffer result{};
        HeapAllocationFailure ignored;
        if (!failure) failure = &ignored;
        *failure = {.allocation_bytes = size};
        const char* reason = nullptr;
        const auto checked = [&](VkResult status, const char* operation) {
            if (status == VK_SUCCESS) return true;
            reason = strcmp(operation, "find_memory_type") == 0
                ? "No compatible GPU memory heap can hold this buffer." : allocation_error(status);
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
        const VkBufferCreateInfo buffer_info{]==]
[==[        detail::BackingBuffer result{};
        HeapAllocationFailure ignored;
        if (!failure) failure = &ignored;
        *failure = {.allocation_bytes = size};
        const char* reason = nullptr;
        const auto checked = [&](VkResult status, const char* operation) {
            if (status == VK_SUCCESS) return true;
            reason = strcmp(operation, "find_memory_type") == 0
                ? "No compatible GPU memory heap can hold this buffer." : allocation_error(status);
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
# Accept an already-patched #92 dependency tree as well as fresh source.
replace_exact(src/NoGraphicsAPI.cpp
[==[        require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));]==]
[==[        // Vulkan leaves output parameters undefined on error. Publish only
        // successful outputs to result, whose members the cleanup path owns.
        VkBuffer buffer{};
        if (!checked(WOBY_HEAP_CALL("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, nullptr, &buffer),
            memset(&buffer, 0xcd, sizeof(buffer))), "vkCreateBuffer")) return;
        result.buffer = buffer;]==]
[==[        // Vulkan leaves output parameters undefined on error. Publish only
        // successful outputs to result, whose members the cleanup path owns.
        VkBuffer buffer{};
        if (!checked(WOBY_HEAP_CALL("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, nullptr, &buffer),
            memset(&buffer, 0xcd, sizeof(buffer))), "vkCreateBuffer")) return reason;
        result.buffer = buffer;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        // Vulkan leaves output parameters undefined on error. Publish only
        // successful outputs to result, whose members the cleanup path owns.
        VkBuffer buffer{};
        if (!checked(WOBY_HEAP_CALL("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, nullptr, &buffer),
            memset(&buffer, 0xcd, sizeof(buffer))), "vkCreateBuffer")) return;
        result.buffer = buffer;]==]
[==[        // Vulkan leaves output parameters undefined on error. Publish only
        // successful outputs to result, whose members the cleanup path owns.
        VkBuffer buffer{};
        if (!checked(WOBY_HEAP_CALL("vkCreateBuffer", vkCreateBuffer(device, &buffer_info, nullptr, &buffer),
            memset(&buffer, 0xcd, sizeof(buffer))), "vkCreateBuffer")) return reason;
        result.buffer = buffer;]==])
# Accept an already-patched #92 dependency tree as well as fresh source.
replace_exact(src/NoGraphicsAPI.cpp
[==[        assert(has_memory_type);
        if (!has_memory_type)
            abort();]==]
[==[        if (!checked(has_memory_type ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT, "find_memory_type")) return;
        if (failure) {
            failure->allocation_bytes = requirements.size;
            failure->memory_type = memory_type;
        }]==]
[==[        if (!checked(has_memory_type ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT, "find_memory_type")) return reason;
        if (failure) {
            failure->allocation_bytes = requirements.size;
            failure->memory_type = memory_type;
        }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        if (!checked(has_memory_type ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT, "find_memory_type")) return;
        if (failure) {
            failure->allocation_bytes = requirements.size;
            failure->memory_type = memory_type;
        }]==]
[==[        if (!checked(has_memory_type ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT, "find_memory_type")) return reason;
        if (failure) {
            failure->allocation_bytes = requirements.size;
            failure->memory_type = memory_type;
        }]==])
# Accept an already-patched #92 dependency tree as well as fresh source.
replace_exact(src/NoGraphicsAPI.cpp
[==[        require_vk(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory));
        require_vk(vkBindBufferMemory(device, result.buffer, result.memory, 0));]==]
[==[        VkDeviceMemory allocation{};
        if (!checked(WOBY_HEAP_CALL("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, nullptr, &allocation),
            memset(&allocation, 0xcd, sizeof(allocation))), "vkAllocateMemory")) return;
        result.memory = allocation;
        if (!checked(WOBY_HEAP_CALL("vkBindBufferMemory", vkBindBufferMemory(device, result.buffer, result.memory, 0),
            (void)0), "vkBindBufferMemory")) return;]==]
[==[        VkDeviceMemory allocation{};
        if (!checked(WOBY_HEAP_CALL("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, nullptr, &allocation),
            memset(&allocation, 0xcd, sizeof(allocation))), "vkAllocateMemory")) return reason;
        result.memory = allocation;
        if (!checked(WOBY_HEAP_CALL("vkBindBufferMemory", vkBindBufferMemory(device, result.buffer, result.memory, 0),
            (void)0), "vkBindBufferMemory")) return reason;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        VkDeviceMemory allocation{};
        if (!checked(WOBY_HEAP_CALL("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, nullptr, &allocation),
            memset(&allocation, 0xcd, sizeof(allocation))), "vkAllocateMemory")) return;
        result.memory = allocation;
        if (!checked(WOBY_HEAP_CALL("vkBindBufferMemory", vkBindBufferMemory(device, result.buffer, result.memory, 0),
            (void)0), "vkBindBufferMemory")) return;]==]
[==[        VkDeviceMemory allocation{};
        if (!checked(WOBY_HEAP_CALL("vkAllocateMemory", vkAllocateMemory(device, &allocate_info, nullptr, &allocation),
            memset(&allocation, 0xcd, sizeof(allocation))), "vkAllocateMemory")) return reason;
        result.memory = allocation;
        if (!checked(WOBY_HEAP_CALL("vkBindBufferMemory", vkBindBufferMemory(device, result.buffer, result.memory, 0),
            (void)0), "vkBindBufferMemory")) return reason;]==])
# Accept an already-patched #92 dependency tree as well as fresh source.
replace_exact(src/NoGraphicsAPI.cpp
[==[            require_vk(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));]==]
[==[            void* mapped = nullptr;
            if (!checked(WOBY_HEAP_CALL("vkMapMemory", vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                memset(&mapped, 0xcd, sizeof(mapped))), "vkMapMemory")) return;
            result.mapped = mapped;]==]
[==[            void* mapped = nullptr;
            if (!checked(WOBY_HEAP_CALL("vkMapMemory", vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                memset(&mapped, 0xcd, sizeof(mapped))), "vkMapMemory")) return reason;
            result.mapped = mapped;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[            void* mapped = nullptr;
            if (!checked(WOBY_HEAP_CALL("vkMapMemory", vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                memset(&mapped, 0xcd, sizeof(mapped))), "vkMapMemory")) return;
            result.mapped = mapped;]==]
[==[            void* mapped = nullptr;
            if (!checked(WOBY_HEAP_CALL("vkMapMemory", vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped),
                memset(&mapped, 0xcd, sizeof(mapped))), "vkMapMemory")) return reason;
            result.mapped = mapped;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    [[nodiscard]] GpuHeap allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept;]==]
[==[    [[nodiscard]] GpuHeap allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure = nullptr) noexcept;]==])
# Accept an already-patched #92 dependency tree as well as fresh source.
replace_exact(src/NoGraphicsAPI.cpp
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory) noexcept
{]==]
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure) noexcept
{]==]
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure) noexcept
{
    HeapAllocationFailure ignored;
    if (!failure) failure = &ignored;
    *failure = {};]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure) noexcept
{]==]
[==[GpuHeap Device::allocate_gpu_heap(VkDeviceSize size, MemoryType memory, HeapAllocationFailure* failure) noexcept
{
    HeapAllocationFailure ignored;
    if (!failure) failure = &ignored;
    *failure = {};]==])
# Accept an already-patched #92 dependency tree as well as fresh source.
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
    }]==]
[==[    GpuHeapOwner* heap = new (std::nothrow) GpuHeapOwner{.state = this};
    if (!heap) {
        *failure = {.operation = "GpuHeapOwner", .api_result = VK_ERROR_OUT_OF_HOST_MEMORY, .allocation_bytes = size};
        return {.error = allocation_error(VK_ERROR_OUT_OF_HOST_MEMORY)};
    }
    if (const auto error = create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided, failure)) {
        delete heap;
        return {.error = error};
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
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
    }]==]
[==[    GpuHeapOwner* heap = new (std::nothrow) GpuHeapOwner{.state = this};
    if (!heap) {
        *failure = {.operation = "GpuHeapOwner", .api_result = VK_ERROR_OUT_OF_HOST_MEMORY, .allocation_bytes = size};
        return {.error = allocation_error(VK_ERROR_OUT_OF_HOST_MEMORY)};
    }
    if (const auto error = create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided, failure)) {
        delete heap;
        return {.error = error};
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

replace_exact(${source} "        output = result;\n    }\n\n    [[nodiscard]] GpuHeap"
    "        output = result;\n        return nullptr;\n    }\n\n    [[nodiscard]] GpuHeap")
replace_exact(${source} "    GpuHeapOwner* heap = new GpuHeapOwner{.state = this};"
    "    GpuHeapOwner* heap = new (std::nothrow) GpuHeapOwner{.state = this};\n    if (!heap) return {.error = \"Insufficient CPU memory for GPU buffer ownership.\"};")
foreach(call IN ITEMS
    "create_backing_buffer(heap->backing, backing_size, universal_buffer_usage | VK_BUFFER_USAGE_DESCRIPTOR_HEAP_BIT_EXT, cpu_visible_memory_properties, 0)")
    replace_exact(${source} "    ${call};" "    if (const auto error = ${call}) { delete heap; return {.error = error}; }")
endforeach()
# Pipelines and startup handles have nullable return types too.
replace_exact(${source} "    PSO* result = new PSO{" "    PSO* result = new (std::nothrow) PSO{")
foreach(kind IN ITEMS Graphics Compute)
    replace_exact(${source} "    require_vk(vkCreate${kind}Pipelines(device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso));"
        "    if (!result) return nullptr;\n    if (vkCreate${kind}Pipelines(device->device, VK_NULL_HANDLE, 1, &pso_info, nullptr, &result->pso) != VK_SUCCESS) { delete result; return nullptr; }")
endforeach()
replace_exact(${source} "    TimelineSemaphore* result = new TimelineSemaphore{" "    TimelineSemaphore* result = new (std::nothrow) TimelineSemaphore{")
replace_exact(${source} "    require_vk(vkCreateSemaphore(device->device, &create_info, nullptr, &result->semaphore));"
    "    if (!result) return nullptr;\n    if (vkCreateSemaphore(device->device, &create_info, nullptr, &result->semaphore) != VK_SUCCESS) { delete result; return nullptr; }")
replace_exact(${source} "    CommandPool* pool = new CommandPool{.state = device, .timestamps = device->queues[queue_index].timestamps};"
    "    CommandPool* pool = new (std::nothrow) CommandPool{.state = device, .timestamps = device->queues[queue_index].timestamps};\n    if (!pool) return nullptr;")
replace_exact(${source} "    require_vk(vkCreateCommandPool(device->device, &pool_info, nullptr, &pool->command_pool));"
    "    if (vkCreateCommandPool(device->device, &pool_info, nullptr, &pool->command_pool) != VK_SUCCESS) { delete pool; return nullptr; }")
replace_exact(${source} "    TextureHeapOwner* owner = new TextureHeapOwner{\n        .state = device,\n    };"
    "    TextureHeapOwner* owner = new (std::nothrow) TextureHeapOwner{\n        .state = device,\n    };\n    if (!owner) return {.error = \"Insufficient CPU memory for GPU texture ownership.\"};")
replace_exact(${source} "    require_vk(vkAllocateMemory(device->device, &allocate_info, nullptr, &owner->memory));"
    "    if (const auto error = allocation_error(vkAllocateMemory(device->device, &allocate_info, nullptr, &owner->memory))) { delete owner; return {.error = error}; }")
replace_exact(${source} "    Texture* result = new Texture{\n        .state = device,"
    "    Texture* result = new (std::nothrow) Texture{\n        .state = device,")
replace_exact(${source} [=[    require_vk(vkCreateImage(device->device, &texture.image_info, nullptr, &result->image));
    require_vk(vkBindImageMemory(device->device, result->image, heap.owner->memory, offset));]=] [=[    if (!result) return nullptr;
    if (vkCreateImage(device->device, &texture.image_info, nullptr, &result->image) != VK_SUCCESS ||
        vkBindImageMemory(device->device, result->image, heap.owner->memory, offset) != VK_SUCCESS) {
        delete result;
        return nullptr;
    }]=])
replace_exact(${source} "    RenderView* result = new RenderView{"
    "    RenderView* result = new (std::nothrow) RenderView{")
replace_exact(${source} "    require_vk(vkCreateImageView(texture->state->device, &view_info, nullptr, &result->view));"
    "    if (!result) return nullptr;\n    if (vkCreateImageView(texture->state->device, &view_info, nullptr, &result->view) != VK_SUCCESS) { delete result; return nullptr; }")
foreach(kind IN ITEMS Texture Sampler)
    string(TOLOWER "${kind}" lower)
    if(kind STREQUAL "Texture")
        set(stride image)
    else()
        set(stride sampler)
    endif()
    replace_exact(${source} "    return new ${kind}DescriptorHeap{\n        .state = device,\n        .storage = device->allocate_descriptor_heap(uint64(capacity) * device->heap_properties.${stride}DescriptorSize, DescriptorHeapType::${lower}),\n        .capacity = capacity,\n    };"
        "    auto storage = device->allocate_descriptor_heap(uint64(capacity) * device->heap_properties.${stride}DescriptorSize, DescriptorHeapType::${lower});\n    if (!storage.owner) return nullptr;\n    auto result = new (std::nothrow) ${kind}DescriptorHeap{.state = device, .storage = storage, .capacity = capacity};\n    if (!result) destroy_gpu_heap(storage);\n    return result;")
endforeach()
