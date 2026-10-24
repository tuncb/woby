if(NOT WIN32)
    message(FATAL_ERROR "The point pipeline experiment currently measures Windows Vulkan only")
endif()
set(outputs)
foreach(entry IN ITEMS vs_pipeline_combined cs_pipeline_claim_clear cs_pipeline_seed cs_pipeline_capture)
    set(stage compute)
    if(entry MATCHES "^vs_")
        set(stage vertex)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${entry}.bin")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${WOBY_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/pipeline.slang"
            -target spirv -profile spirv_1_5 -emit-spirv-directly -fvk-use-entrypoint-name
            -entry ${entry} -stage ${stage} -fvk-use-c-layout -matrix-layout-row-major
            -DWOBY_VULKAN -DWOBY_POINT_ATOMIC64 -warnings-as-errors all
            -I "${woby_ngapi_SOURCE_DIR}/include" -I "${woby_ngapi_SOURCE_DIR}/utility/include"
            -capability spvDescriptorHeapEXT -o "${output}"
        COMMAND "${WOBY_SPIRV_VAL}" --target-env vulkan1.4 --scalar-block-layout "${output}"
        DEPENDS pipeline.slang root.h "${PROJECT_SOURCE_DIR}/shaders/native/woby.slang"
            "${PROJECT_SOURCE_DIR}/shaders/native/root.h" "${PROJECT_SOURCE_DIR}/shaders/native/opaque_points.slang"
            "${PROJECT_SOURCE_DIR}/shaders/native/opaque_points.h" VERBATIM)
    list(APPEND outputs "${output}")
endforeach()
add_custom_target(woby_point_pipeline_shaders DEPENDS ${outputs})
add_executable(woby_point_pipeline main.cpp)
target_compile_features(woby_point_pipeline PRIVATE cxx_std_20)
target_include_directories(woby_point_pipeline PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(woby_point_pipeline PRIVATE woby_core NoGraphicsAPIUtility::allocators
    NoGraphicsAPIUtility::textures NoGraphicsAPIUtility::uploads NoGraphicsAPIUtility::types)
target_compile_definitions(woby_point_pipeline PRIVATE
    WOBY_PIPELINE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_dependencies(woby_point_pipeline woby woby_point_pipeline_shaders)
woby_enable_project_warnings(woby_point_pipeline)
if(BUILD_TESTING AND WOBY_TEST_HEADLESS)
    add_test(NAME woby_point_pipeline_gpu COMMAND woby_point_pipeline --self-test)
    set_tests_properties(woby_point_pipeline_gpu PROPERTIES LABELS graphics TIMEOUT 120 RUN_SERIAL TRUE
        ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
endif()
