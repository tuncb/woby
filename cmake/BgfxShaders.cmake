find_program(BGFX_SHADERC_EXECUTABLE
    NAMES shaderc shaderc.exe
    HINTS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/bgfx"
        "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/tools/bgfx"
        "$ENV{VCPKG_ROOT}/installed/${VCPKG_HOST_TRIPLET}/tools/bgfx"
    DOC "Path to the bgfx shader compiler"
)

find_path(BGFX_SHADER_INCLUDE_DIR
    NAMES bgfx/bgfx_shader.sh
    HINTS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/include"
        "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/include"
        "$ENV{VCPKG_ROOT}/installed/${VCPKG_HOST_TRIPLET}/include"
    DOC "Path containing bgfx/bgfx_shader.sh"
)

if(NOT BGFX_SHADERC_EXECUTABLE)
    message(FATAL_ERROR "bgfx shaderc was not found. Install bgfx with the vcpkg tools feature, for example bgfx[tools].")
endif()

if(NOT BGFX_SHADER_INCLUDE_DIR)
    message(FATAL_ERROR "bgfx shader include directory was not found. Expected to find bgfx/bgfx_shader.sh.")
endif()

set(WOBY_BGFX_SHADER_PROFILES)

if(WIN32)
    list(APPEND WOBY_BGFX_SHADER_PROFILES "dx11|windows|s_5_0")
elseif(APPLE)
    list(APPEND WOBY_BGFX_SHADER_PROFILES "metal|osx|metal")
    list(APPEND WOBY_BGFX_SHADER_PROFILES "glsl|osx|120")
else()
    list(APPEND WOBY_BGFX_SHADER_PROFILES "spirv|linux|spirv")
    list(APPEND WOBY_BGFX_SHADER_PROFILES "glsl|linux|120")
    list(APPEND WOBY_BGFX_SHADER_PROFILES "essl|linux|100_es")
endif()

function(woby_compile_bgfx_shader target)
    set(options)
    set(one_value_args SOURCE TYPE OUTPUT_NAME VARYING)
    set(multi_value_args)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT ARG_SOURCE OR NOT ARG_TYPE OR NOT ARG_OUTPUT_NAME OR NOT ARG_VARYING)
        message(FATAL_ERROR "woby_compile_bgfx_shader requires SOURCE, TYPE, OUTPUT_NAME, and VARYING.")
    endif()

    set(outputs)

    foreach(profile_spec IN LISTS WOBY_BGFX_SHADER_PROFILES)
        string(REPLACE "|" ";" profile_parts "${profile_spec}")
        list(GET profile_parts 0 shader_folder)
        list(GET profile_parts 1 shader_platform)
        list(GET profile_parts 2 shader_profile)

        set(output_file "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/${shader_folder}/${ARG_OUTPUT_NAME}.bin")

        add_custom_command(
            OUTPUT "${output_file}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/assets/shaders/${shader_folder}"
            COMMAND "${BGFX_SHADERC_EXECUTABLE}"
                    -f "${ARG_SOURCE}"
                    -o "${output_file}"
                    --type "${ARG_TYPE}"
                    --platform "${shader_platform}"
                    --profile "${shader_profile}"
                    --varyingdef "${ARG_VARYING}"
                    -i "${BGFX_SHADER_INCLUDE_DIR}"
            DEPENDS "${ARG_SOURCE}" "${ARG_VARYING}"
            COMMENT "Compiling ${ARG_OUTPUT_NAME} for ${shader_folder}"
            VERBATIM
        )

        list(APPEND outputs "${output_file}")
    endforeach()

    set(shader_target "${target}_${ARG_OUTPUT_NAME}_shader")
    add_custom_target("${shader_target}" DEPENDS ${outputs})
    add_dependencies("${target}" "${shader_target}")
endfunction()
