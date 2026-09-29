# Remove the old prototype patch markers before applying the production extension.
if(EXISTS "${NGAPI_SOURCE}/include/NoGraphicsAPI/NoGraphicsAPI.hpp")
    file(READ "${NGAPI_SOURCE}/include/NoGraphicsAPI/NoGraphicsAPI.hpp" old_header)
    string(REPLACE "enum class PrimitiveTopology : uint8 { triangles, lines };" "enum class PrimitiveTopology : uint8 { triangles, lines, triangle_strip };" new_header "${old_header}")
    string(REPLACE "// Woby experiment: 1 or 4, Vulkan only." "// Woby: 1 or 4 samples, Vulkan and Metal." new_header "${new_header}")
    if(NOT old_header STREQUAL new_header)
        file(WRITE "${NGAPI_SOURCE}/include/NoGraphicsAPI/NoGraphicsAPI.hpp" "${new_header}")
    endif()
    file(READ "${NGAPI_SOURCE}/src/NoGraphicsAPI.cpp" old_source)
    string(REPLACE ".topology = topology == PrimitiveTopology::lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST," ".topology = topology == PrimitiveTopology::lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : topology == PrimitiveTopology::triangle_strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST," new_source "${old_source}")
    if(NOT old_source STREQUAL new_source)
        file(WRITE "${NGAPI_SOURCE}/src/NoGraphicsAPI.cpp" "${new_source}")
    endif()
endif()
# Woby's Vulkan and Metal extensions against the pinned source. Never patch a
# user's checkout: FetchContent invokes this only in its private dependency tree.
function(replace_exact path before after)
    file(READ "${NGAPI_SOURCE}/${path}" source)
    string(FIND "${source}" "${after}" applied)
    if(NOT applied EQUAL -1)
        return()
    endif()
    string(FIND "${source}" "${before}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "NoGraphicsAPI patch no longer matches ${path}")
    endif()
    string(REPLACE "${before}" "${after}" source "${source}")
    file(WRITE "${NGAPI_SOURCE}/${path}" "${source}")
endfunction()
set(header include/NoGraphicsAPI/NoGraphicsAPI.hpp)
set(source src/NoGraphicsAPI.cpp)
replace_exact(${header} "TextureUsage usage = TextureUsage::sampled;"
    "TextureUsage usage = TextureUsage::sampled;\n    uint32 sample_count = 1; // Woby: 1 or 4 samples, Vulkan and Metal.")
replace_exact(${header} "struct GraphicsPSODesc\n{"
    "enum class PrimitiveTopology : uint8 { triangles, lines, triangle_strip };\n\nstruct GraphicsPSODesc\n{")
replace_exact(${header} "    RasterizationState rasterization = {};\n};\n\nstruct MeshPSODesc"
    "    RasterizationState rasterization = {};\n    uint32 sample_count = 1;\n    PrimitiveTopology topology = PrimitiveTopology::triangles;\n};\n\nstruct MeshPSODesc")
replace_exact(${header} "    ClearColor clear = {};\n};"
    "    ClearColor clear = {};\n    RenderView* resolve_view = nullptr; // Color average resolve; never resolve integer IDs.\n};")
replace_exact(${source} "        .arrayLayers = desc.layer_count,\n        .samples = VK_SAMPLE_COUNT_1_BIT,"
    "        .arrayLayers = desc.layer_count,\n        .samples = static_cast<VkSampleCountFlagBits>(desc.sample_count),")
replace_exact(${source} "    output = {};\n    output.format_list.sType"
    "    assert(desc.sample_count == 1 || desc.sample_count == 4);\n    assert(desc.sample_count == 1 || (desc.type == TextureType::two_d && desc.mip_levels == 1));\n    output = {};\n    output.format_list.sType")
replace_exact(${source} "bool mesh, const ShaderStage& task = {}) noexcept"
    "bool mesh, const ShaderStage& task = {}, uint32 sample_count = 1, PrimitiveTopology topology = PrimitiveTopology::triangles) noexcept")
replace_exact(${source} ".topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,"
    ".topology = topology == PrimitiveTopology::lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : topology == PrimitiveTopology::triangle_strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,")
replace_exact(${source} ".rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,"
    ".rasterizationSamples = static_cast<VkSampleCountFlagBits>(sample_count),")
replace_exact(${source} "features.core.features.samplerAnisotropy == VK_TRUE &&"
    "features.core.features.samplerAnisotropy == VK_TRUE &&\n        features.core.features.sampleRateShading == VK_TRUE &&")
replace_exact(${source} "enabled_features.core.features.samplerAnisotropy = VK_TRUE;"
    "enabled_features.core.features.samplerAnisotropy = VK_TRUE;\n    enabled_features.core.features.sampleRateShading = VK_TRUE;")
replace_exact(${source} "desc.stencil_format, desc.rasterization, false);"
    "desc.stencil_format, desc.rasterization, false, {}, desc.sample_count, desc.topology);")
replace_exact(${source} "            .imageView = attachment.render_view->view,\n            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,"
    "            .imageView = attachment.render_view->view,\n            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,\n            .resolveMode = attachment.resolve_view ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,\n            .resolveImageView = attachment.resolve_view ? attachment.resolve_view->view : VK_NULL_HANDLE,\n            .resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL,")

# SDL presentation and Metal feature parity used by the production renderer.
replace_exact(include/NoGraphicsAPI/NoGraphicsAPI.hpp
[==[    uint32 timestamp_query_count = 256; // Per command buffer; zero disables timestamps.]==]
[==[    uint32 timestamp_query_count = 256; // Per command buffer; zero disables timestamps.
    // Optional Vulkan window-system adapter. The device owns the returned surface.
    const char* const* surface_extensions = nullptr;
    uint32 surface_extension_count = 0;
    uint64 (*create_surface)(void* window, void* instance) = nullptr;
    uint32x2 (*drawable_extent)(void* window) = nullptr;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    VkSurfaceKHR surface = VK_NULL_HANDLE;
    uint32 queue_families]==]
[==[    VkSurfaceKHR surface = VK_NULL_HANDLE;
    void* window = nullptr;
    uint32x2 (*drawable_extent)(void*) = nullptr;
    uint32 queue_families]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[#if !defined(_WIN32)
    if (presentation)
        return {.error = Error::unsupported};
#endif]==]
[==[#if !defined(_WIN32)
    if (presentation && !desc.create_surface)
        return {.error = Error::unsupported};
#endif]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    state->timestamp_query_count = desc.timestamp_query_count;]==]
[==[    state->timestamp_query_count = desc.timestamp_query_count;
    state->window = desc.window;
    state->drawable_extent = desc.drawable_extent;]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[#if defined(_WIN32)
    const bool khr_surface_maintenance1]==]
[==[    const bool khr_surface_maintenance1]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[         !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_WIN32_SURFACE_EXTENSION_NAME) ||]==]
[==[#if defined(_WIN32)
         (!desc.create_surface && !has_name({instance_extensions, instance_extension_count},
                   VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) ||
#endif]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[#else
    constexpr bool khr_surface_maintenance1 = false;
    constexpr bool ext_surface_maintenance1 = false;
#endif
#if !defined(NDEBUG)]==]
[==[#if !defined(NDEBUG)]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    const char* enabled_instance_extensions[6]{};]==]
[==[    const char* enabled_instance_extensions[32]{};
    if (desc.surface_extension_count > 24) return fail_device_creation(state, Error::unsupported);]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[#if defined(_WIN32)
    if (presentation)
    {
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;]==]
[==[    if (presentation)
    {
        enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
#if defined(_WIN32)
        if (!desc.create_surface) enabled_instance_extensions[enabled_instance_extension_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#endif
        for (uint32 i=0; i<desc.surface_extension_count; ++i) {
            const char* name=desc.surface_extensions[i];
            if (!has_name({instance_extensions, instance_extension_count}, name)) return fail_device_creation(state, Error::unsupported);
            if (strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) != 0) enabled_instance_extensions[enabled_instance_extension_count++] = name;
        }]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    }
#endif

    const VkApplicationInfo app_info]==]
[==[    }

    const VkApplicationInfo app_info]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[#if defined(_WIN32)
    if (presentation)
    {
        const VkWin32SurfaceCreateInfoKHR surface_info]==]
[==[    if (presentation && desc.create_surface) {
        state->surface = reinterpret_cast<VkSurfaceKHR>(desc.create_surface(desc.window, state->instance));
        if (!state->surface) return fail_device_creation(state, Error::driver_error);
    }
#if defined(_WIN32)
    if (presentation && !desc.create_surface)
    {
        const VkWin32SurfaceCreateInfoKHR surface_info]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[[[nodiscard]] bool swapchain_surface_configuration_changed(const Swapchain& swapchain) noexcept]==]
[==[VkExtent2D surface_extent(const Device& device, const VkSurfaceCapabilitiesKHR& capabilities) noexcept
{
    if (capabilities.currentExtent.width != UINT_MAX) return capabilities.currentExtent;
    if (!device.drawable_extent) return capabilities.currentExtent;
    const auto size=device.drawable_extent(device.window);
    if (!size.x || !size.y) return {};
    return {std::clamp(size.x, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
            std::clamp(size.y, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
}

[[nodiscard]] bool swapchain_surface_configuration_changed(const Swapchain& swapchain) noexcept]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    const VkExtent2D extent = capabilities.currentExtent;
    const uint32 variable_extent]==]
[==[    const VkExtent2D extent = surface_extent(*swapchain.state, capabilities);
    const uint32 variable_extent]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT_MAX)
]==]
[==[    const VkExtent2D extent = surface_extent(device, capabilities);
    if (extent.width == UINT_MAX)
]==])
replace_exact(src/NoGraphicsAPI.cpp
[==[    const VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT_MAX ||]==]
[==[    const VkExtent2D extent = surface_extent(*device, capabilities);
    if (extent.width == UINT_MAX ||]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[    result.textureType = texture_type(desc.type);]==]
[==[    result.textureType = desc.sample_count > 1 ? MTLTextureType2DMultisample : texture_type(desc.type);
    result.sampleCount = desc.sample_count;]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[    bool mesh = false;]==]
[==[    bool mesh = false;
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[        pipeline.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;]==]
[==[        pipeline.inputPrimitiveTopology = desc.topology == PrimitiveTopology::lines ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
        pipeline.rasterSampleCount = desc.sample_count;]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[        return new PSO{.device = device, .render = state, .rasterization = desc.rasterization};]==]
[==[        return new PSO{.device = device, .render = state, .rasterization = desc.rasterization,
            .primitive = desc.topology == PrimitiveTopology::lines ? MTLPrimitiveTypeLine :
                desc.topology == PrimitiveTopology::triangle_strip ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle};]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[drawPrimitives:MTLPrimitiveTypeTriangle]==]
[==[drawPrimitives:commands->pso->primitive]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[drawIndexedPrimitives:MTLPrimitiveTypeTriangle]==]
[==[drawIndexedPrimitives:commands->pso->primitive]==])
replace_exact(src/NoGraphicsAPIMetal.mm
[==[                render_attachment(commands->pass.colorAttachments[i], color.render_view, color.load, color.store);]==]
[==[                render_attachment(commands->pass.colorAttachments[i], color.render_view, color.load, color.store);
                commands->pass.colorAttachments[i].resolveTexture = color.resolve_view ? color.resolve_view->texture : nil;
                if (color.resolve_view) commands->pass.colorAttachments[i].storeAction = MTLStoreActionStoreAndMultisampleResolve;]==])

replace_exact(src/NoGraphicsAPI.cpp "#include <assert.h>" "#include <assert.h>\n#include <algorithm>")
