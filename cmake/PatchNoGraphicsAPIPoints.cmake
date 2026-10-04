# Optional point capabilities; adaptive selection is independent of atomics.
# Upgrade previously configured experiment trees.
foreach(path IN ITEMS include/NoGraphicsAPI/NoGraphicsAPI.hpp src/NoGraphicsAPI.cpp)
    file(READ "${NGAPI_SOURCE}/${path}" source)
    string(REPLACE "WOBY_POINT_EXPERIMENT" "WOBY_OPAQUE_POINTS" updated "${source}")
    string(REPLACE [==[    const bool point_atomics = point_query.features.shaderInt64 && point_features.shaderBufferInt64Atomics;]==] [==[    VkPhysicalDeviceProperties point_properties{};
    vkGetPhysicalDeviceProperties(state->physical_device, &point_properties);
    const bool point_atomics = point_query.features.shaderInt64 && point_features.shaderBufferInt64Atomics
        && point_properties.limits.standardSampleLocations;]==] updated "${updated}")
    if(NOT source STREQUAL updated)
        file(WRITE "${NGAPI_SOURCE}/${path}" "${updated}")
    endif()
endforeach()
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
    "    bool fragment_barycentric = false;"
    "    bool fragment_barycentric = false;\n    bool point_buffer_int64_atomics = false;")
replace_exact(src/NoGraphicsAPI.cpp
    "    state->texture_compression_etc2 = selected.texture_compression_etc2;"
[==[#ifdef WOBY_OPAQUE_POINTS
    VkPhysicalDeviceVulkan12Features point_features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 point_query{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &point_features};
    vkGetPhysicalDeviceFeatures2(state->physical_device, &point_query);
    VkPhysicalDeviceProperties point_properties{};
    vkGetPhysicalDeviceProperties(state->physical_device, &point_properties);
    const bool point_atomics = point_query.features.shaderInt64 && point_features.shaderBufferInt64Atomics
        && point_properties.limits.standardSampleLocations;
    enabled_features.core.features.shaderInt64 = point_atomics ? VK_TRUE : VK_FALSE;
    enabled_features.vulkan12.shaderBufferInt64Atomics = point_atomics ? VK_TRUE : VK_FALSE;
#endif
    state->texture_compression_etc2 = selected.texture_compression_etc2;]==]
    "    const bool point_compute = point_query.features.shaderInt64")
replace_exact(src/NoGraphicsAPI.cpp
    "    if (presentation)\n    {\n        state->swapchain = new Swapchain;"
[==[
#ifdef WOBY_OPAQUE_POINTS
    state->caps.point_buffer_int64_atomics = point_atomics;
#endif
    if (presentation)
    {
        state->swapchain = new Swapchain;]==]
    "    state->caps.point_buffer_int64_atomics = point_atomics;")
target_compile_definitions(NoGraphicsAPI PRIVATE WOBY_OPAQUE_POINTS)
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
    "    bool point_buffer_int64_atomics = false;"
    "    bool point_buffer_int64_atomics = false;\n    bool point_compute = false;")
replace_exact(src/NoGraphicsAPI.cpp
[==[    const bool point_atomics = point_query.features.shaderInt64 && point_features.shaderBufferInt64Atomics
        && point_properties.limits.standardSampleLocations;
    enabled_features.core.features.shaderInt64 = point_atomics ? VK_TRUE : VK_FALSE;]==]
[==[    const bool point_compute = point_query.features.shaderInt64 && point_properties.limits.standardSampleLocations;
    const bool point_atomics = point_compute && point_features.shaderBufferInt64Atomics;
    enabled_features.core.features.shaderInt64 = point_compute ? VK_TRUE : VK_FALSE;]==])
replace_exact(src/NoGraphicsAPI.cpp
    "    state->caps.point_buffer_int64_atomics = point_atomics;"
    "    state->caps.point_buffer_int64_atomics = point_atomics;\n    state->caps.point_compute = point_compute;")
replace_exact(src/NoGraphicsAPIMetal.mm
[==[        if (desc.window)
        {]==]
[==[        // Both compute backends use the same 1x/4x circle coverage as the
        // hardware render targets. Fall back to quads for an unknown layout.
        const MTLSamplePosition expected[4] = {{.375f,.125f},{.875f,.375f},{.125f,.625f},{.625f,.875f}};
        MTLSamplePosition positions[4] = {};
        bool point_samples = device->metal.areProgrammableSamplePositionsSupported
            && [device->metal supportsTextureSampleCount:4];
        if (point_samples) {
            [device->metal getDefaultSamplePositions:positions count:4];
            for (uint32 i = 0; i < 4; ++i)
                point_samples = point_samples && positions[i].x == expected[i].x && positions[i].y == expected[i].y;
            [device->metal getDefaultSamplePositions:positions count:1];
            point_samples = point_samples && positions[0].x == .5f && positions[0].y == .5f;
        }
        device->caps.point_compute = point_samples;
        // Apple8 supports ulong min/max on macOS; Apple9+ supports the full set.
        device->caps.point_buffer_int64_atomics = point_samples
            && ([device->metal supportsFamily:MTLGPUFamilyApple8] || [device->metal supportsFamily:MTLGPUFamilyApple9]);
        if (desc.window)
        {]==])
