# Upgrade a private FetchContent tree previously configured with the prototype.
foreach(path IN ITEMS include/NoGraphicsAPI/NoGraphicsAPI.hpp src/NoGraphicsAPI.cpp)
    file(READ "${NGAPI_SOURCE}/${path}" source)
    string(REPLACE "overlay_fragment_barycentric" "fragment_barycentric" updated "${source}")
    string(REPLACE "WOBY_OVERLAY_EXPERIMENT" "WOBY_FRAGMENT_BARYCENTRICS" updated "${updated}")
    string(REPLACE " // Opt-in Woby experiment only." "" updated "${updated}")
    string(REPLACE "optional overlay experiment" "optional barycentric feature" updated "${updated}")
    if(NOT source STREQUAL updated)
        file(WRITE "${NGAPI_SOURCE}/${path}" "${updated}")
    endif()
endforeach()
# std::strcmp requires <cstring>; <string.h> only guarantees the global name.
replace_exact(src/NoGraphicsAPI.cpp
    "#include <string.h>"
    "#include <string.h>\n#include <cstring>")
# Optional Vulkan feature; unsupported devices and Metal use vertex pulling.
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
    "    bool indirect_mesh_draw = false;"
    "    bool indirect_mesh_draw = false;\n    bool fragment_barycentric = false;")
replace_exact(src/NoGraphicsAPI.cpp
    "    QueriedFeatures enabled_features(presentation, selected.unified_image_layouts);"
[==[    QueriedFeatures enabled_features(presentation, selected.unified_image_layouts);
#ifdef WOBY_FRAGMENT_BARYCENTRICS
    VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR overlay_barycentric{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 overlay_query{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &overlay_barycentric};
    VkExtensionProperties overlay_extensions[max_device_extensions]{};
    uint32 overlay_extension_count = 0;
    bool overlay_supported = false;
    if (enumerate_device_extensions(state->physical_device,
            {overlay_extensions, max_device_extensions}, overlay_extension_count) == Error::none)
    {
        for (uint32 i = 0; i < overlay_extension_count; ++i)
            if (std::strcmp(overlay_extensions[i].extensionName, VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME) == 0)
                overlay_supported = true;
    }
    if (overlay_supported)
    {
        vkGetPhysicalDeviceFeatures2(state->physical_device, &overlay_query);
        overlay_supported = overlay_barycentric.fragmentShaderBarycentric == VK_TRUE;
    }
    if (overlay_supported)
    {
        overlay_barycentric.pNext = enabled_features.core.pNext;
        enabled_features.core.pNext = &overlay_barycentric;
    }
#endif]==])
replace_exact(src/NoGraphicsAPI.cpp "    const char* enabled_device_extensions[7]{};"
    "    const char* enabled_device_extensions[8]{}; // Room for the optional barycentric feature.")
replace_exact(src/NoGraphicsAPI.cpp "    const VkDeviceCreateInfo device_info{"
[==[#ifdef WOBY_FRAGMENT_BARYCENTRICS
    if (overlay_supported)
        enabled_device_extensions[enabled_device_extension_count++] = VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME;
#endif
    const VkDeviceCreateInfo device_info{]==])
replace_exact(src/NoGraphicsAPI.cpp "        .indirect_mesh_draw = true,"
[==[        .indirect_mesh_draw = true,
#ifdef WOBY_FRAGMENT_BARYCENTRICS
        .fragment_barycentric = overlay_supported,
#endif]==])
