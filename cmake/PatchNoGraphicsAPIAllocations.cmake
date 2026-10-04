# Geometry heaps are operation-owned: allocation failure must reach the caller,
# not abort the viewer. Keep initialization-only descriptor allocations unchanged.
replace_exact(src/NoGraphicsAPI.cpp
[==[#include <string.h>]==]
[==[#include <string.h>
#include <new> // Woby: recoverable GPU heap ownership allocation.]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0) const noexcept
    {
        output = {};
        detail::BackingBuffer result{};]==]
[==[VkMemoryPropertyFlags preferred, VkMemoryPropertyFlags avoided = 0, bool recoverable = false) const noexcept
    {
        output = {};
        detail::BackingBuffer result{};
        const auto accepted = [&](VkResult status) {
            if (status == VK_SUCCESS) return true;
            if (!recoverable) require_vk(status);
            fprintf(stderr, "GPU buffer allocation failed: bytes=%llu VkResult=%d\n",
                    static_cast<unsigned long long>(size), static_cast<int>(status));
            if (result.mapped) vkUnmapMemory(device, result.memory);
            if (result.buffer) vkDestroyBuffer(device, result.buffer, nullptr);
            if (result.memory) vkFreeMemory(device, result.memory, nullptr);
            result = {};
            return false;
        };]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));]==]
[==[        VkBuffer buffer = VK_NULL_HANDLE;
        if (!accepted(vkCreateBuffer(device, &buffer_info, nullptr, &buffer))) return;
        result.buffer = buffer;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        assert(has_memory_type);
        if (!has_memory_type)
            abort();]==]
[==[        if (!has_memory_type)
        {
            (void)accepted(VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return;
        }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        require_vk(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory));
        require_vk(vkBindBufferMemory(device, result.buffer, result.memory, 0));]==]
[==[        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (!accepted(vkAllocateMemory(device, &allocate_info, nullptr, &memory))) return;
        result.memory = memory;
        if (!accepted(vkBindBufferMemory(device, result.buffer, result.memory, 0))) return;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[            require_vk(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));]==]
[==[            void* mapped = nullptr;
            if (!accepted(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapped))) return;
            result.mapped = mapped;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    GpuHeapOwner* heap = new GpuHeapOwner{.state = this};
    create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided);]==]
[==[    GpuHeapOwner* heap = new (std::nothrow) GpuHeapOwner{.state = this};
    if (!heap) return {};
    create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided, true);
    if (!heap->backing.buffer)
    {
        delete heap;
        return {};
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[            result = {};
            return false;
        };
        const VkBufferCreateInfo buffer_info{]==]
[==[            result = {};
            return false;
        };
        // Reject heaps that cannot fit any compatible physical memory type
        // before asking the driver to create an impossible buffer object.
        uint32 candidate = 0;
        if (recoverable && !find_memory_type(~uint32{0}, required, preferred, size, candidate, avoided))
        {
            (void)accepted(VK_ERROR_OUT_OF_DEVICE_MEMORY);
            return;
        }
        const VkBufferCreateInfo buffer_info{]==])
