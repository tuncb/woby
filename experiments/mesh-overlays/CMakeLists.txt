if(NOT WIN32)
    message(FATAL_ERROR "The overlay experiment currently measures Windows Vulkan only")
endif()


set(shader_outputs)
foreach(entry IN ITEMS vs_mesh vs_color vs_point_sprite fs_mesh fs_color fs_point_sprite
        fs_marker_mesh fs_marker_line fs_marker_point vs_overlay_pull fs_overlay_pull
        fs_overlay_native fs_overlay_native_ids fs_overlay_pull_ids cs_capture_ids
        cs_capture_depth vs_point_compacted cs_cull_depth cs_cull_reduce cs_cull_classify
        cs_cull_scan cs_cull_add cs_cull_arguments cs_cull_scatter
        cs_point_clear cs_point_raster cs_point_ids vs_point_resolve fs_point_resolve)
    if(entry MATCHES "^vs_")
        set(stage vertex)
    elseif(entry MATCHES "^fs_")
        set(stage fragment)
    else()
        set(stage compute)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${entry}.spv")
    set(source overlay.slang)
    if(entry MATCHES "^cs_cull_")
        set(source gpu_culling.slang)
    endif()
    if(entry MATCHES "^cs_point_" OR entry MATCHES "point_resolve$")
        set(source point.slang)
    endif()
    set(capabilities -capability spvDescriptorHeapEXT)
    if(entry MATCHES "^fs_overlay_native")
        list(APPEND capabilities -capability spvFragmentBarycentricKHR)
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${WOBY_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}"
            -target spirv -profile spirv_1_5 -emit-spirv-directly -fvk-use-entrypoint-name
            -entry ${entry} -stage ${stage} -fvk-use-c-layout -matrix-layout-row-major
            -DWOBY_VULKAN -warnings-as-errors all
            -I "${PROJECT_SOURCE_DIR}/shaders/native"
            -I "${woby_ngapi_SOURCE_DIR}/include" -I "${woby_ngapi_SOURCE_DIR}/utility/include"
            ${capabilities} -o "${output}"
        COMMAND "${WOBY_SPIRV_VAL}" --target-env vulkan1.4 --scalar-block-layout "${output}"
        DEPENDS "${source}" gpu_culling_root.h point_root.h "${PROJECT_SOURCE_DIR}/shaders/native/woby.slang"
            "${PROJECT_SOURCE_DIR}/shaders/native/root.h" VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()
add_custom_target(woby_overlay_shaders DEPENDS ${shader_outputs})

add_library(woby_overlay_experiment STATIC renderer.cpp visibility.cpp gpu_culling.cpp ../../src/point_cloud.cpp point_renderer.cpp
    ../../src/model_mesh.cpp ../../src/obj_mesh.cpp ../../src/obj_freeform.cpp
    ../../src/freeform.cpp ../../src/freeform_trim.cpp ../../src/scene_mesh_preparation.cpp ../../src/camera.cpp)
target_include_directories(woby_overlay_experiment PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/shaders/native")
target_include_directories(woby_overlay_experiment SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include")
target_link_libraries(woby_overlay_experiment PUBLIC NoGraphicsAPI::NoGraphicsAPI
    NoGraphicsAPIUtility::allocators NoGraphicsAPIUtility::textures NoGraphicsAPIUtility::uploads
    NoGraphicsAPIUtility::types SDL3::SDL3 woby_math Threads::Threads CDT::CDT Boost::multiprecision)
target_compile_definitions(woby_overlay_experiment PRIVATE NOMINMAX
    WOBY_OVERLAY_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
woby_enable_project_warnings(woby_overlay_experiment)
add_dependencies(woby_overlay_experiment woby_overlay_shaders)
add_executable(woby_overlay_prototype main.cpp visibility_run.cpp gpu_culling_run.cpp)
target_link_libraries(woby_overlay_prototype PRIVATE woby_overlay_experiment nlohmann_json::nlohmann_json)
woby_enable_project_warnings(woby_overlay_prototype)
add_executable(woby_point_prototype point_main.cpp)
target_link_libraries(woby_point_prototype PRIVATE woby_overlay_experiment nlohmann_json::nlohmann_json)
woby_enable_project_warnings(woby_point_prototype)
if(BUILD_TESTING)
    find_package(doctest CONFIG REQUIRED)
    add_executable(woby_overlay_tests tests.cpp gpu_culling_tests.cpp point_tests.cpp)
    target_link_libraries(woby_overlay_tests PRIVATE woby_overlay_experiment doctest::doctest)
    woby_enable_project_warnings(woby_overlay_tests)
    add_test(NAME woby_overlay_logic COMMAND woby_overlay_tests --test-case-exclude=*GPU*)
    find_program(WOBY_OVERLAY_UV uv REQUIRED)
    add_test(NAME woby_overlay_analysis COMMAND "${WOBY_OVERLAY_UV}" run
        "${CMAKE_CURRENT_SOURCE_DIR}/analysis_tests.py")
    if(WOBY_TEST_HEADLESS)
        add_test(NAME woby_overlay_gpu COMMAND woby_overlay_tests --test-case=*GPU*)
        set_tests_properties(woby_overlay_gpu PROPERTIES LABELS graphics TIMEOUT 120 RUN_SERIAL TRUE
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endif()
endif()
