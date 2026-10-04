# Optional experiment feature. Production builds do not enable it, and devices
# without 64-bit buffer atomics still create normally (the prototype rejects them).
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
    "    bool fragment_barycentric = false;"
    "    bool fragment_barycentric = false;\n    bool point_buffer_int64_atomics = false;")
replace_exact(src/NoGraphicsAPI.cpp
    "    state->texture_compression_etc2 = selected.texture_compression_etc2;"
[==[#ifdef WOBY_POINT_EXPERIMENT
    VkPhysicalDeviceVulkan12Features point_features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 point_query{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &point_features};
    vkGetPhysicalDeviceFeatures2(state->physical_device, &point_query);
    const bool point_atomics = point_query.features.shaderInt64 && point_features.shaderBufferInt64Atomics;
    enabled_features.core.features.shaderInt64 = point_atomics ? VK_TRUE : VK_FALSE;
    enabled_features.vulkan12.shaderBufferInt64Atomics = point_atomics ? VK_TRUE : VK_FALSE;
#endif
    state->texture_compression_etc2 = selected.texture_compression_etc2;]==])
replace_exact(src/NoGraphicsAPI.cpp
    "    if (presentation)\n    {\n        state->swapchain = new Swapchain;"
[==[
#ifdef WOBY_POINT_EXPERIMENT
    state->caps.point_buffer_int64_atomics = point_atomics;
#endif
    if (presentation)
    {
        state->swapchain = new Swapchain;]==])
target_compile_definitions(NoGraphicsAPI PRIVATE WOBY_POINT_EXPERIMENT)
