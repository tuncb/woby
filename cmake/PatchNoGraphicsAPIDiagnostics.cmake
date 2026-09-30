# Retain diagnostics at the pinned backend's actual checks, without a second
# Vulkan probe or a separate capability profile that could drift from them.
# Include after PatchNoGraphicsAPI.cmake, which defines replace_exact().
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[==[struct DeviceDesc
{]==]
[==[// Woby: synchronous diagnostic callbacks. All strings are borrowed for the call.
enum class DeviceDiagnosticKind : uint8
{
    device, missing_extension, missing_feature, missing_requirement,
    feature_checks_skipped, initialization_error, no_suitable_device, selected_device,
};
struct DeviceDiagnostic
{
    DeviceDiagnosticKind kind = DeviceDiagnosticKind::device;
    const char* detail = nullptr;
    const char* device_name = nullptr;
    const char* driver_info = nullptr;
    uint32 driver_version = 0;
    uint32 api_version = 0;
    Error error = Error::none;
    int32 api_result = 0;
};

struct DeviceDesc
{]==])
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[==[    uint32x2 (*drawable_extent)(void* window) = nullptr;]==]
[==[    uint32x2 (*drawable_extent)(void* window) = nullptr;
    void* diagnostic_context = nullptr;
    void (*diagnostic)(void* context, const DeviceDiagnostic& diagnostic) = nullptr;]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[Error inspect_candidate(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool khr_surface_maintenance1, bool ext_surface_maintenance1,]==]
[==[void report_diagnostic(const DeviceDesc& desc, DeviceDiagnosticKind kind, const char* detail,
                       Error error = Error::none, int32 api_result = 0) noexcept
{
    if (desc.diagnostic)
        desc.diagnostic(desc.diagnostic_context, {.kind = kind, .detail = detail, .error = error, .api_result = api_result});
}

Error diagnostic_vk_result(const DeviceDesc& desc, const char* operation, VkResult result) noexcept
{
    const Error error = error_from_vk(result);
    if (error != Error::none)
        report_diagnostic(desc, DeviceDiagnosticKind::initialization_error, operation, error, static_cast<int32>(result));
    return error;
}

void report_candidate(const DeviceDesc& desc, const VkPhysicalDeviceProperties& properties,
                      VkPhysicalDevice physical_device) noexcept
{
    if (!desc.diagnostic) return;
    VkPhysicalDeviceDriverProperties driver{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    if (properties.apiVersion >= VK_API_VERSION_1_2)
    {
        VkPhysicalDeviceProperties2 queried{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
        vkGetPhysicalDeviceProperties2(physical_device, &queried);
    }
    desc.diagnostic(desc.diagnostic_context, {
        .kind = DeviceDiagnosticKind::device,
        .device_name = properties.deviceName,
        .driver_info = driver.driverInfo,
        .driver_version = properties.driverVersion,
        .api_version = properties.apiVersion,
    });
}

Error inspect_candidate(VkPhysicalDevice physical_device, VkSurfaceKHR surface, bool khr_surface_maintenance1, bool ext_surface_maintenance1,]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    VkExtensionProperties extensions[max_device_extensions]{};
    uint32 extension_count = 0;
    const Error extension_error]==]
[==[    VkPhysicalDeviceProperties basic_properties{};
    vkGetPhysicalDeviceProperties(physical_device, &basic_properties);
    report_candidate(desc, basic_properties, physical_device);
    VkExtensionProperties extensions[max_device_extensions]{};
    uint32 extension_count = 0;
    const Error extension_error]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (extension_error != Error::none)
        return extension_error;]==]
[==[    if (extension_error != Error::none)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::initialization_error, "vkEnumerateDeviceExtensionProperties", extension_error);
        return extension_error;
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    for (const char* name : required_extensions)
    {
        if (!has_name({extensions, extension_count}, name))
            return Error::unsupported;
    }]==]
[==[    bool missing_capability = false;
    for (const char* name : required_extensions)
    {
        if (!has_name({extensions, extension_count}, name))
        {
            report_diagnostic(desc, DeviceDiagnosticKind::missing_extension, name);
            missing_capability = true;
        }
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (surface &&
        (!has_name({extensions, extension_count}, VK_KHR_SWAPCHAIN_EXTENSION_NAME) ||
         (!khr_swapchain_maintenance1 && !ext_swapchain_maintenance1)))
    {
        return Error::unsupported;
    }]==]
[==[    if (surface && !has_name({extensions, extension_count}, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    {
        report_diagnostic(desc, DeviceDiagnosticKind::missing_extension, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        missing_capability = true;
    }
    if (surface && !khr_swapchain_maintenance1 && !ext_swapchain_maintenance1)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::missing_extension,
            "VK_KHR_swapchain_maintenance1 or VK_EXT_swapchain_maintenance1 (with matching instance surface maintenance support)");
        missing_capability = true;
    }
    if (basic_properties.apiVersion < VK_API_VERSION_1_4)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::missing_requirement, "Vulkan device API 1.4");
        missing_capability = true;
    }
    if (missing_capability)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::feature_checks_skipped,
            "required extensions or Vulkan API version are missing; unqueried flags are not evidence of unsupported features");
        return Error::unsupported;
    }]==])
# The API version was checked above, before querying newer extension properties.
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (result.properties.apiVersion < VK_API_VERSION_1_4)
        return Error::unsupported;]==]
[==[    // Woby: device API version was checked before querying extension properties.]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (!cpu_visible_memory) return Error::unsupported;]==]
[==[    if (!cpu_visible_memory)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::missing_requirement, "usable CPU-visible, host-coherent device memory");
        return Error::unsupported;
    }]==])

# Keep the original feature gate intact. On failure, report every failed check,
# including the original alternatives and conditional presentation requirements.
# Generate the names from the actual expressions, so the profile cannot drift.
file(READ "${NGAPI_SOURCE}/src/NoGraphicsAPI.cpp" diagnostic_source)
if(NOT diagnostic_source MATCHES "// Woby: report every failed required feature")
    string(REGEX MATCH "    const bool required_features =[^;]+;" feature_checks "${diagnostic_source}")
    if(NOT feature_checks)
        message(FATAL_ERROR "NoGraphicsAPI required feature checks no longer match")
    endif()
    set(reported_checks "${feature_checks}")
    foreach(feature_struct IN ITEMS
            "core.features|VkPhysicalDeviceFeatures"
            "vulkan11|VkPhysicalDeviceVulkan11Features"
            "vulkan12|VkPhysicalDeviceVulkan12Features"
            "vulkan13|VkPhysicalDeviceVulkan13Features"
            "vulkan14|VkPhysicalDeviceVulkan14Features"
            "descriptor_heap|VkPhysicalDeviceDescriptorHeapFeaturesEXT"
            "address_commands|VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR"
            "untyped_pointers|VkPhysicalDeviceShaderUntypedPointersFeaturesKHR"
            "mesh_shader|VkPhysicalDeviceMeshShaderFeaturesEXT")
        string(REPLACE "|" ";" parts "${feature_struct}")
        list(GET parts 0 member)
        list(GET parts 1 type)
        string(REPLACE "." "\\." member_regex "${member}")
        string(REGEX MATCHALL "features\\.${member_regex}\\.[a-zA-Z0-9_]+ == VK_TRUE" expressions "${feature_checks}")
        foreach(expression IN LISTS expressions)
            if(expression MATCHES "textureCompression")
                continue() # BC or ASTC is one alternative requirement below.
            endif()
            string(REGEX REPLACE ".*\\.([a-zA-Z0-9_]+) == VK_TRUE" "\\1" flag "${expression}")
            string(REPLACE "${expression}" "check_feature(${expression}, \"${type}::${flag}\")" reported_checks "${reported_checks}")
        endforeach()
    endforeach()
    string(REPLACE
        "(features.core.features.textureCompressionBC == VK_TRUE ||\n         features.core.features.textureCompressionASTC_LDR == VK_TRUE)"
        "check_feature(features.core.features.textureCompressionBC == VK_TRUE || features.core.features.textureCompressionASTC_LDR == VK_TRUE,\n            \"VkPhysicalDeviceFeatures::textureCompressionBC or VkPhysicalDeviceFeatures::textureCompressionASTC_LDR\")"
        reported_checks "${reported_checks}")
    string(REPLACE "(!surface || features.swapchain_maintenance1.swapchainMaintenance1 == VK_TRUE)"
        "check_feature(!surface || features.swapchain_maintenance1.swapchainMaintenance1 == VK_TRUE,\n            \"VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR::swapchainMaintenance1\")"
        reported_checks "${reported_checks}")
    string(REPLACE "    const bool required_features =\n" "" reported_checks "${reported_checks}")
    string(REPLACE " &&\n" ";\n" reported_checks "${reported_checks}")
    set(reported_checks "    if (!required_features)\n    {\n        // Woby: report every failed required feature without short-circuiting.\n        const auto check_feature = [&](bool supported, const char* name) {\n            if (!supported) report_diagnostic(desc, DeviceDiagnosticKind::missing_feature, name);\n        };\n${reported_checks}\n        return Error::unsupported;\n    }")
    replace_exact(src/NoGraphicsAPI.cpp "    if (!required_features)\n        return Error::unsupported;" "${reported_checks}")
endif()

replace_exact(src/NoGraphicsAPI.cpp
[==[    if (available_queue_count > max_queue_families)
        return Error::unsupported;]==]
[==[    if (available_queue_count > max_queue_families)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::initialization_error, "queue family enumeration exceeds backend capacity", Error::unsupported);
        return Error::unsupported;
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, index, surface, &presentation_supported))]==]
[==[diagnostic_vk_result(desc, "vkGetPhysicalDeviceSurfaceSupportKHR", vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, index, surface, &presentation_supported))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (result.queue_counts[0] == 0 || (desc.desired_compute_queue_count && result.queue_counts[1] == 0) ||
        (desc.desired_copy_queue_count && result.queue_counts[2] == 0))
        return Error::unsupported;]==]
[==[    if (result.queue_counts[0] == 0 || (desc.desired_compute_queue_count && result.queue_counts[1] == 0) ||
        (desc.desired_copy_queue_count && result.queue_counts[2] == 0))
    {
        report_diagnostic(desc, DeviceDiagnosticKind::missing_requirement,
            "requested graphics/compute/transfer queues with surface presentation support when windowed");
        return Error::unsupported;
    }]==])

replace_exact(src/NoGraphicsAPI.cpp
[==[Error recreate_swapchain(Swapchain& swapchain) noexcept;]==]
[==[DeviceInit fail_initialization(const DeviceDesc& desc, Device* device, Error error, const char* operation) noexcept
{
    report_diagnostic(desc, DeviceDiagnosticKind::initialization_error, operation, error);
    return fail_device_creation(device, error);
}

Error recreate_swapchain(Swapchain& swapchain) noexcept;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (presentation && !desc.create_surface)
        return {.error = Error::unsupported};]==]
[==[    if (presentation && !desc.create_surface)
        return fail_initialization(desc, nullptr, Error::unsupported, "Vulkan surface adapter is unavailable");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(vkEnumerateInstanceVersion(&loader_version))]==]
[==[diagnostic_vk_result(desc, "vkEnumerateInstanceVersion", vkEnumerateInstanceVersion(&loader_version))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (loader_version < VK_API_VERSION_1_4)
        return { .error = Error::unsupported };]==]
[==[    if (loader_version < VK_API_VERSION_1_4)
    {
        char requirement[128]{};
        snprintf(requirement, sizeof(requirement), "Vulkan loader API 1.4 (found %u.%u.%u)",
            VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version), VK_API_VERSION_PATCH(loader_version));
        report_diagnostic(desc, DeviceDiagnosticKind::missing_requirement, requirement);
        return { .error = Error::unsupported };
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    error = enumerate_instance_extensions({instance_extensions, max_instance_extensions}, instance_extension_count);
    if (error != Error::none)
        return fail_device_creation(state, error);]==]
[==[    error = enumerate_instance_extensions({instance_extensions, max_instance_extensions}, instance_extension_count);
    if (error != Error::none)
        return fail_initialization(desc, state, error, "vkEnumerateInstanceExtensionProperties");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (presentation &&
        (!has_name({instance_extensions, instance_extension_count},
                   VK_KHR_SURFACE_EXTENSION_NAME) ||
#if defined(_WIN32)
         (!desc.create_surface && !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) ||
#endif
         !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) ||
         (!khr_surface_maintenance1 && !ext_surface_maintenance1)))
    {
        return fail_device_creation(state, Error::unsupported);
    }]==]
[==[    if (presentation)
    {
        bool missing_instance_extension = false;
        const auto check_instance_extension = [&](const char* name) {
            if (!has_name({instance_extensions, instance_extension_count}, name))
            {
                report_diagnostic(desc, DeviceDiagnosticKind::missing_extension, name);
                missing_instance_extension = true;
            }
        };
        check_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(_WIN32)
        if (!desc.create_surface) check_instance_extension(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif
        check_instance_extension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        if (!khr_surface_maintenance1 && !ext_surface_maintenance1)
        {
            report_diagnostic(desc, DeviceDiagnosticKind::missing_extension,
                "VK_KHR_surface_maintenance1 or VK_EXT_surface_maintenance1");
            missing_instance_extension = true;
        }
        if (missing_instance_extension) return fail_device_creation(state, Error::unsupported);
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    error = enumerate_instance_layers({layers, max_instance_layers}, layer_count);
    if (error != Error::none)
        return fail_device_creation(state, error);]==]
[==[    error = enumerate_instance_layers({layers, max_instance_layers}, layer_count);
    if (error != Error::none)
        return fail_initialization(desc, state, error, "vkEnumerateInstanceLayerProperties");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (desc.surface_extension_count > 24) return fail_device_creation(state, Error::unsupported);]==]
[==[    if (desc.surface_extension_count > 24)
        return fail_initialization(desc, state, Error::unsupported, "surface extension count exceeds backend capacity");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[            if (!has_name({instance_extensions, instance_extension_count}, name)) return fail_device_creation(state, Error::unsupported);]==]
[==[            if (!has_name({instance_extensions, instance_extension_count}, name)) {
                report_diagnostic(desc, DeviceDiagnosticKind::missing_extension, name);
                return fail_device_creation(state, Error::unsupported);
            }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(vkCreateInstance(&instance_info, nullptr, &state->instance))]==]
[==[diagnostic_vk_result(desc, "vkCreateInstance", vkCreateInstance(&instance_info, nullptr, &state->instance))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        if (!create_debug || !state->destroy_debug_messenger)
            return fail_device_creation(state, Error::driver_error);]==]
[==[        if (!create_debug || !state->destroy_debug_messenger)
            return fail_initialization(desc, state, Error::driver_error, "loading Vulkan debug messenger entry points");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger))]==]
[==[diagnostic_vk_result(desc, "vkCreateDebugUtilsMessengerEXT", create_debug(state->instance, &debug_info, nullptr, &state->debug_messenger))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        if (!state->surface) return fail_device_creation(state, Error::driver_error);]==]
[==[        if (!state->surface) return fail_initialization(desc, state, Error::driver_error, "creating the SDL Vulkan surface");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(vkCreateWin32SurfaceKHR(state->instance, &surface_info, nullptr, &state->surface))]==]
[==[diagnostic_vk_result(desc, "vkCreateWin32SurfaceKHR", vkCreateWin32SurfaceKHR(state->instance, &surface_info, nullptr, &state->surface))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    error = enumerate_physical_devices(state->instance, {physical_devices, max_physical_devices}, physical_device_count);
    if (error != Error::none)
        return fail_device_creation(state, error);]==]
[==[    error = enumerate_physical_devices(state->instance, {physical_devices, max_physical_devices}, physical_device_count);
    if (error != Error::none)
        return fail_initialization(desc, state, error, "vkEnumeratePhysicalDevices");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (!has_selected)
        return fail_device_creation(state, Error::unsupported);]==]
[==[    if (!has_selected)
    {
        report_diagnostic(desc, DeviceDiagnosticKind::no_suitable_device, physical_device_count
            ? "No detected GPU passed device selection." : "No Vulkan physical devices were detected.");
        return fail_device_creation(state, Error::unsupported);
    }
    report_diagnostic(desc, DeviceDiagnosticKind::selected_device, selected.properties.deviceName);]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device))]==]
[==[diagnostic_vk_result(desc, "vkCreateDevice", vkCreateDevice(state->physical_device, &device_info, nullptr, &state->device))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    if (!supports_gpu_heap_memory(*state) || !select_texture_memory_type(*state))
        return fail_device_creation(state, Error::unsupported);]==]
[==[    if (!supports_gpu_heap_memory(*state) || !select_texture_memory_type(*state))
    {
        report_diagnostic(desc, DeviceDiagnosticKind::missing_requirement, "GPU heap and texture memory types");
        return fail_device_creation(state, Error::unsupported);
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[!state->fn.cmd_copy_image_to_memory)
    {
        return fail_device_creation(state, Error::driver_error);
    }]==]
[==[!state->fn.cmd_copy_image_to_memory)
    {
        return fail_initialization(desc, state, Error::driver_error, "loading required Vulkan device entry points");
    }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[error_from_vk(vkCreateSemaphore(state->device, &semaphore_info, nullptr, &state->presentation_retirement))]==]
[==[diagnostic_vk_result(desc, "vkCreateSemaphore (presentation retirement)", vkCreateSemaphore(state->device, &semaphore_info, nullptr, &state->presentation_retirement))]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[            error = state->create_present_context(state->present_contexts[index]);
            if (error != Error::none)
                return fail_device_creation(state, error);]==]
[==[            error = state->create_present_context(state->present_contexts[index]);
            if (error != Error::none)
                return fail_initialization(desc, state, error, "creating Vulkan presentation context");]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[        error = recreate_swapchain(*state->swapchain);
        if (error != Error::none)
            return fail_device_creation(state, error);]==]
[==[        error = recreate_swapchain(*state->swapchain);
        if (error != Error::none)
            return fail_initialization(desc, state, error, "creating Vulkan swapchain");]==])
