# Checked recording/submission and allocation-free ownership, against the pinned backend.
configure_file("${CMAKE_CURRENT_LIST_DIR}/ngapi/backend_allocation.hpp" "${NGAPI_SOURCE}/include/NoGraphicsAPI/backend_allocation.hpp" COPYONLY)

replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[====[    TimelineSemaphore* semaphore = nullptr;
    uint64 value = 0;
};

struct SubmitDesc
{
]====]
[====[    TimelineSemaphore* semaphore = nullptr;
    uint64 value = 0;
};

// A presentation error can follow successful submission. Always commit the
// completion value when submitted is true, even if error is non-null.
struct SubmitResult { const char* error = nullptr; bool submitted = false; };

struct SubmitDesc
{
]====])

replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[====[
struct SwapchainFrame
{
    RenderView* render_view = nullptr;
    uint32x2 extent = {};
};
]====]
[====[
struct SwapchainFrame
{
    const char* error = nullptr;
    RenderView* render_view = nullptr;
    uint32x2 extent = {};
};
]====])

replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[====[// submit_and_present prepares the image for presentation after all submitted buffers.
// Empty while the drawable extent is zero. A nonempty acquire must be submitted with submit_and_present on queue zero.
[[nodiscard]] SwapchainFrame acquire(CommandBuffer* commands) noexcept;
void submit_and_present(Device* device, const SubmitDesc& desc) noexcept;

// Every non-null returned pointer is 16-byte aligned. GPU heaps are raw blocks for application-side suballocation.
[[nodiscard]] GpuHeap create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory = MemoryType::cpu_visible) noexcept;
]====]
[====[// submit_and_present prepares the image for presentation after all submitted buffers.
// Empty while the drawable extent is zero. A nonempty acquire must be submitted with submit_and_present on queue zero.
[[nodiscard]] SwapchainFrame acquire(CommandBuffer* commands) noexcept;
SubmitResult submit_and_present(Device* device, const SubmitDesc& desc) noexcept;

// Every non-null returned pointer is 16-byte aligned. GPU heaps are raw blocks for application-side suballocation.
[[nodiscard]] GpuHeap create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory = MemoryType::cpu_visible) noexcept;
]====])

replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[====[// Buffers from a pool must be submitted to the selected queue's family.
[[nodiscard]] CommandPool* create_command_pool(Device* device, uint32 queue_index = 0) noexcept;
void destroy_command_pool(CommandPool* pool) noexcept;
void reset_command_pool(CommandPool* pool) noexcept;
[[nodiscard]] CommandBuffer* begin_commands(CommandPool* pool) noexcept;
void end_commands(CommandBuffer* commands) noexcept;
// Submit any ended subset exactly once before pool reset. Order completion semaphore signal values across queues.
// queue_index must be less than DeviceCaps::queue_count; omitted selects queue zero.
void submit(Device* device, const SubmitDesc& desc, uint32 queue_index = 0) noexcept;

void set_texture_descriptor_heap(CommandBuffer* commands, TextureDescriptorHeap* heap) noexcept;
void set_sampler_descriptor_heap(CommandBuffer* commands, SamplerDescriptorHeap* heap) noexcept;
]====]
[====[// Buffers from a pool must be submitted to the selected queue's family.
[[nodiscard]] CommandPool* create_command_pool(Device* device, uint32 queue_index = 0) noexcept;
void destroy_command_pool(CommandPool* pool) noexcept;
const char* reset_command_pool(CommandPool* pool) noexcept;
const char* command_pool_error(const CommandPool* pool) noexcept;
[[nodiscard]] CommandBuffer* begin_commands(CommandPool* pool) noexcept;
const char* end_commands(CommandBuffer* commands) noexcept;
// Submit any ended subset exactly once before pool reset. Order completion semaphore signal values across queues.
// queue_index must be less than DeviceCaps::queue_count; omitted selects queue zero.
SubmitResult submit(Device* device, const SubmitDesc& desc, uint32 queue_index = 0) noexcept;
// Discard the acquired drawable before resetting/destroying abandoned commands.
void abandon_acquired_image(Device* device) noexcept;

void set_texture_descriptor_heap(CommandBuffer* commands, TextureDescriptorHeap* heap) noexcept;
void set_sampler_descriptor_heap(CommandBuffer* commands, SamplerDescriptorHeap* heap) noexcept;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <string.h>

]====]
[====[#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <new>
#include <NoGraphicsAPI/backend_allocation.hpp>
#include <new>
#include <string.h>

]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    abort();
}

void require_vk(VkResult result) noexcept
{
    if (result != VK_SUCCESS)
        abort_vk_failure(result);
]====]
[====[    abort();
}

[[maybe_unused]] void require_vk(VkResult result) noexcept
{
    if (result != VK_SUCCESS)
        abort_vk_failure(result);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    (void)result;
}

void require_error(Error error) noexcept
{
    if (error != Error::none)
    {
]====]
[====[    (void)result;
}

[[maybe_unused]] void require_error(Error error) noexcept
{
    if (error != Error::none)
    {
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[
struct SwapchainDeleteQueue
{
    void push(uint64 retire_value, VkSwapchainKHR swapchain, VkImageView view) noexcept
    {
        assert(swapchain && view);
        if (count == capacity)
        {
            const size_t old_size = capacity;
            capacity = old_size == 0 ? 1 : old_size * 2;
            entries = static_cast<DeferredSwapchainImage*>(realloc(entries, capacity * sizeof(DeferredSwapchainImage)));
            for (size_t index = 0; index < first; ++index)
            {
                entries[old_size + index] = entries[index];
                entries[index] = {};
            }
        }
        if (count != 0)
        {
            const size_t back = (first + count - 1) % capacity;
]====]
[====[
struct SwapchainDeleteQueue
{
    bool reserve(size_t additional) noexcept
    {
        if (additional > SIZE_MAX - count) return false;
        if (count + additional <= capacity) return true;
        const size_t old_size = capacity;
        if (old_size > SIZE_MAX / 2) return false;
        const size_t required = std::max(count + additional, old_size * 2);
        if (!reserve_backend_array(entries, capacity, required)) return false;
        for (size_t index = 0; index < first; ++index)
        {
            entries[old_size + index] = entries[index];
            entries[index] = {};
        }
        return true;
    }

    void push(uint64 retire_value, VkSwapchainKHR swapchain, VkImageView view) noexcept
    {
        assert(swapchain && view && count < capacity);
        if (count != 0)
        {
            const size_t back = (first + count - 1) % capacity;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    Swapchain* swapchain = nullptr;
    bool submitted = false;
    bool has_epilogue = false;
};

struct CommandPool
]====]
[====[    Swapchain* swapchain = nullptr;
    bool submitted = false;
    bool has_epilogue = false;
    const char* error = nullptr;
    bool ended = false;
};

struct CommandPool
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    CommandBuffer* last = nullptr;
    CommandBuffer* next_buffer = nullptr;
    bool timestamps = false;
};

namespace detail
]====]
[====[    CommandBuffer* last = nullptr;
    CommandBuffer* next_buffer = nullptr;
    bool timestamps = false;
    const char* error = nullptr;
};

namespace detail
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    VkFence presented = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool present_pending = false;
};
]====]
[====[    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    VkFence presented = VK_NULL_HANDLE;
    VkFence acquisition_complete = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    bool present_pending = false;
};
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device.physical_device, &format_info, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        return false;
    require_vk(result);
    if (output)
        *output = properties.imageFormatProperties;
    return fits_image_format_properties(image_info, properties.imageFormatProperties);
]====]
[====[    const VkResult result = vkGetPhysicalDeviceImageFormatProperties2(device.physical_device, &format_info, &properties);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED)
        return false;
    if (result != VK_SUCCESS) return false;
    if (output)
        *output = properties.imageFormatProperties;
    return fits_image_format_properties(image_info, properties.imageFormatProperties);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        assert(swapchain_delete_queue.count == 0);
    }
    if (device && presentation_retirement) vkDestroySemaphore(device, presentation_retirement, nullptr);
    for (uint32 index = 0; index < queue_count; ++index)
    {
        free(queues[index].command_submit_infos);
        free(queues[index].wait_submit_infos);
]====]
[====[        assert(swapchain_delete_queue.count == 0);
    }
    if (device && presentation_retirement) vkDestroySemaphore(device, presentation_retirement, nullptr);
    for (uint32 index = 0; queues && index < queue_count; ++index)
    {
        free(queues[index].command_submit_infos);
        free(queues[index].wait_submit_infos);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[            wait_presentation_retirement(next - max_timeline_value_difference);
        assert(next - completed_presentation_retirement <= max_timeline_value_difference);
    }
    presentation_retirement_value = next;
    return next;
}

Error Device::create_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.acquired && !context.rendered && !context.presented && !context.swapchain && !context.present_pending);
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    Error error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &context.acquired));
    if (error != Error::none)
        return error;
    error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &context.rendered));
    if (error != Error::none)
    {
        destroy_present_context(context);
        return error;
    }
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    error = error_from_vk(vkCreateFence(device, &fence_info, nullptr, &context.presented));
    if (error != Error::none) destroy_present_context(context);
    return error;
}

]====]
[====[            wait_presentation_retirement(next - max_timeline_value_difference);
        assert(next - completed_presentation_retirement <= max_timeline_value_difference);
    }
    return next; // Commit only after vkQueueSubmit2 succeeds.
}

Error Device::create_present_context(detail::PresentContext& context) noexcept
{
    assert(!context.acquired && !context.rendered && !context.presented && !context.acquisition_complete && !context.swapchain && !context.present_pending);
    const VkSemaphoreCreateInfo semaphore_info{
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
    };
    Error error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &context.acquired));
    if (error != Error::none) {
        context.acquired = VK_NULL_HANDLE;
        return error;
    }
    error = error_from_vk(vkCreateSemaphore(device, &semaphore_info, nullptr, &context.rendered));
    if (error != Error::none)
    {
        context.rendered = VK_NULL_HANDLE;
        destroy_present_context(context);
        return error;
    }
    const VkFenceCreateInfo fence_info{
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    };
    error = error_from_vk(vkCreateFence(device, &fence_info, nullptr, &context.acquisition_complete));
    if (error != Error::none) {
        context.acquisition_complete = VK_NULL_HANDLE;
        destroy_present_context(context);
        return error;
    }
    error = error_from_vk(vkCreateFence(device, &fence_info, nullptr, &context.presented));
    if (error != Error::none) {
        context.presented = VK_NULL_HANDLE;
        destroy_present_context(context);
    }
    return error;
}

]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    if (device && context.acquired) vkDestroySemaphore(device, context.acquired, nullptr);
    if (device && context.rendered) vkDestroySemaphore(device, context.rendered, nullptr);
    if (device && context.presented) vkDestroyFence(device, context.presented, nullptr);
    context = {};
}

]====]
[====[    if (device && context.acquired) vkDestroySemaphore(device, context.acquired, nullptr);
    if (device && context.rendered) vkDestroySemaphore(device, context.rendered, nullptr);
    if (device && context.presented) vkDestroyFence(device, context.presented, nullptr);
    if (device && context.acquisition_complete) vkDestroyFence(device, context.acquisition_complete, nullptr);
    context = {};
}

]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[void Device::queue_retired_swapchain(detail::RetiredSwapchain& retired) noexcept
{
    assert(retired.handle && retired.view_count != 0);
    for (uint32 index = 0; index < retired.view_count; ++index)
    {
        const VkImageView view = retired.views[index];
]====]
[====[void Device::queue_retired_swapchain(detail::RetiredSwapchain& retired) noexcept
{
    assert(retired.handle && retired.view_count != 0);
    if (!swapchain_delete_queue.reserve(retired.view_count))
    {
        // Presentation fences for this handle are already retired. Wait only
        // for work actually submitted, then destroy this whole group directly.
        wait_presentation_retirement(presentation_retirement_value);
        for (uint32 index = 0; index < retired.view_count; ++index)
            vkDestroyImageView(device, retired.views[index], nullptr);
        vkDestroySwapchainKHR(device, retired.handle, nullptr);
        retired = {};
        return;
    }
    for (uint32 index = 0; index < retired.view_count; ++index)
    {
        const VkImageView view = retired.views[index];
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        return { .error = Error::unsupported };
    }

    Device* state = new Device;
    state->timestamp_query_count = desc.timestamp_query_count;
    state->window = desc.window;
    state->drawable_extent = desc.drawable_extent;
]====]
[====[        return { .error = Error::unsupported };
    }

    Device* state = new (detail::BackendAllocation{}) Device;
    if (!state) return fail_initialization(desc, nullptr, Error::driver_error, "Insufficient CPU memory for Vulkan device ownership");
    state->timestamp_query_count = desc.timestamp_query_count;
    state->window = desc.window;
    state->drawable_extent = desc.drawable_extent;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        .ppEnabledExtensionNames = enabled_instance_extension_count ? enabled_instance_extensions : nullptr,
    };
    error = diagnostic_vk_result(desc, "vkCreateInstance", vkCreateInstance(&instance_info, nullptr, &state->instance));
    if (error != Error::none)
        return fail_device_creation(state, error);

#if !defined(NDEBUG)
    if (debug_utils_available)
]====]
[====[        .ppEnabledExtensionNames = enabled_instance_extension_count ? enabled_instance_extensions : nullptr,
    };
    error = diagnostic_vk_result(desc, "vkCreateInstance", vkCreateInstance(&instance_info, nullptr, &state->instance));
    if (error != Error::none) {
        state->instance = VK_NULL_HANDLE;
        return fail_device_creation(state, error);
    }

#if !defined(NDEBUG)
    if (debug_utils_available)
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[            .pfnUserCallback = debug_callback,
        };
        error = diagnostic_vk_result(desc, "vkCreateDebugUtilsMessengerEXT", create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger));
        if (error != Error::none)
            return fail_device_creation(state, error);
    }
#endif

]====]
[====[            .pfnUserCallback = debug_callback,
        };
        error = diagnostic_vk_result(desc, "vkCreateDebugUtilsMessengerEXT", create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger));
        if (error != Error::none) {
            state->debug_messenger = VK_NULL_HANDLE;
            return fail_device_creation(state, error);
        }
    }
#endif

]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[            .hwnd = static_cast<HWND>(desc.window),
        };
        error = diagnostic_vk_result(desc, "vkCreateWin32SurfaceKHR", vkCreateWin32SurfaceKHR(state->instance, &surface_info, nullptr, &state->surface));
        if (error != Error::none)
            return fail_device_creation(state, error);
    }
#endif

]====]
[====[            .hwnd = static_cast<HWND>(desc.window),
        };
        error = diagnostic_vk_result(desc, "vkCreateWin32SurfaceKHR", vkCreateWin32SurfaceKHR(state->instance, &surface_info, nullptr, &state->surface));
        if (error != Error::none) {
            state->surface = VK_NULL_HANDLE;
            return fail_device_creation(state, error);
        }
    }
#endif

]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        queue_counts[type] = requested_counts[type] < selected.queue_counts[type] ? requested_counts[type] : selected.queue_counts[type];
        state->queue_count += queue_counts[type];
    }
    state->queues = new detail::Queue[state->queue_count];
    float* queue_priorities = new float[state->queue_count];
    for (uint32 index = 0; index < state->queue_count; ++index)
        queue_priorities[index] = 1.0f;
    VkDeviceQueueCreateInfo queue_infos[queue_type_count]{};
]====]
[====[        queue_counts[type] = requested_counts[type] < selected.queue_counts[type] ? requested_counts[type] : selected.queue_counts[type];
        state->queue_count += queue_counts[type];
    }
    state->queues = detail::backend_array<detail::Queue>(state->queue_count);
    if (!state->queues) return fail_initialization(desc, state, Error::driver_error, "Insufficient CPU memory for Vulkan queues");
    float* queue_priorities = detail::backend_array<float>(state->queue_count);
    if (!queue_priorities) return fail_initialization(desc, state, Error::driver_error, "Insufficient CPU memory for Vulkan queue priorities");
    for (uint32 index = 0; index < state->queue_count; ++index)
        queue_priorities[index] = 1.0f;
    VkDeviceQueueCreateInfo queue_infos[queue_type_count]{};
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    };
    error = diagnostic_vk_result(desc, "vkCreateDevice", vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device));
    delete[] queue_priorities;
    if (error != Error::none)
        return fail_device_creation(state, error);
    first_queue = 0;
    for (uint32 type = 0; type < queue_type_count; ++type)
    {
]====]
[====[    };
    error = diagnostic_vk_result(desc, "vkCreateDevice", vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device));
    delete[] queue_priorities;
    if (error != Error::none) {
        state->device = VK_NULL_HANDLE;
        return fail_device_creation(state, error);
    }
    first_queue = 0;
    for (uint32 type = 0; type < queue_type_count; ++type)
    {
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[            .pNext = &type_info,
        };
        error = diagnostic_vk_result(desc, "vkCreateSemaphore (presentation retirement)", vkCreateSemaphore(state->device, &semaphore_info, nullptr, &state->presentation_retirement));
        if (error != Error::none)
            return fail_device_creation(state, error);
        for (uint32 index = 0; index < state->present_context_count; ++index)
        {
            error = state->create_present_context(state->present_contexts[index]);
]====]
[====[            .pNext = &type_info,
        };
        error = diagnostic_vk_result(desc, "vkCreateSemaphore (presentation retirement)", vkCreateSemaphore(state->device, &semaphore_info, nullptr, &state->presentation_retirement));
        if (error != Error::none) {
            state->presentation_retirement = VK_NULL_HANDLE;
            return fail_device_creation(state, error);
        }
        for (uint32 index = 0; index < state->present_context_count; ++index)
        {
            error = state->create_present_context(state->present_contexts[index]);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[#endif
    if (presentation)
    {
        state->swapchain = new Swapchain;
        state->swapchain->state = state;
        state->swapchain->format = desc.swapchain_format;
        error = recreate_swapchain(*state->swapchain);
]====]
[====[#endif
    if (presentation)
    {
        state->swapchain = new (detail::BackendAllocation{}) Swapchain;
        if (!state->swapchain) return fail_initialization(desc, state, Error::driver_error, "Insufficient CPU memory for Vulkan swapchain ownership");
        state->swapchain->state = state;
        state->swapchain->format = desc.swapchain_format;
        error = recreate_swapchain(*state->swapchain);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[{
    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(swapchain.state->physical_device, swapchain.state->surface, &capabilities));
    require_error(error);

    const VkExtent2D extent = surface_extent(*swapchain.state, capabilities);
    const uint32 variable_extent = UINT_MAX;
]====]
[====[{
    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(swapchain.state->physical_device, swapchain.state->surface, &capabilities));
    // A failed query leaves capabilities undefined. Defer to checked recreation.
    if (error != Error::none) return true;

    const VkExtent2D extent = surface_extent(*swapchain.state, capabilities);
    const uint32 variable_extent = UINT_MAX;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[
    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device, device->surface, &capabilities));
    require_error(error);
    const VkExtent2D extent = surface_extent(*device, capabilities);
    if (extent.width == UINT_MAX || extent.height == UINT_MAX)
    {
        require_error(Error::unsupported);
    }

    Swapchain& swapchain = *device->swapchain;
    if (swapchain.handle && (swapchain.width != extent.width || swapchain.height != extent.height))
    {
        swapchain.recreate_required = true;
]====]
[====[
    VkSurfaceCapabilitiesKHR capabilities{};
    const Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device, device->surface, &capabilities));
    Swapchain& swapchain = *device->swapchain;
    if (error != Error::none)
    {
        swapchain.recreate_required = true;
        return {swapchain.width, swapchain.height};
    }
    const VkExtent2D extent = surface_extent(*device, capabilities);
    if (extent.width == UINT_MAX || extent.height == UINT_MAX)
    {
        swapchain.recreate_required = true;
        return {swapchain.width, swapchain.height};
    }

    if (swapchain.handle && (swapchain.width != extent.width || swapchain.height != extent.height))
    {
        swapchain.recreate_required = true;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        if (!swapchain->handle || swapchain->recreate_required)
        {
            const Error error = recreate_swapchain(*swapchain);
            require_error(error);
            const bool drawable = swapchain->handle && swapchain->width != 0 && swapchain->height != 0;
            if (!drawable)
                return {};
]====]
[====[        if (!swapchain->handle || swapchain->recreate_required)
        {
            const Error error = recreate_swapchain(*swapchain);
            if (error != Error::none) return {.error = "Cannot allocate or recreate the Vulkan swapchain."};
            const bool drawable = swapchain->handle && swapchain->width != 0 && swapchain->height != 0;
            if (!drawable)
                return {};
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[            assert(!present_context->present_pending && !present_context->swapchain);
        }

        uint32 image_index = 0;
        const VkResult result = vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            ~uint64{0},
            present_context->acquired,
            VK_NULL_HANDLE,
            &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
]====]
[====[            assert(!present_context->present_pending && !present_context->swapchain);
        }

        if (!present_context->acquired && device->create_present_context(*present_context) != Error::none)
            return {.error = "Cannot allocate Vulkan presentation synchronization."};
        const auto reset = vkResetFences(device->device, 1, &present_context->acquisition_complete);
        if (reset != VK_SUCCESS) return {.error = allocation_error(reset)};
        uint32 image_index = 0;
        const VkResult result = vkAcquireNextImageKHR(
            device->device,
            swapchain->handle,
            ~uint64{0},
            present_context->acquired,
            present_context->acquisition_complete,
            &image_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            abort_vk_failure(result);
        assert((image_index < swapchain->image_count) && "swapchain returned an invalid image index");

        swapchain->image_index = image_index;
]====]
[====[            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            return {.error = allocation_error(result)};
        assert((image_index < swapchain->image_count) && "swapchain returned an invalid image index");

        swapchain->image_index = image_index;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        assert(!commands->swapchain && "an acquired swapchain image must be presented before destroying its command pool");
        pool->first = commands->next;
        if (commands->timestamp_pool) vkDestroyQueryPool(pool->state->device, commands->timestamp_pool, nullptr);
        free(commands->timestamp_destinations);
        free(commands->timestamp_results);
        delete commands;
    }
    vkDestroyCommandPool(pool->state->device, pool->command_pool, nullptr);
    delete pool;
}

void reset_command_pool(CommandPool* pool) noexcept
{
    assert(pool && pool->state && pool->command_pool);
    for (CommandBuffer* commands = pool->first; commands; commands = commands->next)
]====]
[====[        assert(!commands->swapchain && "an acquired swapchain image must be presented before destroying its command pool");
        pool->first = commands->next;
        if (commands->timestamp_pool) vkDestroyQueryPool(pool->state->device, commands->timestamp_pool, nullptr);
        delete[] commands->timestamp_destinations;
        delete[] commands->timestamp_results;
        delete commands;
    }
    vkDestroyCommandPool(pool->state->device, pool->command_pool, nullptr);
    delete pool;
}

const char* reset_command_pool(CommandPool* pool) noexcept
{
    assert(pool && pool->state && pool->command_pool);
    for (CommandBuffer* commands = pool->first; commands; commands = commands->next)
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        commands->submitted = false;
        commands->timestamp_count = 0;
    }
    assert_vk(vkResetCommandPool(pool->state->device, pool->command_pool, 0));
    pool->next_buffer = pool->first;
}

void read_timestamps(CommandPool* pool) noexcept
]====]
[====[        commands->submitted = false;
        commands->timestamp_count = 0;
    }
    pool->error = allocation_error(vkResetCommandPool(pool->state->device, pool->command_pool, 0));
    if (!pool->error) pool->next_buffer = pool->first;
    return pool->error;
}

void read_timestamps(CommandPool* pool) noexcept
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    }
}

CommandBuffer* begin_commands(CommandPool* pool) noexcept
{
    assert(pool && pool->state && pool->command_pool);
    CommandBuffer* commands = pool->next_buffer;
    if (commands)
    {
        pool->next_buffer = commands->next;
    }
    else
    {
        commands = new CommandBuffer{.state = pool->state, .command_pool = pool->command_pool};
        const VkCommandBufferAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        require_vk(vkAllocateCommandBuffers(pool->state->device, &allocate_info, &commands->command_buffer));
        if (pool->timestamps)
        {
            const VkQueryPoolCreateInfo query_info{
]====]
[====[    }
}

const char* command_pool_error(const CommandPool* pool) noexcept { return pool->error; }

CommandBuffer* begin_commands(CommandPool* pool) noexcept
{
    assert(pool && pool->state && pool->command_pool);
    pool->error = nullptr;
    CommandBuffer* commands = pool->next_buffer;
    if (commands) pool->next_buffer = commands->next;
    else
    {
        commands = new (detail::BackendAllocation{}) CommandBuffer{.state = pool->state, .command_pool = pool->command_pool};
        if (!commands) { pool->error = allocation_error(VK_ERROR_OUT_OF_HOST_MEMORY); return nullptr; }
        const auto fail = [&](VkResult result) -> CommandBuffer* {
            pool->error = allocation_error(result);
            if (commands->timestamp_pool) vkDestroyQueryPool(pool->state->device, commands->timestamp_pool, nullptr);
            if (commands->command_buffer) vkFreeCommandBuffers(pool->state->device, pool->command_pool, 1, &commands->command_buffer);
            delete[] commands->timestamp_destinations;
            delete[] commands->timestamp_results;
            delete commands;
            return nullptr;
        };
        const VkCommandBufferAllocateInfo allocate_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool->command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer buffer{};
        VkResult result = vkAllocateCommandBuffers(pool->state->device, &allocate_info, &buffer);
        if (result != VK_SUCCESS) return fail(result);
        commands->command_buffer = buffer;
        if (pool->timestamps)
        {
            const VkQueryPoolCreateInfo query_info{
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = pool->state->timestamp_query_count,
            };
            require_vk(vkCreateQueryPool(pool->state->device, &query_info, nullptr, &commands->timestamp_pool));
            commands->timestamp_destinations = static_cast<uint64**>(malloc(sizeof(uint64*) * pool->state->timestamp_query_count));
            commands->timestamp_results = static_cast<uint64*>(malloc(sizeof(uint64) * pool->state->timestamp_query_count));
        }
        if (pool->last)
            pool->last->next = commands;
        else
            pool->first = commands;
        pool->last = commands;
    }
    assert(!commands->swapchain);
]====]
[====[                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = pool->state->timestamp_query_count,
            };
            VkQueryPool queries{};
            result = vkCreateQueryPool(pool->state->device, &query_info, nullptr, &queries);
            if (result != VK_SUCCESS) return fail(result);
            commands->timestamp_pool = queries;
            commands->timestamp_destinations = detail::backend_array<uint64*>(pool->state->timestamp_query_count);
            if (!commands->timestamp_destinations) return fail(VK_ERROR_OUT_OF_HOST_MEMORY);
            commands->timestamp_results = detail::backend_array<uint64>(pool->state->timestamp_query_count);
            if (!commands->timestamp_results) return fail(VK_ERROR_OUT_OF_HOST_MEMORY);
        }
        if (pool->last) pool->last->next = commands;
        else pool->first = commands;
        pool->last = commands;
    }
    assert(!commands->swapchain);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    assert_vk(vkBeginCommandBuffer(commands->command_buffer, &begin_info));
    commands->timestamp_count = 0;
    commands->submitted = false;
    commands->has_epilogue = false;
    if (commands->timestamp_pool)
        vkResetQueryPool(pool->state->device, commands->timestamp_pool, 0, pool->state->timestamp_query_count);
    return commands;
}

void end_commands(CommandBuffer* commands) noexcept
{
    assert(commands);
    VkCommandBuffer command_buffer = commands->command_buffer;
    if (commands->swapchain)
    {
        assert_vk(vkEndCommandBuffer(command_buffer));
        if (!commands->epilogue)
        {
            const VkCommandBufferAllocateInfo allocate_info{
]====]
[====[        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    commands->timestamp_count = 0;
    commands->submitted = false;
    commands->has_epilogue = false;
    commands->ended = false;
    commands->error = allocation_error(vkBeginCommandBuffer(commands->command_buffer, &begin_info));
    if (commands->error) { pool->error = commands->error; return nullptr; }
    if (commands->timestamp_pool)
        vkResetQueryPool(pool->state->device, commands->timestamp_pool, 0, pool->state->timestamp_query_count);
    return commands;
}

const char* end_commands(CommandBuffer* commands) noexcept
{
    assert(commands);
    if (commands->error) return commands->error;
    VkCommandBuffer command_buffer = commands->command_buffer;
    if (commands->swapchain)
    {
        commands->error = allocation_error(vkEndCommandBuffer(command_buffer));
        if (commands->error) return commands->error;
        if (!commands->epilogue)
        {
            const VkCommandBufferAllocateInfo allocate_info{
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            require_vk(vkAllocateCommandBuffers(commands->state->device, &allocate_info, &commands->epilogue));
        }
        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        command_buffer = commands->epilogue;
        assert_vk(vkBeginCommandBuffer(command_buffer, &begin_info));
        commands->has_epilogue = true;
        Swapchain* swapchain = commands->swapchain;
        assert(swapchain->acquired && swapchain->transition_commands == commands);
]====]
[====[                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VkCommandBuffer epilogue{};
            commands->error = allocation_error(vkAllocateCommandBuffers(commands->state->device, &allocate_info, &epilogue));
            if (commands->error) return commands->error;
            commands->epilogue = epilogue;
        }
        const VkCommandBufferBeginInfo begin_info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        command_buffer = commands->epilogue;
        commands->error = allocation_error(vkBeginCommandBuffer(command_buffer, &begin_info));
        if (commands->error) return commands->error;
        commands->has_epilogue = true;
        Swapchain* swapchain = commands->swapchain;
        assert(swapchain->acquired && swapchain->transition_commands == commands);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        };
        record_image_barriers(command_buffer, {&barrier, 1});
    }
    assert_vk(vkEndCommandBuffer(command_buffer));
}

namespace
{

void submit_commands(Device* device, const SubmitDesc& desc, uint32 queue_index, VkSemaphore wait_semaphore, VkSemaphore signal_semaphore) noexcept
{
    detail::Queue* queue = &device->queues[queue_index];
    TimelineSemaphore* completion = desc.completion.semaphore;
]====]
[====[        };
        record_image_barriers(command_buffer, {&barrier, 1});
    }
    commands->error = allocation_error(vkEndCommandBuffer(command_buffer));
    commands->ended = !commands->error;
    return commands->error;
}

namespace
{

SubmitResult submit_commands(Device* device, const SubmitDesc& desc, uint32 queue_index, VkSemaphore wait_semaphore, VkSemaphore signal_semaphore) noexcept
{
    detail::Queue* queue = &device->queues[queue_index];
    TimelineSemaphore* completion = desc.completion.semaphore;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    for (size_t index = 0; index < desc.commands.size; ++index)
    {
        assert(desc.commands.data[index]);
        if (desc.commands.data[index]->has_epilogue) ++command_count;
    }
    if (command_count > queue->command_submit_capacity)
    {
        queue->command_submit_capacity = queue->command_submit_capacity == 0 ? 4 : queue->command_submit_capacity * 2;
        if (queue->command_submit_capacity < command_count) queue->command_submit_capacity = command_count;
        queue->command_submit_infos = static_cast<VkCommandBufferSubmitInfo*>(
            realloc(queue->command_submit_infos, queue->command_submit_capacity * sizeof(VkCommandBufferSubmitInfo)));
    }
    for (size_t index = 0; index < desc.commands.size; ++index)
    {
        CommandBuffer* commands = desc.commands.data[index];
]====]
[====[    for (size_t index = 0; index < desc.commands.size; ++index)
    {
        assert(desc.commands.data[index]);
        if (desc.commands.data[index]->error) return {desc.commands.data[index]->error};
        if (!desc.commands.data[index]->ended) return {"Cannot submit unfinished command recording."};
        if (desc.commands.data[index]->has_epilogue) {
            if (command_count == UINT32_MAX) return {"Too many command buffers in a submission."};
            ++command_count;
        }
    }
    if (command_count > UINT32_MAX || desc.waits.size > UINT32_MAX - (wait_semaphore ? 1u : 0u))
        return {"Command submission exceeds Vulkan representation limits."};
    if (!detail::reserve_backend_array(queue->command_submit_infos, queue->command_submit_capacity, command_count))
        return {allocation_error(VK_ERROR_OUT_OF_HOST_MEMORY)};
    for (size_t index = 0; index < desc.commands.size; ++index)
    {
        CommandBuffer* commands = desc.commands.data[index];
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        }
    }
    const size_t wait_count = desc.waits.size + (wait_semaphore ? 1 : 0);
    if (wait_count > queue->wait_submit_capacity)
    {
        queue->wait_submit_capacity = queue->wait_submit_capacity == 0 ? 4 : queue->wait_submit_capacity * 2;
        if (queue->wait_submit_capacity < wait_count) queue->wait_submit_capacity = wait_count;
        queue->wait_submit_infos = static_cast<VkSemaphoreSubmitInfo*>(
            realloc(queue->wait_submit_infos, queue->wait_submit_capacity * sizeof(VkSemaphoreSubmitInfo)));
    }
    for (size_t index = 0; index < desc.waits.size; ++index)
    {
        const TimelinePoint point = desc.waits.data[index];
]====]
[====[        }
    }
    const size_t wait_count = desc.waits.size + (wait_semaphore ? 1 : 0);
    if (!detail::reserve_backend_array(queue->wait_submit_infos, queue->wait_submit_capacity, wait_count))
        return {allocation_error(VK_ERROR_OUT_OF_HOST_MEMORY)};
    for (size_t index = 0; index < desc.waits.size; ++index)
    {
        const TimelinePoint point = desc.waits.data[index];
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signal_infos,
    };
    assert_vk(vkQueueSubmit2(queue->queue, 1, &submit_info, VK_NULL_HANDLE));
    for (size_t index = 0; index < desc.commands.size; ++index) desc.commands.data[index]->submitted = true;
}

} // namespace

void submit(Device* device, const SubmitDesc& desc, uint32 queue_index) noexcept
{
    assert(device && queue_index < device->queue_count && "submit requires an available queue index");
    assert(desc.commands.data || desc.commands.size == 0);
]====]
[====[        .signalSemaphoreInfoCount = signal_count,
        .pSignalSemaphoreInfos = signal_infos,
    };
    const VkResult result = vkQueueSubmit2(queue->queue, 1, &submit_info, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) return {allocation_error(result)};
    if (signal_semaphore) device->presentation_retirement_value = signal_infos[2].value;
    for (size_t index = 0; index < desc.commands.size; ++index) desc.commands.data[index]->submitted = true;
    return {nullptr, true};
}

} // namespace

SubmitResult submit(Device* device, const SubmitDesc& desc, uint32 queue_index) noexcept
{
    assert(device && queue_index < device->queue_count && "submit requires an available queue index");
    assert(desc.commands.data || desc.commands.size == 0);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[    for (size_t index = 0; index < desc.commands.size; ++index)
        assert(desc.commands.data[index] && !desc.commands.data[index]->swapchain && "swapchain commands require submit_and_present");
#endif
    submit_commands(device, desc, queue_index, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

void submit_and_present(Device* device, const SubmitDesc& desc) noexcept
{
    assert(device);
    assert(desc.commands.data || desc.commands.size == 0);
]====]
[====[    for (size_t index = 0; index < desc.commands.size; ++index)
        assert(desc.commands.data[index] && !desc.commands.data[index]->swapchain && "swapchain commands require submit_and_present");
#endif
    return submit_commands(device, desc, queue_index, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

SubmitResult submit_and_present(Device* device, const SubmitDesc& desc) noexcept
{
    assert(device);
    assert(desc.commands.data || desc.commands.size == 0);
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[#endif
    detail::PresentContext& present_context = *swapchain->present_context;
    assert(!present_context.present_pending && !present_context.swapchain);
    assert_vk(vkResetFences(device->device, 1, &present_context.presented));
    submit_commands(device, desc, 0, present_context.acquired, present_context.rendered);
    swapchain->transition_commands->swapchain = nullptr;
    swapchain->transition_commands = nullptr;
    swapchain->initialized[swapchain->image_index] = true;
]====]
[====[#endif
    detail::PresentContext& present_context = *swapchain->present_context;
    assert(!present_context.present_pending && !present_context.swapchain);
    // The separate acquisition fence is only waited on when abandoning an
    // image. Normal submission waits on the acquired semaphore on the GPU.
    VkResult result = vkResetFences(device->device, 1, &present_context.presented);
    if (result != VK_SUCCESS) return {allocation_error(result)};
    const auto submitted = submit_commands(device, desc, 0, present_context.acquired, present_context.rendered);
    if (!submitted.submitted) return submitted;
    swapchain->transition_commands->swapchain = nullptr;
    swapchain->transition_commands = nullptr;
    swapchain->initialized[swapchain->image_index] = true;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        .pSwapchains = &swapchain->handle,
        .pImageIndices = &swapchain->image_index,
    };
    const VkResult result = vkQueuePresentKHR(device->queues[0].queue, &present_info);
    present_context.present_pending = true;
    present_context.swapchain = swapchain->handle;
    swapchain->present_context = nullptr;
]====]
[====[        .pSwapchains = &swapchain->handle,
        .pImageIndices = &swapchain->image_index,
    };
    result = vkQueuePresentKHR(device->queues[0].queue, &present_info);
    present_context.present_pending = true;
    present_context.swapchain = swapchain->handle;
    swapchain->present_context = nullptr;
]====])

replace_exact(src/NoGraphicsAPI.cpp
[====[        swapchain->recreate_required = true;
    else if (result == VK_SUBOPTIMAL_KHR)
        swapchain->recreate_required = swapchain->recreate_required || swapchain_surface_configuration_changed(*swapchain);
    else if (result != VK_SUCCESS)
        abort_vk_failure(result);
}

void wait_idle(Device* device) noexcept
]====]
[====[        swapchain->recreate_required = true;
    else if (result == VK_SUBOPTIMAL_KHR)
        swapchain->recreate_required = swapchain->recreate_required || swapchain_surface_configuration_changed(*swapchain);
    else if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_DEVICE_LOST)
    {
        // OOM presentation is not enqueued: neither its fence nor semaphore
        // wait will execute. Finish submitted GPU work before destroying them.
        present_context.present_pending = false;
        (void)vkDeviceWaitIdle(device->device);
        device->finish_present_context(present_context);
        device->destroy_present_context(present_context);
        swapchain->recreate_required = true;
        return {allocation_error(result), true};
    }
    else if (result != VK_SUCCESS)
    {
        // Surface rejection still enqueues the semaphore wait and present fence.
        swapchain->recreate_required = true;
        return {allocation_error(result), true};
    }
    return {nullptr, true};
}

void abandon_acquired_image(Device* device) noexcept
{
    if (!device || !device->acquired_swapchain) return;
    Swapchain* swapchain = device->acquired_swapchain;
    auto& context = *swapchain->present_context;
    (void)vkWaitForFences(device->device, 1, &context.acquisition_complete, VK_TRUE, ~uint64{0});
    // This image was never submitted. Destroying the retired swapchain releases
    // it without a new submission (which could itself run out of memory).
    swapchain->transition_commands->swapchain = nullptr;
    swapchain->transition_commands = nullptr;
    swapchain->present_context = nullptr;
    swapchain->acquired = false;
    device->acquired_swapchain = nullptr;
    device->destroy_present_context(context);
    retire_swapchain_handle(*swapchain);
}

void wait_idle(Device* device) noexcept
]====])

replace_exact(utility/src/upload_queue.cpp
[====[        Batch& batch = state_.batches[index];
        batch.pool = create_command_pool(device, queue_index);
        if (!batch.pool) { error = "GPU upload command pool allocation failed."; destroy(); return; }
        end_commands(begin_commands(batch.pool));
        reset_command_pool(batch.pool);
    }
}

]====]
[====[        Batch& batch = state_.batches[index];
        batch.pool = create_command_pool(device, queue_index);
        if (!batch.pool) { error = "GPU upload command pool allocation failed."; destroy(); return; }
        auto* commands = begin_commands(batch.pool);
        if (!commands) { error = command_pool_error(batch.pool); destroy(); return; }
        error = end_commands(commands);
        if (!error) error = reset_command_pool(batch.pool);
        if (error) { destroy(); return; }
    }
}

]====])

replace_exact(utility/src/upload_queue.cpp
[====[        Batch& batch = state.batches[state.retirement_first];
        state.tail = batch.end;
        read_timestamps(batch.pool);
        reset_command_pool(batch.pool);
        state.retirement_first = (state.retirement_first + 1) % state.max_pending_batches;
        --state.retirement_count;
    }
]====]
[====[        Batch& batch = state.batches[state.retirement_first];
        state.tail = batch.end;
        read_timestamps(batch.pool);
        if (const auto failed = reset_command_pool(batch.pool)) error = failed;
        state.retirement_first = (state.retirement_first + 1) % state.max_pending_batches;
        --state.retirement_count;
    }
]====])

replace_exact(utility/src/upload_queue.cpp
[====[{
    State& state = state_;
    assert(state.completion.semaphore && !state.in_callback && byte_size && byte_size <= state.heap.range.size);
    if (state.operation_count == operation_limit) flush();
    for (;;)
    {
        uint64 offset = (state.head % state.heap.range.size + alignment - 1) & ~(alignment - 1);
]====]
[====[{
    State& state = state_;
    assert(state.completion.semaphore && !state.in_callback && byte_size && byte_size <= state.heap.range.size);
    if (error) return {};
    if (state.operation_count == operation_limit) flush();
    if (error) return {};
    for (;;)
    {
        uint64 offset = (state.head % state.heap.range.size + alignment - 1) & ~(alignment - 1);
]====])

replace_exact(utility/src/upload_queue.cpp
[====[        }
        const uint32 previous_count = state.retirement_count;
        reclaim();
        if (state.retirement_count != previous_count) continue;
        if (state.commands)
        {
            flush();
            continue;
        }
        wait_oldest();
    }
}

]====]
[====[        }
        const uint32 previous_count = state.retirement_count;
        reclaim();
        if (error) return {};
        if (state.retirement_count != previous_count) continue;
        if (state.commands)
        {
            flush();
            if (error) return {};
            continue;
        }
        wait_oldest();
        if (error) return {};
    }
}

]====])

replace_exact(utility/src/upload_queue.cpp
[====[{
    State& state = state_;
    assert(state.completion.semaphore && !state.in_callback);
    if (!state.commands)
    {
        if (state.retirement_count == state.max_pending_batches)
]====]
[====[{
    State& state = state_;
    assert(state.completion.semaphore && !state.in_callback);
    if (error) return nullptr;
    if (!state.commands)
    {
        if (state.retirement_count == state.max_pending_batches)
]====])

replace_exact(utility/src/upload_queue.cpp
[====[            reclaim();
            if (state.retirement_count == state.max_pending_batches) wait_oldest();
        }
        state.commands = begin_commands(state.batches[(state.retirement_first + state.retirement_count) % state.max_pending_batches].pool);
        barrier(state.commands, Stage::all_commands, state.queue_access, state.upload_stages, state.upload_access);
    }
    return state.commands;
]====]
[====[            reclaim();
            if (state.retirement_count == state.max_pending_batches) wait_oldest();
        }
        if (error) return nullptr;
        auto* pool = state.batches[(state.retirement_first + state.retirement_count) % state.max_pending_batches].pool;
        state.commands = begin_commands(pool);
        if (!state.commands) { error = command_pool_error(pool); return nullptr; }
        barrier(state.commands, Stage::all_commands, state.queue_access, state.upload_stages, state.upload_access);
    }
    return state.commands;
]====])

replace_exact(utility/src/upload_queue.cpp
[====[    {
        const uint64 size = source.size - offset < state_.heap.range.size ? source.size - offset : state_.heap.range.size;
        const GpuCpuRange<byte> staging = reserve(size);
        memcpy(staging.cpu, source.data + offset, size_t(size));
        copy_memory(begin(), gpu_range(staging), {.gpu = static_cast<byte*>(destination.gpu) + offset, .size = size});
        ++state_.operation_count;
        offset += size;
    }
]====]
[====[    {
        const uint64 size = source.size - offset < state_.heap.range.size ? source.size - offset : state_.heap.range.size;
        const GpuCpuRange<byte> staging = reserve(size);
        if (!staging.cpu) return;
        auto* commands = begin();
        if (!commands) return;
        memcpy(staging.cpu, source.data + offset, size_t(size));
        copy_memory(commands, gpu_range(staging), {.gpu = static_cast<byte*>(destination.gpu) + offset, .size = size});
        ++state_.operation_count;
        offset += size;
    }
]====])

replace_exact(utility/src/upload_queue.cpp
[====[{
    assert(source.data);
    const GpuCpuRange<byte> staging = reserve(source.size);
    memcpy(staging.cpu, source.data, source.size);
    copy_memory_to_texture(begin(), gpu_range(staging), destination, copy);
    ++state_.operation_count;
}

]====]
[====[{
    assert(source.data);
    const GpuCpuRange<byte> staging = reserve(source.size);
    if (!staging.cpu) return;
    auto* commands = begin();
    if (!commands) return;
    memcpy(staging.cpu, source.data, source.size);
    copy_memory_to_texture(commands, gpu_range(staging), destination, copy);
    ++state_.operation_count;
}

]====])

replace_exact(utility/src/upload_queue.cpp
[====[{
    assert(state_.upload_stages != Stage::transfer && "compute uploads require a general or compute queue");
    const GpuCpuRange<byte> staging = reserve(byte_size);
    barrier(begin(), Stage::transfer | Stage::compute, Access::transfer_write | Access::shader_write,
            Stage::compute, Access::shader_read | Access::shader_write | Access::descriptor_read);
    state_.in_callback = true;
    return staging;
]====]
[====[{
    assert(state_.upload_stages != Stage::transfer && "compute uploads require a general or compute queue");
    const GpuCpuRange<byte> staging = reserve(byte_size);
    if (!staging.cpu) return {};
    auto* commands = begin();
    if (!commands) return {};
    barrier(commands, Stage::transfer | Stage::compute, Access::transfer_write | Access::shader_write,
            Stage::compute, Access::shader_read | Access::shader_write | Access::descriptor_read);
    state_.in_callback = true;
    return staging;
]====])

replace_exact(utility/src/upload_queue.cpp
[====[
void UploadQueue::write_timestamp(uint64* cpu_destination) noexcept
{
    gpu::write_timestamp(begin(), cpu_destination);
}

TimelinePoint UploadQueue::flush() noexcept
{
    State& state = state_;
    assert(!state.in_callback);
    if (!state.commands) return state.completion;
    reclaim();
    barrier(state.commands, state.upload_stages, state.upload_access, Stage::all_commands, state.queue_access);
    end_commands(state.commands);
    ++state.completion.value;
    submit(state.device, {.commands = {state.commands}, .completion = state.completion}, state.queue_index);
    Batch& batch = state.batches[(state.retirement_first + state.retirement_count) % state.max_pending_batches];
    batch.end = state.head;
    batch.value = state.completion.value;
]====]
[====[
void UploadQueue::write_timestamp(uint64* cpu_destination) noexcept
{
    if (auto* commands = begin()) gpu::write_timestamp(commands, cpu_destination);
}

TimelinePoint UploadQueue::flush() noexcept
{
    State& state = state_;
    assert(!state.in_callback);
    if (error || !state.commands) return state.completion;
    reclaim();
    if (error) return state.completion;
    barrier(state.commands, state.upload_stages, state.upload_access, Stage::all_commands, state.queue_access);
    error = end_commands(state.commands);
    if (error) return state.completion;
    const TimelinePoint completion{state.completion.semaphore, state.completion.value + 1};
    const auto submitted = submit(state.device, {.commands = {state.commands}, .completion = completion}, state.queue_index);
    if (!submitted.submitted) { error = submitted.error; return state.completion; }
    state.completion = completion;
    Batch& batch = state.batches[(state.retirement_first + state.retirement_count) % state.max_pending_batches];
    batch.end = state.head;
    batch.value = state.completion.value;
]====])

replace_exact(utility/include/NoGraphicsAPIUtility/upload_queue.hpp
[====[    void upload_with_compute(uint64 byte_size, Callback&& callback) noexcept
    {
        const GpuCpuRange<byte> staging = begin_compute(byte_size);
        static_assert(noexcept(callback(state_.commands, staging)));
        callback(state_.commands, staging);
        end_compute();
]====]
[====[    void upload_with_compute(uint64 byte_size, Callback&& callback) noexcept
    {
        const GpuCpuRange<byte> staging = begin_compute(byte_size);
        if (!staging.cpu) return;
        static_assert(noexcept(callback(state_.commands, staging)));
        callback(state_.commands, staging);
        end_compute();
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NDEBUG)
]====]
[====[#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <NoGraphicsAPI/backend_allocation.hpp>
#include <string.h>

#if defined(NDEBUG)
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
namespace
{
constexpr MTLRenderStages render_stages = MTLRenderStageVertex | MTLRenderStageFragment | MTLRenderStageObject | MTLRenderStageMesh;

uint64 align_up(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }
]====]
[====[{
namespace
{
// Test builds can return nil at native command-resource allocation sites too.
// The production path has no additional allocation or bookkeeping.
template<typename Create>
auto command_resource(Create create) noexcept -> decltype(create())
{
#if defined(WOBY_BACKEND_ALLOCATION_TESTS)
    if (!detail::backend_allocation_allowed()) return nil;
#endif
    return create();
}

constexpr MTLRenderStages render_stages = MTLRenderStageVertex | MTLRenderStageFragment | MTLRenderStageObject | MTLRenderStageMesh;

uint64 align_up(uint64 value, uint64 alignment) { return (value + alignment - 1) / alignment * alignment; }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    bool ended = false;
    bool render_continuation = false;
    bool acquired = false;
    const PSO* pso = nullptr;
};

]====]
[====[    bool ended = false;
    bool render_continuation = false;
    bool acquired = false;
    const char* error = nullptr;
    const PSO* pso = nullptr;
};

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    CommandBuffer* last = nullptr;
    CommandBuffer* next_buffer = nullptr;
    DepthStateChunk* depth_states = nullptr;
};

struct Device
]====]
[====[    CommandBuffer* last = nullptr;
    CommandBuffer* next_buffer = nullptr;
    DepthStateChunk* depth_states = nullptr;
    const char* error = nullptr;
};

struct Device
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    uint32 counter_page_count = 0;
    uint32 timestamp_query_count = 0;
    bool shader_validation = false;
};

namespace
]====]
[====[    uint32 counter_page_count = 0;
    uint32 timestamp_query_count = 0;
    bool shader_validation = false;
    const char* error = nullptr;
};

namespace
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[            continue;
        }
        NSError* error = nil;
        MTL4CounterHeapDescriptor* counters = [MTL4CounterHeapDescriptor new];
        counters.type = MTL4CounterHeapTypeTimestamp;
        counters.count = 4096;
        CounterPage* page = new CounterPage{.heap = [device->metal newCounterHeapWithDescriptor:counters error:&error]};
        [counters release];
        if (!page->heap)
        {
            __atomic_fetch_sub(&device->counter_page_count, 1u, __ATOMIC_RELAXED);
            report_error("timestamp storage", error);
]====]
[====[            continue;
        }
        NSError* error = nil;
        MTL4CounterHeapDescriptor* counters = command_resource([] { return [MTL4CounterHeapDescriptor new]; });
        if (!counters) {
            __atomic_fetch_sub(&device->counter_page_count, 1u, __ATOMIC_RELAXED);
            return false;
        }
        counters.type = MTL4CounterHeapTypeTimestamp;
        counters.count = 4096;
        CounterPage* page = new (detail::BackendAllocation{}) CounterPage{.heap = command_resource([&] { return [device->metal newCounterHeapWithDescriptor:counters error:&error]; })};
        [counters release];
        if (!page || !page->heap)
        {
            __atomic_fetch_sub(&device->counter_page_count, 1u, __ATOMIC_RELAXED);
            report_error("timestamp storage", error);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[CommandBuffer* create_context(CommandPool* pool)
{
    Device* device = pool->device;
    CommandBuffer* result = new CommandBuffer{.device = device, .pool = pool};
    NSError* error = nil;
    result->native_buffers = new NativeCommandBuffer{.buffer = [device->metal newCommandBuffer], .allocator = [device->metal newCommandAllocator]};
    result->commands = result->native_buffers->buffer;
    MTL4ArgumentTableDescriptor* arguments = [MTL4ArgumentTableDescriptor new];
    arguments.maxBufferBindCount = 3;
    arguments.initializeBindings = YES;
    result->arguments = [device->metal newArgumentTableWithDescriptor:arguments error:&error];
    [arguments release];
    result->pass = [MTL4RenderPassDescriptor new];
    if (device->timestamp_query_count)
    {
        result->timestamps = new TimestampSlot[device->timestamp_query_count]{};
        if (!allocate_timestamps(device, result->timestamps))
        {
            destroy_context(device, result);
            return nullptr;
        }
    }
    if (!result->commands || !result->native_buffers->allocator || !result->arguments)
    {
        report_error("command context", error);
        destroy_context(device, result);
]====]
[====[CommandBuffer* create_context(CommandPool* pool)
{
    Device* device = pool->device;
    CommandBuffer* result = new (detail::BackendAllocation{}) CommandBuffer{.device = device, .pool = pool};
    if (!result) return nullptr;
    NSError* error = nil;
    result->native_buffers = new (detail::BackendAllocation{}) NativeCommandBuffer{.buffer = command_resource([&] { return [device->metal newCommandBuffer]; }), .allocator = command_resource([&] { return [device->metal newCommandAllocator]; })};
    if (!result->native_buffers) { destroy_context(device, result); return nullptr; }
    result->commands = result->native_buffers->buffer;
    MTL4ArgumentTableDescriptor* arguments = command_resource([] { return [MTL4ArgumentTableDescriptor new]; });
    if (!arguments) { destroy_context(device, result); return nullptr; }
    arguments.maxBufferBindCount = 3;
    arguments.initializeBindings = YES;
    result->arguments = command_resource([&] { return [device->metal newArgumentTableWithDescriptor:arguments error:&error]; });
    [arguments release];
    result->pass = command_resource([&] { return [MTL4RenderPassDescriptor new]; });
    if (device->timestamp_query_count)
    {
        result->timestamps = detail::backend_array<TimestampSlot>(device->timestamp_query_count);
        if (!result->timestamps || !allocate_timestamps(device, result->timestamps))
        {
            destroy_context(device, result);
            return nullptr;
        }
    }
    if (!result->commands || !result->native_buffers->allocator || !result->arguments || !result->pass)
    {
        report_error("command context", error);
        destroy_context(device, result);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[
id<MTL4CommandBuffer> native_commands(CommandBuffer* commands)
{
    if (!commands->commands)
    {
        if (!commands->native->next)
            commands->native->next = new NativeCommandBuffer{.buffer = [commands->device->metal newCommandBuffer],
                                                           .allocator = [commands->device->metal newCommandAllocator]};
        commands->native = commands->native->next;
        commands->commands = commands->native->buffer;
        ++commands->native_count;
        [commands->commands beginCommandBufferWithAllocator:commands->native->allocator];
]====]
[====[
id<MTL4CommandBuffer> native_commands(CommandBuffer* commands)
{
    if (commands->error) return nil;
    if (!commands->commands)
    {
        if (!commands->native->next)
            commands->native->next = new (detail::BackendAllocation{}) NativeCommandBuffer{.buffer = command_resource([&] { return [commands->device->metal newCommandBuffer]; }),
                                                           .allocator = command_resource([&] { return [commands->device->metal newCommandAllocator]; })};
        auto* next = commands->native->next;
        if (!next || !next->buffer || !next->allocator) {
            if (next) { [next->buffer release]; [next->allocator release]; delete next; commands->native->next = nullptr; }
            commands->error = "Insufficient CPU/GPU memory for a Metal command segment.";
            return nil;
        }
        commands->native = next;
        commands->commands = commands->native->buffer;
        ++commands->native_count;
        [commands->commands beginCommandBufferWithAllocator:commands->native->allocator];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    assert(commands->recording && !commands->render);
    if (!commands->compute)
    {
        commands->compute = [[native_commands(commands) computeCommandEncoder] retain];
        [commands->compute setArgumentTable:commands->arguments];
        if (commands->pso && commands->pso->compute) [commands->compute setComputePipelineState:commands->pso->compute];
    }
]====]
[====[    assert(commands->recording && !commands->render);
    if (!commands->compute)
    {
        commands->compute = [command_resource([&] { return [native_commands(commands) computeCommandEncoder]; }) retain];
        if (!commands->compute) { commands->error = "Cannot allocate a Metal compute encoder."; return nil; }
        [commands->compute setArgumentTable:commands->arguments];
        if (commands->pso && commands->pso->compute) [commands->compute setComputePipelineState:commands->pso->compute];
    }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    }
}

void register_buffer(Device* device, GpuHeapOwner* owner, uint64 base, uint64 size)
{
    uint64 occupied = __atomic_load_n(&device->occupied_buffers, __ATOMIC_RELAXED);
    for (;;)
    {
        assert(occupied != ~uint64{0});
        const uint32 slot = static_cast<uint32>(__builtin_ctzll(~occupied));
        if (__atomic_compare_exchange_n(&device->occupied_buffers, &occupied, occupied | (uint64{1} << slot), true,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
]====]
[====[    }
}

bool register_buffer(Device* device, GpuHeapOwner* owner, uint64 base, uint64 size)
{
    uint64 occupied = __atomic_load_n(&device->occupied_buffers, __ATOMIC_RELAXED);
    for (;;)
    {
        if (occupied == ~uint64{0}) return false;
        const uint32 slot = static_cast<uint32>(__builtin_ctzll(~occupied));
        if (__atomic_compare_exchange_n(&device->occupied_buffers, &occupied, occupied | (uint64{1} << slot), true,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    __atomic_store_n(&owner->record->size, size, __ATOMIC_RELAXED);
    __atomic_store_n(&owner->record->buffer, reinterpret_cast<uintptr>(owner->buffer), __ATOMIC_RELAXED);
    update_buffer_index(device, owner->record, true);
}

void unregister_buffer(Device* device, GpuHeapOwner* owner)
]====]
[====[    __atomic_store_n(&owner->record->size, size, __ATOMIC_RELAXED);
    __atomic_store_n(&owner->record->buffer, reinterpret_cast<uintptr>(owner->buffer), __ATOMIC_RELAXED);
    update_buffer_index(device, owner->record, true);
    return true;
}

void unregister_buffer(Device* device, GpuHeapOwner* owner)
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        Device* device = new Device{};
        // MetalTools on macOS 26.6.2 cannot enumerate placed resources through heap residency.
        const char* shader_validation = getenv("MTL_SHADER_VALIDATION");
        device->shader_validation = shader_validation && atoi(shader_validation) != 0;
]====]
[====[{
    @autoreleasepool
    {
        Device* device = new (detail::BackendAllocation{}) Device{};
        if (!device) return {.error = Error::driver_error};
        // MetalTools on macOS 26.6.2 cannot enumerate placed resources through heap residency.
        const char* shader_validation = getenv("MTL_SHADER_VALIDATION");
        device->shader_validation = shader_validation && atoi(shader_validation) != 0;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        device->caps.general_queue_count = desc.desired_queue_count;
        device->caps.compute_queue_count = desc.desired_compute_queue_count;
        device->caps.copy_queue_count = desc.desired_copy_queue_count;
        device->caps.queue_count = desc.desired_queue_count + desc.desired_compute_queue_count + desc.desired_copy_queue_count;
        device->queues = new Queue[device->caps.queue_count]{};
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
        {
            Queue& queue = device->queues[i];
]====]
[====[        device->caps.general_queue_count = desc.desired_queue_count;
        device->caps.compute_queue_count = desc.desired_compute_queue_count;
        device->caps.copy_queue_count = desc.desired_copy_queue_count;
        const uint64 queue_count = uint64{desc.desired_queue_count} + desc.desired_compute_queue_count + desc.desired_copy_queue_count;
        if (queue_count > UINT32_MAX) { destroy_device(device); return {.error = Error::unsupported}; }
        device->caps.queue_count = static_cast<uint32>(queue_count);
        device->queues = detail::backend_array<Queue>(device->caps.queue_count);
        if (!device->queues) { destroy_device(device); return {.error = Error::driver_error}; }
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
        {
            Queue& queue = device->queues[i];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        if (device->layer) [device->queues[0].queue removeResidencySet:device->layer.residencySet];
        [device->drawable release];
        [device->layer release];
        for (uint32 i = 0; i < device->caps.queue_count; ++i)
        {
            Queue& queue = device->queues[i];
            assert(queue.completion.signaledValue >= queue.submitted_value);
]====]
[====[        if (device->layer) [device->queues[0].queue removeResidencySet:device->layer.residencySet];
        [device->drawable release];
        [device->layer release];
        for (uint32 i = 0; device->queues && i < device->caps.queue_count; ++i)
        {
            Queue& queue = device->queues[i];
            assert(queue.completion.signaledValue >= queue.submitted_value);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        TimelineSemaphore* result = new TimelineSemaphore{.device = device, .event = [device->metal newSharedEvent]};
        if (!result->event) { report_error("timeline", nil); delete result; return nullptr; }
        result->event.signaledValue = initial_value;
        return result;
]====]
[====[{
    @autoreleasepool
    {
        TimelineSemaphore* result = new (detail::BackendAllocation{}) TimelineSemaphore{.device = device, .event = [device->metal newSharedEvent]};
        if (!result) return nullptr;
        if (!result->event) { report_error("timeline", nil); delete result; return nullptr; }
        result->event.signaledValue = initial_value;
        return result;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        desc.type = MTLHeapTypePlacement;
        desc.size = align_up(requirements.size, requirements.align);
        desc.resourceOptions = options;
        GpuHeapOwner* owner = new GpuHeapOwner{.device = device, .heap = [device->metal newHeapWithDescriptor:desc]};
        [desc release];
        owner->buffer = [owner->heap newBufferWithLength:byte_count options:options offset:0];
        if (!owner->buffer)
        {
]====]
[====[        desc.type = MTLHeapTypePlacement;
        desc.size = align_up(requirements.size, requirements.align);
        desc.resourceOptions = options;
        GpuHeapOwner* owner = new (detail::BackendAllocation{}) GpuHeapOwner{.device = device, .heap = [device->metal newHeapWithDescriptor:desc]};
        [desc release];
        if (!owner) return {.error = "Insufficient CPU memory for Metal buffer ownership."};
        owner->buffer = [owner->heap newBufferWithLength:byte_count options:options offset:0];
        if (!owner->buffer)
        {
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[            delete owner;
            return {};
        }
        register_buffer(device, owner, owner->buffer.gpuAddress, byte_count);
        add_resident(device, device->shader_validation ? (id<MTLAllocation>)owner->buffer : (id<MTLAllocation>)owner->heap);
        return {.range = {.cpu = memory == MemoryType::gpu_only ? nullptr : static_cast<byte*>(owner->buffer.contents),
                          .gpu = reinterpret_cast<byte*>(owner->buffer.gpuAddress), .size = byte_count},
]====]
[====[            delete owner;
            return {};
        }
        if (!register_buffer(device, owner, owner->buffer.gpuAddress, byte_count)) {
            [owner->buffer release]; [owner->heap release]; delete owner;
            return {.error = "Metal buffer registration capacity exhausted."};
        }
        add_resident(device, device->shader_validation ? (id<MTLAllocation>)owner->buffer : (id<MTLAllocation>)owner->heap);
        return {.range = {.cpu = memory == MemoryType::gpu_only ? nullptr : static_cast<byte*>(owner->buffer.contents),
                          .gpu = reinterpret_cast<byte*>(owner->buffer.gpuAddress), .size = byte_count},
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        desc.size = byte_count;
        desc.storageMode = MTLStorageModePrivate;
        desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        TextureHeapOwner* owner = new TextureHeapOwner{.device = device, .heap = [device->metal newHeapWithDescriptor:desc]};
        [desc release];
        if (!owner->heap) { report_error("texture heap", nil); delete owner; return {}; }
        if (!device->shader_validation) add_resident(device, owner->heap);
        return {.size = byte_count, .owner = owner};
    }
]====]
[====[        desc.size = byte_count;
        desc.storageMode = MTLStorageModePrivate;
        desc.hazardTrackingMode = MTLHazardTrackingModeUntracked;
        TextureHeapOwner* owner = new (detail::BackendAllocation{}) TextureHeapOwner{.device = device, .heap = [device->metal newHeapWithDescriptor:desc]};
        [desc release];
        if (!owner || !owner->heap) { report_error("texture heap", nil); delete owner; return {}; }
        if (!device->shader_validation) add_resident(device, owner->heap);
        return {.size = byte_count, .owner = owner};
    }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    {
        if (!supports_texture_format(device, desc.format, desc.usage)) return {};
        MTLTextureDescriptor* descriptor = texture_descriptor(desc);
        const MTLSizeAndAlign result = [device->metal heapTextureSizeAndAlignWithDescriptor:descriptor];
        [descriptor release];
        assert(device->caps.texture_heap_alignment % result.align == 0);
        return {.size = result.size, .align = result.align};
    }
]====]
[====[    {
        if (!supports_texture_format(device, desc.format, desc.usage)) return {};
        MTLTextureDescriptor* descriptor = texture_descriptor(desc);
        if (!descriptor) return {};
        const MTLSizeAndAlign result = [device->metal heapTextureSizeAndAlignWithDescriptor:descriptor];
        [descriptor release];
        if (!result.align || !result.size) return {};
        assert(device->caps.texture_heap_alignment % result.align == 0);
        return {.size = result.size, .align = result.align};
    }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        id<MTLTexture> texture = [heap.owner->heap newTextureWithDescriptor:descriptor offset:offset];
        [descriptor release];
        if (!texture) { report_error("texture", nil); return nullptr; }
        if (device->shader_validation) add_resident(device, texture);
        return new Texture{.device = device, .texture = texture, .desc = desc};
    }
}

]====]
[====[        id<MTLTexture> texture = [heap.owner->heap newTextureWithDescriptor:descriptor offset:offset];
        [descriptor release];
        if (!texture) { report_error("texture", nil); return nullptr; }
        auto* result = new (detail::BackendAllocation{}) Texture{.device = device, .texture = texture, .desc = desc};
        if (!result) { [texture release]; return nullptr; }
        if (device->shader_validation) add_resident(device, texture);
        return result;
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        return new RenderView{.texture = [texture->texture retain], .mip = desc.mip_level, .slice = desc.slice};
    }
}

]====]
[====[{
    @autoreleasepool
    {
        return new (detail::BackendAllocation{}) RenderView{.texture = [texture->texture retain], .mip = desc.mip_level, .slice = desc.slice};
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        NSError* error = nil;
        MTLResourceViewPoolDescriptor* desc = [MTLResourceViewPoolDescriptor new];
        desc.resourceViewCount = capacity;
        TextureDescriptorHeap* result = new TextureDescriptorHeap{.device = device, .capacity = capacity};
        result->pool = [device->metal newTextureViewPoolWithDescriptor:desc error:&error];
        [desc release];
        if (!result->pool) { report_error("texture descriptor heap", error); delete result; return nullptr; }
]====]
[====[        NSError* error = nil;
        MTLResourceViewPoolDescriptor* desc = [MTLResourceViewPoolDescriptor new];
        desc.resourceViewCount = capacity;
        TextureDescriptorHeap* result = new (detail::BackendAllocation{}) TextureDescriptorHeap{.device = device, .capacity = capacity};
        if (!result) { [desc release]; return nullptr; }
        result->pool = [device->metal newTextureViewPoolWithDescriptor:desc error:&error];
        [desc release];
        if (!result->pool) { report_error("texture descriptor heap", error); delete result; return nullptr; }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        view.levelRange = NSMakeRange(desc.base_mip, desc.mip_count ? desc.mip_count : texture->desc.mip_levels - desc.base_mip);
        view.sliceRange = NSMakeRange(desc.base_layer, desc.layer_count ? desc.layer_count : texture->desc.layer_count - desc.base_layer);
        const MTLResourceID resource = [heap->pool setTextureView:texture->texture descriptor:view atIndex:index];
        assert(resource._impl == heap->pool.baseResourceID._impl + index);
        [view release];
    }
}
]====]
[====[        view.levelRange = NSMakeRange(desc.base_mip, desc.mip_count ? desc.mip_count : texture->desc.mip_levels - desc.base_mip);
        view.sliceRange = NSMakeRange(desc.base_layer, desc.layer_count ? desc.layer_count : texture->desc.layer_count - desc.base_layer);
        const MTLResourceID resource = [heap->pool setTextureView:texture->texture descriptor:view atIndex:index];
        if (resource._impl != heap->pool.baseResourceID._impl + index)
            heap->device->error = "Cannot allocate a Metal texture descriptor.";
        [view release];
    }
}
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        SamplerDescriptorHeap* result = new SamplerDescriptorHeap{.device = device, .capacity = capacity};
        result->buffer = [device->metal newBufferWithLength:capacity * sizeof(MTLResourceID) options:MTLResourceStorageModeShared];
        if (!result->buffer) { report_error("sampler heap", nil); delete result; return nullptr; }
        result->samplers = new id<MTLSamplerState>[capacity]{};
        memset(result->buffer.contents, 0, capacity * sizeof(MTLResourceID));
        add_resident(device, result->buffer);
        return result;
]====]
[====[{
    @autoreleasepool
    {
        SamplerDescriptorHeap* result = new (detail::BackendAllocation{}) SamplerDescriptorHeap{.device = device, .capacity = capacity};
        if (!result) return nullptr;
        result->buffer = [device->metal newBufferWithLength:capacity * sizeof(MTLResourceID) options:MTLResourceStorageModeShared];
        if (!result->buffer) { report_error("sampler heap", nil); delete result; return nullptr; }
        result->samplers = detail::backend_array<id<MTLSamplerState>>(capacity);
        if (!result->samplers) { [result->buffer release]; delete result; return nullptr; }
        memset(result->buffer.contents, 0, capacity * sizeof(MTLResourceID));
        add_resident(device, result->buffer);
        return result;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        sampler.supportArgumentBuffers = YES;
        id<MTLSamplerState> state = [heap->device->metal newSamplerStateWithDescriptor:sampler];
        [sampler release];
        [heap->samplers[index] release];
        heap->samplers[index] = state;
        static_cast<MTLResourceID*>(heap->buffer.contents)[index] = state.gpuResourceID;
]====]
[====[        sampler.supportArgumentBuffers = YES;
        id<MTLSamplerState> state = [heap->device->metal newSamplerStateWithDescriptor:sampler];
        [sampler release];
        if (!state) { heap->device->error = "Cannot allocate a Metal sampler state."; return; }
        [heap->samplers[index] release];
        heap->samplers[index] = state;
        static_cast<MTLResourceID*>(heap->buffer.contents)[index] = state.gpuResourceID;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        [fragment release];
        [pipeline release];
        if (!state) { report_error("graphics pipeline", error); return nullptr; }
        add_resident(device, state);
        return new PSO{.device = device, .render = state, .rasterization = desc.rasterization,
            .primitive = desc.topology == PrimitiveTopology::lines ? MTLPrimitiveTypeLine :
                desc.topology == PrimitiveTopology::triangle_strip ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle};
    }
}

]====]
[====[        [fragment release];
        [pipeline release];
        if (!state) { report_error("graphics pipeline", error); return nullptr; }
        auto* result = new (detail::BackendAllocation{}) PSO{.device = device, .render = state, .rasterization = desc.rasterization,
            .primitive = desc.topology == PrimitiveTopology::lines ? MTLPrimitiveTypeLine :
                desc.topology == PrimitiveTopology::triangle_strip ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle};
        if (!result) [state release];
        else add_resident(device, state);
        return result;
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        id<MTLRenderPipelineState> state = [device->compiler newRenderPipelineStateWithDescriptor:pipeline compilerTaskOptions:nil error:&error];
        [task release]; [mesh release]; [fragment release]; [pipeline release];
        if (!state) { report_error("mesh pipeline", error); return nullptr; }
        add_resident(device, state);
        return new PSO{.device = device, .render = state, .rasterization = desc.rasterization,
                       .threads = metal_size(desc.mesh.threadgroup_size), .object_threads = metal_size(desc.task.threadgroup_size), .mesh = true};
    }
}

]====]
[====[        id<MTLRenderPipelineState> state = [device->compiler newRenderPipelineStateWithDescriptor:pipeline compilerTaskOptions:nil error:&error];
        [task release]; [mesh release]; [fragment release]; [pipeline release];
        if (!state) { report_error("mesh pipeline", error); return nullptr; }
        auto* result = new (detail::BackendAllocation{}) PSO{.device = device, .render = state, .rasterization = desc.rasterization,
                       .threads = metal_size(desc.mesh.threadgroup_size), .object_threads = metal_size(desc.task.threadgroup_size), .mesh = true};
        if (!result) [state release];
        else add_resident(device, state);
        return result;
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        [function release];
        [desc release];
        if (!state) { report_error("compute pipeline", error); return nullptr; }
        add_resident(device, state);
        return new PSO{.device = device, .compute = state, .threads = metal_size(stage.threadgroup_size)};
    }
}

]====]
[====[        [function release];
        [desc release];
        if (!state) { report_error("compute pipeline", error); return nullptr; }
        auto* result = new (detail::BackendAllocation{}) PSO{.device = device, .compute = state, .threads = metal_size(stage.threadgroup_size)};
        if (!result) [state release];
        else add_resident(device, state);
        return result;
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    @autoreleasepool
    {
        assert(queue_index < device->caps.queue_count);
        CommandPool* pool = new CommandPool{.device = device, .kind = device->queues[queue_index].kind};
        depth_state(pool, {});
        for (uint32 compare = 0; compare < 8; ++compare)
            for (uint32 write = 0; write < 2; ++write)
                depth_state(pool, {.depth_test = true, .depth_write = write != 0, .depth_compare = static_cast<CompareOp>(compare)});
        return pool;
    }
}

void reset_command_pool(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        for (CommandBuffer* commands = pool->first; commands; commands = commands->next)
        {
            assert(!commands->acquired && (!commands->retirement || commands->retirement.signaledValue >= commands->retirement_value));
            if (commands->render) end_render_pass(commands);
            end_compute(commands);
            if (commands->recording) [commands->commands endCommandBuffer];
            commands->recording = false;
            commands->ended = false;
            commands->retirement = nil;
            commands->retirement_value = 0;
            for (NativeCommandBuffer* native = commands->native_buffers; native; native = native->next) [native->allocator reset];
        }
        pool->next_buffer = pool->first;
    }
}

]====]
[====[    @autoreleasepool
    {
        assert(queue_index < device->caps.queue_count);
        CommandPool* pool = new (detail::BackendAllocation{}) CommandPool{.device = device, .kind = device->queues[queue_index].kind};
        if (!pool) return nullptr;
        depth_state(pool, {});
        for (uint32 compare = 0; compare < 8; ++compare)
            for (uint32 write = 0; write < 2; ++write)
                depth_state(pool, {.depth_test = true, .depth_write = write != 0, .depth_compare = static_cast<CompareOp>(compare)});
        if (pool->error) { destroy_command_pool(pool); return nullptr; }
        return pool;
    }
}

const char* reset_command_pool(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        for (CommandBuffer* commands = pool->first; commands; commands = commands->next)
        {
            assert(!commands->acquired && (!commands->retirement || commands->retirement.signaledValue >= commands->retirement_value));
            if (commands->render) {
                [commands->render endEncoding];
                [commands->render release];
                commands->render = nil;
            }
            end_compute(commands);
            if (commands->recording) [commands->commands endCommandBuffer];
            commands->recording = false;
            commands->ended = false;
            commands->error = nullptr;
            commands->retirement = nil;
            commands->retirement_value = 0;
            for (NativeCommandBuffer* native = commands->native_buffers; native; native = native->next) [native->allocator reset];
        }
        pool->next_buffer = pool->first;
        pool->error = nullptr;
        return nullptr;
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    }
}

CommandBuffer* begin_commands(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        CommandBuffer* context = pool->next_buffer;
        if (context) pool->next_buffer = context->next;
        else
        {
            context = create_context(pool);
            if (!context) return nullptr;
            if (pool->last) pool->last->next = context;
            else pool->first = context;
            pool->last = context;
]====]
[====[    }
}

const char* command_pool_error(const CommandPool* pool) noexcept { return pool->error; }

CommandBuffer* begin_commands(CommandPool* pool) noexcept
{
    @autoreleasepool
    {
        pool->error = pool->device->error;
        if (pool->error) return nullptr;
        CommandBuffer* context = pool->next_buffer;
        if (context) pool->next_buffer = context->next;
        else
        {
            context = create_context(pool);
            if (!context) { pool->error = "Insufficient CPU/GPU memory for Metal command recording."; return nullptr; }
            if (pool->last) pool->last->next = context;
            else pool->first = context;
            pool->last = context;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        [context->arguments setAddress:0 atIndex:1];
        [context->arguments setAddress:0 atIndex:2];
        context->timestamp_count = 0;
        context->pso = nullptr;
        context->recording = true;
        context->render_continuation = false;
]====]
[====[        [context->arguments setAddress:0 atIndex:1];
        [context->arguments setAddress:0 atIndex:2];
        context->timestamp_count = 0;
        context->error = nullptr;
        context->pso = nullptr;
        context->recording = true;
        context->render_continuation = false;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    }
}

void end_commands(CommandBuffer* commands) noexcept
{
    @autoreleasepool
    {
        assert(commands->recording && !commands->render);
        end_compute(commands);
        [commands->commands endCommandBuffer];
        commands->recording = false;
        commands->ended = true;
    }
}

void submit(Device* device, const SubmitDesc& desc, uint32 queue_index) noexcept
{
    @autoreleasepool
    {
        assert(queue_index < device->caps.queue_count && desc.completion.semaphore && desc.completion.semaphore->device == device);
        Queue& queue = device->queues[queue_index];
        size_t count = desc.commands.size;
        for (size_t i = 0; i < desc.commands.size; ++i)
            count += desc.commands.data[i]->native_count - 1;
        if (queue.submission_capacity < count)
        {
            queue.submission_capacity = count;
            queue.submission = static_cast<id<MTL4CommandBuffer>*>(realloc(queue.submission, count * sizeof(id<MTL4CommandBuffer>)));
        }
        for (size_t i = 0, native_index = 0; i < desc.commands.size; ++i)
        {
            CommandBuffer* commands = desc.commands.data[i];
]====]
[====[    }
}

const char* end_commands(CommandBuffer* commands) noexcept
{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error)
            return commands->error ? commands->error : commands->pool->error ? commands->pool->error : commands->device->error;
        assert(commands->recording && !commands->render);
        end_compute(commands);
        [commands->commands endCommandBuffer];
        commands->recording = false;
        commands->ended = true;
        return nullptr;
    }
}

SubmitResult submit(Device* device, const SubmitDesc& desc, uint32 queue_index) noexcept
{
    @autoreleasepool
    {
        assert(queue_index < device->caps.queue_count && desc.completion.semaphore && desc.completion.semaphore->device == device);
        if (device->error) return {device->error};
        Queue& queue = device->queues[queue_index];
        size_t count = desc.commands.size;
        for (size_t i = 0; i < desc.commands.size; ++i) {
            const auto* commands = desc.commands.data[i];
            if (commands->error || commands->pool->error) return {commands->error ? commands->error : commands->pool->error};
            if (!commands->ended) return {"Cannot submit unfinished Metal command recording."};
            if (commands->native_count - 1 > SIZE_MAX - count) return {"Metal submission size overflow."};
            count += commands->native_count - 1;
        }
        if (!detail::reserve_backend_array(queue.submission, queue.submission_capacity, count))
            return {"Insufficient CPU memory for Metal submission bookkeeping."};
        // All fallible preparation precedes queue operations, including drawable waits.
        bool presents = false;
        for (size_t i = 0; i < desc.commands.size; ++i) presents |= desc.commands.data[i] == device->acquired;
        if (presents) [queue.queue waitForDrawable:device->drawable];
        for (size_t i = 0, native_index = 0; i < desc.commands.size; ++i)
        {
            CommandBuffer* commands = desc.commands.data[i];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        if (device->shader_validation) os_unfair_lock_unlock(&device->residency_lock);
        [queue.queue signalEvent:queue.completion value:++queue.submitted_value];
        [queue.queue signalEvent:desc.completion.semaphore->event value:desc.completion.value];
    }
}

]====]
[====[        if (device->shader_validation) os_unfair_lock_unlock(&device->residency_lock);
        [queue.queue signalEvent:queue.completion value:++queue.submitted_value];
        [queue.queue signalEvent:desc.completion.semaphore->event value:desc.completion.value];
        return {nullptr, true};
    }
}

]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[    }
}

void submit_and_present(Device* device, const SubmitDesc& desc) noexcept
{
    @autoreleasepool
    {
]====]
[====[    }
}

SubmitResult submit_and_present(Device* device, const SubmitDesc& desc) noexcept
{
    @autoreleasepool
    {
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        bool found = false;
        for (size_t i = 0; i < desc.commands.size; ++i) found |= desc.commands.data[i] == device->acquired;
        assert(found);
        [device->queues[0].queue waitForDrawable:device->drawable];
        submit(device, desc, 0);
        [device->queues[0].queue signalDrawable:device->drawable];
        [device->drawable present];
        [device->drawable release];
        device->acquired->acquired = false;
        device->acquired = nullptr;
        device->drawable = nil;
        device->drawable_view = {};
    }
]====]
[====[        bool found = false;
        for (size_t i = 0; i < desc.commands.size; ++i) found |= desc.commands.data[i] == device->acquired;
        assert(found);
        const auto result = submit(device, desc, 0);
        if (!result.submitted) return result;
        [device->queues[0].queue signalDrawable:device->drawable];
        [device->drawable present];
        [device->drawable release];
        device->acquired->acquired = false;
        device->acquired = nullptr;
        device->drawable = nil;
        device->drawable_view = {};
        return result;
    }
}

void abandon_acquired_image(Device* device) noexcept
{
    if (!device || !device->acquired) return;
    @autoreleasepool {
        device->acquired->acquired = false;
        device->acquired = nullptr;
        [device->drawable release];
        device->drawable = nil;
        device->drawable_view = {};
    }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording && heap->device == commands->device);
        [commands->arguments setAddress:heap->base.gpuAddress atIndex:1];
    }
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording && heap->device == commands->device);
        [commands->arguments setAddress:heap->base.gpuAddress atIndex:1];
    }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording && heap->device == commands->device);
        [commands->arguments setAddress:heap->buffer.gpuAddress atIndex:2];
    }
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording && heap->device == commands->device);
        [commands->arguments setAddress:heap->buffer.gpuAddress atIndex:2];
    }
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(source.size <= destination.size);
        uint64 source_offset = 0;
        uint64 destination_offset = 0;
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(source.size <= destination.size);
        uint64 source_offset = 0;
        uint64 destination_offset = 0;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        const TextureCopy copy = texture_copy(destination, desc);
        assert(source.size >= copy.size);
        uint64 offset = 0;
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        const TextureCopy copy = texture_copy(destination, desc);
        assert(source.size >= copy.size);
        uint64 offset = 0;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        const TextureCopy copy = texture_copy(source, desc);
        assert(destination.size >= copy.size);
        uint64 offset = 0;
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        const TextureCopy copy = texture_copy(source, desc);
        assert(destination.size >= copy.size);
        uint64 offset = 0;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(!commands->render);
        (void)after_access;
        const MTLStages source = barrier_stages(before, true);
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(!commands->render);
        (void)after_access;
        const MTLStages source = barrier_stages(before, true);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        if (!commands->device->timestamp_query_count) return;
        assert(commands->recording && cpu_destination && commands->timestamp_count < commands->device->timestamp_query_count);
        const uint32 index = commands->timestamp_count++;
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        if (!commands->device->timestamp_query_count) return;
        assert(commands->recording && cpu_destination && commands->timestamp_count < commands->device->timestamp_query_count);
        const uint32 index = commands->timestamp_count++;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        if ((*chunk)->count < 64) break;
        chunk = &(*chunk)->next;
    }
    if (!*chunk) *chunk = new DepthStateChunk{};
    MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
    desc.depthCompareFunction = state.depth_test ? static_cast<MTLCompareFunction>(state.depth_compare) : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = state.depth_write && state.depth_test;
    if (state.stencil_test)
    {
        MTLStencilDescriptor* front = [MTLStencilDescriptor new];
        MTLStencilDescriptor* back = [MTLStencilDescriptor new];
        stencil_descriptor(front, state.front, state);
        stencil_descriptor(back, state.back, state);
        desc.frontFaceStencil = front;
        desc.backFaceStencil = back;
        [front release]; [back release];
    }
    id<MTLDepthStencilState> result = [device->metal newDepthStencilStateWithDescriptor:desc];
    [desc release];
    (*chunk)->entries[(*chunk)->count++] = {.desc = state, .state = result};
    return result;
}
]====]
[====[        if ((*chunk)->count < 64) break;
        chunk = &(*chunk)->next;
    }
    if (pool->error) return nil;
    if (!*chunk) *chunk = new (detail::BackendAllocation{}) DepthStateChunk{};
    if (!*chunk) { pool->error = "Insufficient CPU memory for Metal depth state bookkeeping."; return nil; }
    MTLDepthStencilDescriptor* desc = command_resource([] { return [MTLDepthStencilDescriptor new]; });
    if (!desc) { pool->error = "Cannot allocate a Metal depth descriptor."; return nil; }
    desc.depthCompareFunction = state.depth_test ? static_cast<MTLCompareFunction>(state.depth_compare) : MTLCompareFunctionAlways;
    desc.depthWriteEnabled = state.depth_write && state.depth_test;
    if (state.stencil_test)
    {
        MTLStencilDescriptor* front = command_resource([] { return [MTLStencilDescriptor new]; });
        MTLStencilDescriptor* back = command_resource([] { return [MTLStencilDescriptor new]; });
        if (!front || !back) {
            [front release]; [back release]; [desc release];
            pool->error = "Cannot allocate a Metal stencil descriptor.";
            return nil;
        }
        stencil_descriptor(front, state.front, state);
        stencil_descriptor(back, state.back, state);
        desc.frontFaceStencil = front;
        desc.backFaceStencil = back;
        [front release]; [back release];
    }
    id<MTLDepthStencilState> result = command_resource([&] { return [device->metal newDepthStencilStateWithDescriptor:desc]; });
    [desc release];
    if (!result) { pool->error = "Cannot allocate a Metal depth state."; return nil; }
    (*chunk)->entries[(*chunk)->count++] = {.desc = state, .state = result};
    return result;
}
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording && !commands->render && commands->pool->kind == QueueKind::general && desc.colors.size <= 8);
        end_compute(commands);
        RenderView* area = desc.colors.size ? desc.colors.data[0].render_view :
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording && !commands->render && commands->pool->kind == QueueKind::general && desc.colors.size <= 8);
        end_compute(commands);
        RenderView* area = desc.colors.size ? desc.colors.data[0].render_view :
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[        MTL4RenderEncoderOptions options = (static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::suspending)) != 0 ?
                                          MTL4RenderEncoderOptionSuspending : MTL4RenderEncoderOptionNone;
        if ((static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::resuming)) != 0) options |= MTL4RenderEncoderOptionResuming;
        commands->render = [[native_commands(commands) renderCommandEncoderWithDescriptor:commands->pass options:options] retain];
        [commands->render setArgumentTable:commands->arguments atStages:render_stages];
        [commands->render setFrontFacingWinding:MTLWindingCounterClockwise];
        [commands->render setViewport:MTLViewport{0, double(height ? height : 1), double(width ? width : 1), -double(height ? height : 1), 0, 1}];
]====]
[====[        MTL4RenderEncoderOptions options = (static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::suspending)) != 0 ?
                                          MTL4RenderEncoderOptionSuspending : MTL4RenderEncoderOptionNone;
        if ((static_cast<uint32>(flags) & static_cast<uint32>(RenderingFlags::resuming)) != 0) options |= MTL4RenderEncoderOptionResuming;
        commands->render = [command_resource([&] { return [native_commands(commands) renderCommandEncoderWithDescriptor:commands->pass options:options]; }) retain];
        if (!commands->render) { commands->error = "Cannot allocate a Metal render encoder."; return; }
        [commands->render setArgumentTable:commands->arguments atStages:render_stages];
        [commands->render setFrontFacingWinding:MTLWindingCounterClockwise];
        [commands->render setViewport:MTLViewport{0, double(height ? height : 1), double(width ? width : 1), -double(height ? height : 1), 0, 1}];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render);
        [commands->render endEncoding];
        [commands->render release];
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render);
        [commands->render endEncoding];
        [commands->render release];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording);
        if (!commands->render) return;
        [commands->render setViewport:MTLViewport{viewport.x, double(viewport.y) + viewport.height, viewport.width, -double(viewport.height),
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording);
        if (!commands->render) return;
        [commands->render setViewport:MTLViewport{viewport.x, double(viewport.y) + viewport.height, viewport.width, -double(viewport.height),
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording);
        if (!commands->render) return;
        [commands->render setScissorRect:MTLScissorRect{static_cast<NSUInteger>(scissor.x), static_cast<NSUInteger>(scissor.y), scissor.width, scissor.height}];
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording);
        if (!commands->render) return;
        [commands->render setScissorRect:MTLScissorRect{static_cast<NSUInteger>(scissor.x), static_cast<NSUInteger>(scissor.y), scissor.width, scissor.height}];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording);
        id<MTLDepthStencilState> depth = depth_state(commands->pool, state);
        if (!commands->render) return;
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording);
        id<MTLDepthStencilState> depth = depth_state(commands->pool, state);
        if (!commands->render) return;
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->recording && pso->device == commands->device);
        commands->pso = pso;
        if (pso->compute)
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->recording && pso->device == commands->device);
        commands->pso = pso;
        if (pso->compute)
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        [commands->render drawPrimitives:commands->pso->primitive vertexStart:first_vertex vertexCount:vertex_count instanceCount:instance_count
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        [commands->render drawPrimitives:commands->pso->primitive vertexStart:first_vertex vertexCount:vertex_count instanceCount:instance_count
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        const uint64 offset = first_index * (type == IndexType::uint16 ? 2u : 4u);
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        const uint64 offset = first_index * (type == IndexType::uint16 ? 2u : 4u);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDrawPrimitivesIndirectArguments);
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDrawPrimitivesIndirectArguments);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDrawIndexedPrimitivesIndirectArguments);
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render && commands->pso && !commands->pso->mesh);
        root_data(commands, root);
        if (!stride) stride = sizeof(MTLDrawIndexedPrimitivesIndirectArguments);
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->pso && commands->pso->compute);
        root_data(commands, root);
        [compute_encoder(commands) dispatchThreadgroups:metal_size(group_count) threadsPerThreadgroup:commands->pso->threads];
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->pso && commands->pso->compute);
        root_data(commands, root);
        [compute_encoder(commands) dispatchThreadgroups:metal_size(group_count) threadsPerThreadgroup:commands->pso->threads];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->pso && commands->pso->compute);
        root_data(commands, root);
        [compute_encoder(commands) dispatchThreadgroupsWithIndirectBuffer:reinterpret_cast<uintptr>(arguments.gpu) threadsPerThreadgroup:commands->pso->threads];
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->pso && commands->pso->compute);
        root_data(commands, root);
        [compute_encoder(commands) dispatchThreadgroupsWithIndirectBuffer:reinterpret_cast<uintptr>(arguments.gpu) threadsPerThreadgroup:commands->pso->threads];
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && commands->pso->mesh);
        root_data(commands, root);
        [commands->render drawMeshThreadgroups:metal_size(group_count) threadsPerObjectThreadgroup:commands->pso->object_threads
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render && commands->pso && commands->pso->mesh);
        root_data(commands, root);
        [commands->render drawMeshThreadgroups:metal_size(group_count) threadsPerObjectThreadgroup:commands->pso->object_threads
]====])

replace_exact(src/NoGraphicsAPIMetal.mm
[====[{
    @autoreleasepool
    {
        assert(commands->render && commands->pso && commands->pso->mesh);
        assert(commands->device->caps.indirect_mesh_draw);
        root_data(commands, root);
]====]
[====[{
    @autoreleasepool
    {
        if (commands->error || commands->pool->error || commands->device->error) return;
        assert(commands->render && commands->pso && commands->pso->mesh);
        assert(commands->device->caps.indirect_mesh_draw);
        root_data(commands, root);
]====])

replace_exact(utility/src/texture_upload.cpp
[====[                const uint64 packed_row_bytes = uint64(part_columns) * format.bytes_per_block;
                const uint64 packed_slice_bytes = packed_row_bytes * part_rows;
                const GpuCpuRange<byte> staging = queue.reserve(packed_slice_bytes * part_slices);
                if (part_columns == columns && part_rows == rows)
                {
                    memcpy(staging.cpu, source.data + slice * slice_bytes, size_t(staging.size));
]====]
[====[                const uint64 packed_row_bytes = uint64(part_columns) * format.bytes_per_block;
                const uint64 packed_slice_bytes = packed_row_bytes * part_rows;
                const GpuCpuRange<byte> staging = queue.reserve(packed_slice_bytes * part_slices);
                if (!staging.cpu) return;
                auto* commands = queue.begin();
                if (!commands) return;
                if (part_columns == columns && part_rows == rows)
                {
                    memcpy(staging.cpu, source.data + slice * slice_bytes, size_t(staging.size));
]====])

replace_exact(utility/src/texture_upload.cpp
[====[                        }
                    }
                }
                copy_memory_to_texture(queue.begin(), gpu_range(staging), destination, part);
                ++queue.state_.operation_count;
            }
        }
]====]
[====[                        }
                    }
                }
                copy_memory_to_texture(commands, gpu_range(staging), destination, part);
                ++queue.state_.operation_count;
            }
        }
]====])
