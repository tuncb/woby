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
replace_exact(${source} "    void create_backing_buffer(" "    const char* create_backing_buffer(")
replace_exact(${source} "        require_vk(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer));" [=[
        const auto fail = [&](const char* error) {
            if (result.mapped) vkUnmapMemory(device, result.memory);
            if (result.buffer) vkDestroyBuffer(device, result.buffer, nullptr);
            if (result.memory) vkFreeMemory(device, result.memory, nullptr);
            return error;
        };
        if (const auto error = allocation_error(vkCreateBuffer(device, &buffer_info, nullptr, &result.buffer)))
            return fail(error);]=])
replace_exact(${source} [=[        assert(has_memory_type);
        if (!has_memory_type)
            abort();]=] [=[        if (!has_memory_type)
            return fail("No compatible GPU memory heap can hold this buffer.");]=])
replace_exact(${source} [=[        require_vk(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory));
        require_vk(vkBindBufferMemory(device, result.buffer, result.memory, 0));]=] [=[        if (const auto error = allocation_error(vkAllocateMemory(device, &allocate_info, nullptr, &result.memory)))
            return fail(error);
        if (const auto error = allocation_error(vkBindBufferMemory(device, result.buffer, result.memory, 0)))
            return fail(error);]=])
replace_exact(${source} [=[            require_vk(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped));]=] [=[            if (const auto error = allocation_error(vkMapMemory(
                device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped)))
                return fail(error);]=])
replace_exact(${source} "        output = result;\n    }\n\n    [[nodiscard]] GpuHeap"
    "        output = result;\n        return nullptr;\n    }\n\n    [[nodiscard]] GpuHeap")
replace_exact(${source} "    GpuHeapOwner* heap = new GpuHeapOwner{.state = this};"
    "    GpuHeapOwner* heap = new (std::nothrow) GpuHeapOwner{.state = this};\n    if (!heap) return {.error = \"Insufficient CPU memory for GPU buffer ownership.\"};")
foreach(call IN ITEMS
    "create_backing_buffer(heap->backing, size, universal_buffer_usage, required, preferred, avoided)"
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
# Release a suballocation if native image creation fails. A null texture is not
# the same thing as a full page; surface driver failure instead of adding pages.
replace_exact(utility/src/texture_allocator.cpp "#include <assert.h>" "#include <assert.h>\n#include <stdexcept>")
replace_exact(utility/src/texture_allocator.cpp "const TextureDesc& desc) noexcept" "const TextureDesc& desc) /* Woby: allocation can throw. */")
replace_exact(utility/include/NoGraphicsAPIUtility/texture_allocator.hpp "const TextureDesc& desc) noexcept" "const TextureDesc& desc) /* Woby: allocation can throw. */")
replace_exact(utility/src/texture_allocator.cpp [=[    return {
        .texture = create_texture(commands, desc, heap_, uint64{range.offset} * ranges_.element_size),
        .token = range.token,
    };]=] [=[    auto texture = create_texture(commands, desc, heap_, uint64{range.offset} * ranges_.element_size);
    if (!texture) {
        ranges_.free(range.token);
        throw std::runtime_error("GPU texture creation failed: insufficient CPU/GPU memory or a driver error.");
    }
    return {.texture = texture, .token = range.token};]=])
# These utility constructors allocate CPU storage. Let bad_alloc propagate to
# Woby, cleaning partially initialized storage before it reaches the handler.
foreach(path IN ITEMS utility/src/heap_allocator.cpp utility/include/NoGraphicsAPIUtility/heap_allocator.hpp)
    replace_exact(${path} "uint32 max_allocations) noexcept" "uint32 max_allocations) /* Woby: allocation can throw. */")
    if(path MATCHES "cpp$")
        replace_exact(${path} "uint64 allocation_element_size) noexcept" "uint64 allocation_element_size) /* Woby: allocation can throw. */")
    else()
        replace_exact(${path} "uint64 element_size) noexcept" "uint64 element_size) /* Woby: allocation can throw. */")
    endif()
endforeach()
replace_exact(utility/src/heap_allocator.cpp "    free_nodes = new NodeIndex[node_capacity];"
    "    try { free_nodes = new NodeIndex[node_capacity]; }\n    catch (...) { delete[] nodes; throw; }")
foreach(path IN ITEMS utility/src/texture_allocator.cpp utility/include/NoGraphicsAPIUtility/texture_allocator.hpp)
    replace_exact(${path} "uint32 max_textures) noexcept" "uint32 max_textures) /* Woby: allocation can throw. */")
endforeach()
foreach(path IN ITEMS utility/src/delete_queue.cpp utility/include/NoGraphicsAPIUtility/delete_queue.hpp)
    replace_exact(${path} "uint32 capacity) noexcept" "uint32 capacity) /* Woby: allocation can throw. */")
endforeach()
replace_exact(utility/src/upload_queue.cpp "#include <assert.h>" "#include <assert.h>\n#include <stdexcept>")
replace_exact(utility/src/upload_queue.cpp "uint32 max_pending_batches) noexcept" "uint32 max_pending_batches) /* Woby: allocation can throw. */")
replace_exact(utility/include/NoGraphicsAPIUtility/upload_queue.hpp "uint32 max_pending_batches = 2) noexcept" "uint32 max_pending_batches = 2) /* Woby: allocation can throw. */")
replace_exact(utility/src/upload_queue.cpp "    state_.completion.semaphore = create_timeline_semaphore(device);"
    "    if (!state_.heap.owner) throw std::runtime_error(state_.heap.error);\n    try {\n    state_.completion.semaphore = create_timeline_semaphore(device);")
replace_exact(utility/src/upload_queue.cpp "        reset_command_pool(batch.pool);\n    }\n}"
    "        reset_command_pool(batch.pool);\n    }\n    } catch (...) { destroy(); throw; }\n}")
replace_exact(utility/src/upload_queue.cpp "    state_.completion.semaphore = create_timeline_semaphore(device);"
    "    state_.completion.semaphore = create_timeline_semaphore(device);\n    if (!state_.completion.semaphore) throw std::runtime_error(\"GPU upload timeline allocation failed.\");")
replace_exact(utility/src/upload_queue.cpp "        batch.pool = create_command_pool(device, queue_index);"
    "        batch.pool = create_command_pool(device, queue_index);\n        if (!batch.pool) throw std::runtime_error(\"GPU upload command pool allocation failed.\");")
