if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The OBJ resource probe requires Windows and MSVC.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
target_link_options(woby PRIVATE "$<$<CONFIG:Release>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/production-woby.map>")
find_path(OBJ_PROFILE_NVTX_INCLUDE nvtx3/nvToolsExt.h
    HINTS "C:/Program Files/NVIDIA Corporation/Nsight Systems 2026.5.1/target-windows-x64/nvtx/include")
find_path(OBJ_PROFILE_ITT_INCLUDE ittnotify.h
    HINTS "C:/Program Files (x86)/Intel/oneAPI/vtune/2025.3/sdk/include")
find_library(OBJ_PROFILE_ITT_LIBRARY libittnotify
    HINTS "C:/Program Files (x86)/Intel/oneAPI/vtune/2025.3/sdk/lib64")
set(profile_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(profile_sources obj_mesh model_mesh scene_mesh_preparation point_cloud surface_annotation ui_state)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}/experiments/parser-throughput/generate.py"
    "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/rapidobj.hpp"
    "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/prototype.hpp")
foreach(name IN LISTS profile_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/${name}.cpp")
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}" "${profile_generated}" COMMAND_ERROR_IS_FATAL ANY)
foreach(isa IN ITEMS sse2 avx2 diagnostic uvcheck uvptr)
    set(target "woby_obj_profile_${isa}")
    add_executable(${target} main.cpp profile.cpp deep.cpp
        "${profile_generated}/obj_mesh.cpp" "${profile_generated}/model_mesh.cpp"
        "${profile_generated}/scene_mesh_preparation.cpp" "${profile_generated}/point_cloud.cpp"
        "${profile_generated}/surface_annotation.cpp" "${profile_generated}/ui_state.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${profile_generated}")
    if(OBJ_PROFILE_NVTX_INCLUDE)
        target_include_directories(${target} SYSTEM PRIVATE "${OBJ_PROFILE_NVTX_INCLUDE}")
        target_compile_definitions(${target} PRIVATE OBJ_PROFILE_NVTX)
    endif()
    if(OBJ_PROFILE_ITT_INCLUDE AND OBJ_PROFILE_ITT_LIBRARY)
        target_include_directories(${target} SYSTEM PRIVATE "${OBJ_PROFILE_ITT_INCLUDE}")
        target_link_libraries(${target} PRIVATE "${OBJ_PROFILE_ITT_LIBRARY}")
        target_compile_definitions(${target} PRIVATE OBJ_PROFILE_ITT)
    endif()
    target_link_libraries(${target} PRIVATE woby_core nlohmann_json::nlohmann_json psapi dbghelp)
    woby_enable_project_warnings(${target})
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi> $<$<CONFIG:Release>:/FAs>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    if(isa STREQUAL "avx2")
        # The production x64 baseline is AVX2.
    elseif(isa STREQUAL "diagnostic")
        target_compile_definitions(${target} PRIVATE OBJ_PROFILE_DIAGNOSTIC)
    elseif(isa STREQUAL "uvcheck")
        target_compile_definitions(${target} PRIVATE OBJ_PROFILE_UV_CHECK)
    elseif(isa STREQUAL "uvptr")
        target_compile_definitions(${target} PRIVATE OBJ_PROFILE_UV_PTR)
    endif()
    if(NOT isa STREQUAL "avx2")
        set_target_properties(${target} PROPERTIES WOBY_LEGACY_ISA ON)
    endif()
    list(APPEND woby_runtime_targets ${target})
endforeach()
set(upload_generated "${CMAKE_CURRENT_BINARY_DIR}/upload-viewer")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gpu_viewer_generate.py"
    "${PROJECT_SOURCE_DIR}/src/main.cpp" "${PROJECT_SOURCE_DIR}/src/frame_pacing.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/gpu_viewer_generate.py"
    "${PROJECT_SOURCE_DIR}" "${upload_generated}" COMMAND_ERROR_IS_FATAL ANY)
add_executable(woby_obj_upload_viewer WIN32 "${upload_generated}/main.cpp" "${upload_generated}/frame_pacing.cpp"
    "${PROJECT_SOURCE_DIR}/src/console.cpp" "${PROJECT_SOURCE_DIR}/src/renderer_startup.cpp"
    "${PROJECT_SOURCE_DIR}/src/imgui_graphics.cpp" "${PROJECT_SOURCE_DIR}/src/scene_screenshot.cpp"
    "${PROJECT_SOURCE_DIR}/packaging/windows/long_paths.manifest")
target_include_directories(woby_obj_upload_viewer PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(woby_obj_upload_viewer PRIVATE WOBY_VERSION="${PROJECT_VERSION}" WOBY_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(woby_obj_upload_viewer PRIVATE woby_core spdlog::spdlog)
woby_enable_project_warnings(woby_obj_upload_viewer)
woby_enable_enum_switch_warnings(woby_obj_upload_viewer)
list(APPEND woby_runtime_targets woby_obj_upload_viewer)
add_executable(woby_obj_fast_pacing_viewer WIN32 "${PROJECT_SOURCE_DIR}/src/main.cpp" "${upload_generated}/frame_pacing.cpp"
    "${PROJECT_SOURCE_DIR}/src/console.cpp" "${PROJECT_SOURCE_DIR}/src/renderer_startup.cpp"
    "${PROJECT_SOURCE_DIR}/src/imgui_graphics.cpp" "${PROJECT_SOURCE_DIR}/src/scene_screenshot.cpp"
    "${PROJECT_SOURCE_DIR}/packaging/windows/long_paths.manifest")
target_include_directories(woby_obj_fast_pacing_viewer PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(woby_obj_fast_pacing_viewer PRIVATE WOBY_VERSION="${PROJECT_VERSION}" WOBY_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(woby_obj_fast_pacing_viewer PRIVATE woby_core spdlog::spdlog)
woby_enable_project_warnings(woby_obj_fast_pacing_viewer)
woby_enable_enum_switch_warnings(woby_obj_fast_pacing_viewer)
list(APPEND woby_runtime_targets woby_obj_fast_pacing_viewer)
set(woby_runtime_targets "${woby_runtime_targets}" PARENT_SCOPE)

add_executable(woby_annotation_benchmark annotation.cpp profile.cpp "${profile_generated}/surface_annotation.cpp")
target_include_directories(woby_annotation_benchmark PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(woby_annotation_benchmark PRIVATE woby_core nlohmann_json::nlohmann_json psapi dbghelp)
woby_enable_project_warnings(woby_annotation_benchmark)
list(APPEND woby_runtime_targets woby_annotation_benchmark)
set(woby_runtime_targets "${woby_runtime_targets}" PARENT_SCOPE)

set(e2e_generated "${CMAKE_CURRENT_BINARY_DIR}/e2e-viewer")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/e2e_generate.py"
    "${PROJECT_SOURCE_DIR}/src/obj_corner_mapping.h"
    "${PROJECT_SOURCE_DIR}/src/post_loading.cpp"
    "${PROJECT_SOURCE_DIR}/src/graphics_upload.cpp"
    "${PROJECT_SOURCE_DIR}/src/model_load.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/e2e_generate.py"
    "${PROJECT_SOURCE_DIR}" "${e2e_generated}" COMMAND_ERROR_IS_FATAL ANY)
set(e2e_sources)
foreach(name IN LISTS profile_sources)
    list(APPEND e2e_sources "${e2e_generated}/${name}.cpp")
endforeach()
add_executable(woby_obj_e2e_viewer WIN32 e2e_trace.cpp ${e2e_sources}
    "${e2e_generated}/main.cpp" "${e2e_generated}/post_loading.cpp"
    "${e2e_generated}/graphics_upload.cpp" "${e2e_generated}/model_load.cpp"
    "${PROJECT_SOURCE_DIR}/src/console.cpp" "${PROJECT_SOURCE_DIR}/src/renderer_startup.cpp"
    "${PROJECT_SOURCE_DIR}/src/imgui_graphics.cpp" "${PROJECT_SOURCE_DIR}/src/scene_screenshot.cpp"
    "${PROJECT_SOURCE_DIR}/packaging/windows/long_paths.manifest")
target_include_directories(woby_obj_e2e_viewer PRIVATE "${e2e_generated}" "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(woby_obj_e2e_viewer PRIVATE WOBY_VERSION="${PROJECT_VERSION}" WOBY_BUILD_CONFIG="$<CONFIG>")
target_compile_options(woby_obj_e2e_viewer PRIVATE $<$<CONFIG:Release>:/Zi> $<$<CONFIG:Release>:/FAs>)
target_link_options(woby_obj_e2e_viewer PRIVATE $<$<CONFIG:Release>:/DEBUG>
    "$<$<CONFIG:Release>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/e2e-woby.map>")
target_link_libraries(woby_obj_e2e_viewer PRIVATE woby_core spdlog::spdlog psapi NoGraphicsAPIUtility_uploads Vulkan::Vulkan)
if(OBJ_PROFILE_NVTX_INCLUDE)
    target_include_directories(woby_obj_e2e_viewer SYSTEM PRIVATE "${OBJ_PROFILE_NVTX_INCLUDE}")
    target_compile_definitions(woby_obj_e2e_viewer PRIVATE OBJ_PROFILE_NVTX)
endif()
if(OBJ_PROFILE_ITT_INCLUDE AND OBJ_PROFILE_ITT_LIBRARY)
    target_include_directories(woby_obj_e2e_viewer SYSTEM PRIVATE "${OBJ_PROFILE_ITT_INCLUDE}")
    target_link_libraries(woby_obj_e2e_viewer PRIVATE "${OBJ_PROFILE_ITT_LIBRARY}")
    target_compile_definitions(woby_obj_e2e_viewer PRIVATE OBJ_PROFILE_ITT)
endif()
woby_enable_project_warnings(woby_obj_e2e_viewer)
woby_enable_enum_switch_warnings(woby_obj_e2e_viewer)
list(APPEND woby_runtime_targets woby_obj_e2e_viewer)
set(woby_runtime_targets "${woby_runtime_targets}" PARENT_SCOPE)
if(BUILD_TESTING)
    add_executable(woby_obj_profile_pacing_test pacing_test.cpp "${upload_generated}/frame_pacing.cpp")
    target_include_directories(woby_obj_profile_pacing_test PRIVATE "${PROJECT_SOURCE_DIR}/src")
    woby_enable_project_warnings(woby_obj_profile_pacing_test)
    add_test(NAME woby_obj_profile_pacing COMMAND woby_obj_profile_pacing_test)
    set_tests_properties(woby_obj_profile_pacing PROPERTIES LABELS "unit;cpu")
    add_test(NAME woby_obj_profile_tooling COMMAND "${WOBY_UV_EXECUTABLE}" run "${CMAKE_CURRENT_SOURCE_DIR}/tooling_tests.py")
    set_tests_properties(woby_obj_profile_tooling PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    add_test(NAME woby_obj_e2e_tooling COMMAND "${WOBY_UV_EXECUTABLE}" run "${CMAKE_CURRENT_SOURCE_DIR}/e2e_tests.py"
        --viewer "$<TARGET_FILE:woby_obj_e2e_viewer>")
    set_tests_properties(woby_obj_e2e_tooling PROPERTIES LABELS "unit;cpu" TIMEOUT 120)
    add_test(NAME woby_obj_profile_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests.py"
        "$<TARGET_FILE:woby_obj_profile_sse2>" "$<TARGET_FILE:woby_obj_profile_diagnostic>" "$<TARGET_FILE:woby_obj_profile_uvcheck>" "$<TARGET_FILE:woby_obj_profile_uvptr>")
    set_tests_properties(woby_obj_profile_contract PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
endif()

add_executable(woby_loading_audit_kernels "${PROJECT_SOURCE_DIR}/experiments/loading-audit/kernels.cpp")
target_include_directories(woby_loading_audit_kernels PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(woby_loading_audit_kernels PRIVATE woby_core nlohmann_json::nlohmann_json)
woby_enable_project_warnings(woby_loading_audit_kernels)
if(BUILD_TESTING)
    add_test(NAME woby_loading_audit_replay COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/tests.py" "$<TARGET_FILE:woby_loading_audit_kernels>")
    set_tests_properties(woby_loading_audit_replay PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    add_test(NAME woby_loading_audit_accounting COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/analysis_tests.py")
    set_tests_properties(woby_loading_audit_accounting PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    add_test(NAME woby_loading_audit_provenance COMMAND "${WOBY_UV_EXECUTABLE}" run
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/runner_tests.py")
    set_tests_properties(woby_loading_audit_provenance PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    add_test(NAME woby_parser_simd_accounting COMMAND "${WOBY_UV_EXECUTABLE}" run
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/parser_simd_tests.py")
    set_tests_properties(woby_parser_simd_accounting PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    add_test(NAME woby_parser_microarchitecture_accounting COMMAND "${WOBY_UV_EXECUTABLE}" run
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/micro_tests.py")
    set_tests_properties(woby_parser_microarchitecture_accounting PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
endif()
