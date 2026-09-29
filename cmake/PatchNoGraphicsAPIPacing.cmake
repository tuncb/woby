# Optional presentation support against the pinned private dependency. Include
# after PatchNoGraphicsAPI.cmake, which defines replace_exact(). This exposes
# compatible mailbox presentation without choosing a pacing policy for Woby.
# FIFO remains the initial/default mode; the caller must pace mailbox frames.

replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[==[    uint32 desired_swapchain_image_count = 2; // Vulkan: 1..8 presentation contexts. Metal clamps to 2..3 drawables.]==]
[==[    uint32 desired_swapchain_image_count = 2; // Vulkan: 1..8 presentation contexts. Metal clamps to 2..3 drawables.
    // Opt in to declaring compatible mailbox support. Ignored for headless/Metal.
    bool allow_mailbox_presentation = false;]==])

replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[==[[[nodiscard]] uint32x2 get_drawable_extent(Device* device) noexcept;]==]
[==[[[nodiscard]] uint32x2 get_drawable_extent(Device* device) noexcept;
// Optional Vulkan FIFO/mailbox switching, externally synchronized with presentation.
// Query the current swapchain after acquire, since recreation can change support.
// False for headless/Metal/unsupported surfaces. Unsupported requests remain FIFO.
[[nodiscard]] bool supports_mailbox_presentation(Device* device) noexcept;
void set_mailbox_presentation(Device* device, bool enabled) noexcept;]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    uint32 present_context_count = 0;
    uint32 next_present_context = 0;]==]
[==[    uint32 present_context_count = 0;
    uint32 next_present_context = 0;
    bool allow_mailbox_presentation = false;]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    state->present_context_count = presentation ? desc.desired_swapchain_image_count : 0;]==]
[==[    state->present_context_count = presentation ? desc.desired_swapchain_image_count : 0;
    state->allow_mailbox_presentation = presentation && desc.allow_mailbox_presentation;]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    bool acquired = false;
    bool recreate_required = false;]==]
[==[    bool acquired = false;
    bool recreate_required = false;
    bool mailbox_compatible = false;
    bool mailbox_enabled = false;]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    VkSurfaceCapabilities2KHR capabilities_info{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
    };
    Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilities2KHR(device.physical_device, &surface_info, &capabilities_info));]==]
[==[    // A truncated compatibility list is safe: only modes actually returned
    // are considered, and missing mailbox support simply keeps FIFO.
    VkPresentModeKHR compatible_modes[16]{};
    VkSurfacePresentModeCompatibilityKHR compatibility{
        .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR,
        .presentModeCount = 16,
        .pPresentModes = compatible_modes,
    };
    VkSurfaceCapabilities2KHR capabilities_info{
        .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
        .pNext = device.allow_mailbox_presentation ? &compatibility : nullptr,
    };
    Error error = error_from_vk(vkGetPhysicalDeviceSurfaceCapabilities2KHR(device.physical_device, &surface_info, &capabilities_info));]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    uint32 requested_image_count = device.present_context_count;
    if (requested_image_count < capabilities.minImageCount) requested_image_count = capabilities.minImageCount;
    if (capabilities.maxImageCount != 0 && requested_image_count > capabilities.maxImageCount) requested_image_count = capabilities.maxImageCount;
    if (requested_image_count == 0 || requested_image_count > max_swapchain_images) return Error::unsupported;

    const VkCompositeAlphaFlagBitsKHR composite_alpha = choose_composite_alpha(capabilities.supportedCompositeAlpha);
    const VkSwapchainKHR old_handle = swapchain.handle;
    const VkSwapchainPresentModesCreateInfoKHR present_modes_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .presentModeCount = 1,
        .pPresentModes = &swapchain_present_mode,
    };]==]
[==[    uint32 minimum_image_count = capabilities.minImageCount;
    uint32 maximum_image_count = capabilities.maxImageCount;
    const VkCompositeAlphaFlagBitsKHR composite_alpha = choose_composite_alpha(capabilities.supportedCompositeAlpha);
    bool mailbox_compatible = false;
    for (uint32 index = 0; device.allow_mailbox_presentation && index < std::min(compatibility.presentModeCount, 16u); ++index)
        mailbox_compatible |= compatible_modes[index] == VK_PRESENT_MODE_MAILBOX_KHR;
    if (mailbox_compatible)
    {
        const VkSurfacePresentModeKHR mailbox_mode_info{
            .sType = VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR,
            .presentMode = VK_PRESENT_MODE_MAILBOX_KHR,
        };
        const VkPhysicalDeviceSurfaceInfo2KHR mailbox_surface_info{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,
            .pNext = &mailbox_mode_info,
            .surface = device.surface,
        };
        VkSurfaceCapabilities2KHR mailbox_capabilities_info{
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
        };
        const VkResult mailbox_result = vkGetPhysicalDeviceSurfaceCapabilities2KHR(
            device.physical_device, &mailbox_surface_info, &mailbox_capabilities_info);
        mailbox_compatible = mailbox_result == VK_SUCCESS;
        if (mailbox_compatible)
        {
            const VkSurfaceCapabilitiesKHR& mailbox_capabilities = mailbox_capabilities_info.surfaceCapabilities;
            const uint32 combined_minimum = std::max(minimum_image_count, mailbox_capabilities.minImageCount);
            uint32 combined_maximum = maximum_image_count;
            if (mailbox_capabilities.maxImageCount != 0 &&
                (combined_maximum == 0 || mailbox_capabilities.maxImageCount < combined_maximum))
                combined_maximum = mailbox_capabilities.maxImageCount;
            const VkExtent2D mailbox_extent = surface_extent(device, mailbox_capabilities);
            mailbox_compatible = combined_minimum != 0 && combined_minimum <= max_swapchain_images &&
                (combined_maximum == 0 || combined_minimum <= combined_maximum) &&
                mailbox_extent.width == extent.width && mailbox_extent.height == extent.height &&
                (mailbox_capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
                (mailbox_capabilities.supportedTransforms & capabilities.currentTransform) != 0 &&
                (mailbox_capabilities.supportedCompositeAlpha & composite_alpha) != 0;
            if (mailbox_compatible)
            {
                // Every declared mode must be usable with this image count.
                minimum_image_count = combined_minimum;
                maximum_image_count = combined_maximum;
            }
        }
    }
    uint32 requested_image_count = std::max(device.present_context_count, minimum_image_count);
    if (maximum_image_count != 0 && requested_image_count > maximum_image_count) requested_image_count = maximum_image_count;
    if (requested_image_count == 0 || requested_image_count < minimum_image_count || requested_image_count > max_swapchain_images)
        return Error::unsupported;

    const VkSwapchainKHR old_handle = swapchain.handle;
    const VkPresentModeKHR present_modes[]{swapchain_present_mode, VK_PRESENT_MODE_MAILBOX_KHR};
    const VkSwapchainPresentModesCreateInfoKHR present_modes_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,
        .presentModeCount = mailbox_compatible ? 2u : 1u,
        .pPresentModes = present_modes,
    };]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    swapchain.handle = new_handle;
    swapchain.image_count = image_count;]==]
[==[    swapchain.handle = new_handle;
    swapchain.mailbox_compatible = mailbox_compatible;
    swapchain.mailbox_enabled = swapchain.mailbox_enabled && mailbox_compatible;
    swapchain.image_count = image_count;]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[uint32x2 get_drawable_extent(Device* device) noexcept
{]==]
[==[bool supports_mailbox_presentation(Device* device) noexcept
{
    return device && device->swapchain && device->swapchain->handle &&
        device->swapchain->width != 0 && device->swapchain->height != 0 && device->swapchain->mailbox_compatible;
}

void set_mailbox_presentation(Device* device, bool enabled) noexcept
{
    if (device && device->swapchain)
        device->swapchain->mailbox_enabled = enabled && supports_mailbox_presentation(device);
}

uint32x2 get_drawable_extent(Device* device) noexcept
{]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[    const VkSwapchainPresentFenceInfoKHR fence_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1,]==]
[==[    const VkPresentModeKHR selected_present_mode = swapchain->mailbox_enabled && swapchain->mailbox_compatible
        ? VK_PRESENT_MODE_MAILBOX_KHR : swapchain_present_mode;
    const VkSwapchainPresentModeInfoKHR mode_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,
        .swapchainCount = 1,
        .pPresentModes = &selected_present_mode,
    };
    const VkSwapchainPresentFenceInfoKHR fence_info{
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .pNext = swapchain->mailbox_compatible ? &mode_info : nullptr,
        .swapchainCount = 1,]==])

replace_exact(src/NoGraphicsAPIMetal.mm
[==[uint32x2 get_drawable_extent(Device* device) noexcept
{]==]
[==[bool supports_mailbox_presentation(Device*) noexcept
{
    return false;
}

void set_mailbox_presentation(Device*, bool) noexcept
{
}

uint32x2 get_drawable_extent(Device* device) noexcept
{]==])
