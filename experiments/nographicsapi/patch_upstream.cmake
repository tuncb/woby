# Experimental Vulkan-only additions against the pinned source. Never patch a
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
    "TextureUsage usage = TextureUsage::sampled;\n    uint32 sample_count = 1; // Woby experiment: 1 or 4, Vulkan only.")
replace_exact(${header} "struct GraphicsPSODesc\n{"
    "enum class PrimitiveTopology : uint8 { triangles, lines };\n\nstruct GraphicsPSODesc\n{")
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
    ".topology = topology == PrimitiveTopology::lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,")
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
