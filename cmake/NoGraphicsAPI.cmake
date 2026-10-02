include_guard(GLOBAL)
include(FetchContent)
FetchContent_Declare(woby_ngapi
    URL https://github.com/sebbbi/NoGraphicsAPI/archive/ae017a2f545abc0847e546cc7e84139bf3cc4241.zip
    URL_HASH SHA256=3a51391ca3004b391a1b86b972e16c9004e532bedf26f9419211fbd5d228a722
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(woby_ngapi)
set(NGAPI_SOURCE "${woby_ngapi_SOURCE_DIR}")
include("${CMAKE_CURRENT_LIST_DIR}/PatchNoGraphicsAPI.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PatchNoGraphicsAPIPacing.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PatchNoGraphicsAPIDiagnostics.cmake")

find_program(WOBY_SLANGC slangc HINTS "$ENV{SLANG_ROOT}/bin" REQUIRED)
execute_process(COMMAND "${WOBY_SLANGC}" -version OUTPUT_VARIABLE slang_version ERROR_VARIABLE slang_error)
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" slang_version_number "${slang_version}${slang_error}")
if(NOT slang_version_number OR slang_version_number VERSION_LESS 2026.18.3)
    message(FATAL_ERROR "Woby requires standalone Slang >= 2026.18.3. Set WOBY_SLANGC to its executable; older Vulkan SDK bundled compilers are insufficient.")
endif()
if(APPLE)
    find_program(WOBY_XCRUN xcrun REQUIRED)
else()
    find_program(WOBY_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)
    execute_process(COMMAND "${WOBY_SPIRV_VAL}" --version OUTPUT_VARIABLE spirv_version COMMAND_ERROR_IS_FATAL ANY)
    string(REGEX MATCH "v([0-9]+\\.[0-9]+)" spirv_version_match "${spirv_version}")
    if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 VERSION_LESS 2026.3)
        message(FATAL_ERROR "Woby requires SPIRV-Tools >= 2026.3 for descriptor-heap validation. Set WOBY_SPIRV_VAL to the Vulkan SDK 1.4.357.0+ validator.")
    endif()
endif()
message(STATUS "Woby renderer: NoGraphicsAPI, Slang ${slang_version_number}")

add_library(woby_graphics STATIC "${PROJECT_SOURCE_DIR}/src/graphics.cpp" "${PROJECT_SOURCE_DIR}/src/frame_pacing.cpp"
    "${PROJECT_SOURCE_DIR}/src/graphics_diagnostics.cpp")
target_include_directories(woby_graphics PUBLIC "${PROJECT_SOURCE_DIR}/src" PRIVATE "${PROJECT_SOURCE_DIR}/shaders/native")
target_link_libraries(woby_graphics PUBLIC NoGraphicsAPI::NoGraphicsAPI PRIVATE
    NoGraphicsAPIUtility::allocators NoGraphicsAPIUtility::textures NoGraphicsAPIUtility::uploads
    NoGraphicsAPIUtility::types woby_math SDL3::SDL3)
target_compile_definitions(woby_graphics PRIVATE NOMINMAX)
woby_enable_project_warnings(woby_graphics)

function(woby_compile_graphics_shaders target)
    set(entries vs_mesh fs_mesh vs_color fs_color vs_annotation vs_point_sprite vs_line_sprite fs_point_sprite
        vs_comparison fs_comparison vs_imgui fs_imgui vs_marker_point fs_marker_point fs_marker_mesh
        fs_marker_line fs_marker_comparison vs_marker_screen fs_marker_composite
        vs_marker_highlight fs_marker_highlight_single fs_marker_highlight_msaa
        cs_marker_lookup_single cs_marker_lookup_msaa cs_freeform)
    set(outputs)
    foreach(entry IN LISTS entries)
        if(entry MATCHES "^vs_")
            set(stage vertex)
        elseif(entry MATCHES "^fs_")
            set(stage fragment)
        else()
            set(stage compute)
        endif()
        set(source "${PROJECT_SOURCE_DIR}/shaders/native/woby.slang")
        set(common -warnings-as-errors all -entry ${entry} -stage ${stage} -fvk-use-c-layout -matrix-layout-row-major
            -I "${woby_ngapi_SOURCE_DIR}/include" -I "${woby_ngapi_SOURCE_DIR}/utility/include")
        if(APPLE)
            set(output "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/metal/${entry}.bin")
            set(intermediate "${CMAKE_CURRENT_BINARY_DIR}/native-shaders/${entry}")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/metal" "${CMAKE_CURRENT_BINARY_DIR}/native-shaders"
                COMMAND "${WOBY_SLANGC}" "${source}" -target metal -DNOGRAPHICSAPI_METAL ${common} -o "${intermediate}.metal"
                COMMAND "${WOBY_XCRUN}" -sdk macosx metal -std=metal4.0 -c "${intermediate}.metal" -o "${intermediate}.air"
                COMMAND "${WOBY_XCRUN}" -sdk macosx metallib "${intermediate}.air" -o "${output}"
                DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/shaders/native/root.h"
                    "${woby_ngapi_SOURCE_DIR}/include/NoGraphicsAPI/shader.slang"
                    "${woby_ngapi_SOURCE_DIR}/utility/include/NoGraphicsAPIUtility/shader_types.h" "${WOBY_SLANGC}" VERBATIM)
        else()
            set(output "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/spirv/${entry}.bin")
            add_custom_command(OUTPUT "${output}"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/spirv"
                COMMAND "${WOBY_SLANGC}" "${source}" -target spirv -profile spirv_1_5 -emit-spirv-directly
                    -fvk-use-entrypoint-name -DWOBY_VULKAN ${common} -capability spvDescriptorHeapEXT -o "${output}"
                COMMAND "${WOBY_SPIRV_VAL}" --target-env vulkan1.4 --scalar-block-layout "${output}"
                DEPENDS "${source}" "${PROJECT_SOURCE_DIR}/shaders/native/root.h"
                    "${woby_ngapi_SOURCE_DIR}/include/NoGraphicsAPI/shader.slang"
                    "${woby_ngapi_SOURCE_DIR}/utility/include/NoGraphicsAPIUtility/shader_types.h" "${WOBY_SLANGC}" VERBATIM)
        endif()
        list(APPEND outputs "${output}")
    endforeach()
    set_property(TARGET ${target} PROPERTY WOBY_SHADER_OUTPUTS "${outputs}")
    add_custom_target(${target}_native_shaders DEPENDS ${outputs})
    add_dependencies(${target} ${target}_native_shaders)
endfunction()
