if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The corner mapping research probes require Windows and MSVC.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(mapping_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/split_mapping.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/index_table.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/compact_table.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/compact_mapping.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/release_mapping.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/fast_resolve.inc"
    "${PROJECT_SOURCE_DIR}/experiments/obj-profile/generate.py"
    "${PROJECT_SOURCE_DIR}/experiments/obj-mapping-reference/obj_mesh.cpp"
    "${PROJECT_SOURCE_DIR}/experiments/obj-mapping-reference/obj_corner_mapping.h"
    "${PROJECT_SOURCE_DIR}/experiments/obj-mapping-reference/mapping_reference.h"
    "${PROJECT_SOURCE_DIR}/experiments/obj-mapping-reference/profile_generate.py"
    "${PROJECT_SOURCE_DIR}/experiments/parser-throughput/generate.py"
    "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/rapidobj.hpp"
    "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/prototype.hpp")
foreach(source IN ITEMS obj_mesh model_mesh scene_mesh_preparation point_cloud surface_annotation ui_state)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/${source}.cpp")
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}" "${mapping_generated}" COMMAND_ERROR_IS_FATAL ANY)
foreach(variant IN ITEMS baseline pointer split dense compact release compact_release fast fast_release)
    set(target "woby_corner_mapping_${variant}")
    add_executable(${target} main.cpp "${PROJECT_SOURCE_DIR}/experiments/obj-profile/profile.cpp"
        "${mapping_generated}/obj_${variant}.cpp" "${mapping_generated}/base/model_mesh.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/experiments/obj-profile" "${mapping_generated}/base")
    target_link_libraries(${target} PRIVATE woby_core nlohmann_json::nlohmann_json psapi dbghelp)
    target_compile_definitions(${target} PRIVATE CORNER_MAPPING_VARIANT="${variant}")
    if(variant STREQUAL "pointer")
        target_compile_definitions(${target} PRIVATE OBJ_PROFILE_UV_PTR)
    elseif(variant STREQUAL "dense")
        target_compile_definitions(${target} PRIVATE CORNER_MAPPING_DENSE)
    elseif(variant MATCHES "^(compact|fast)")
        target_compile_definitions(${target} PRIVATE CORNER_MAPPING_COMPACT)
    endif()
    if(variant MATCHES "release$")
        target_compile_definitions(${target} PRIVATE CORNER_MAPPING_RELEASE)
    endif()
    woby_enable_project_warnings(${target})
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    list(APPEND woby_runtime_targets ${target})
endforeach()
set(woby_runtime_targets "${woby_runtime_targets}" PARENT_SCOPE)
if(BUILD_TESTING)
    add_test(NAME woby_corner_mapping_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests.py" --bin "$<TARGET_FILE_DIR:woby_corner_mapping_baseline>")
    set_tests_properties(woby_corner_mapping_contract PROPERTIES LABELS "unit;cpu" TIMEOUT 180)
    add_test(NAME woby_corner_mapping_runner COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/runner_tests.py")
    set_tests_properties(woby_corner_mapping_runner PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    add_test(NAME woby_corner_mapping_storage_stats COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/storage_stats_tests.py")
    set_tests_properties(woby_corner_mapping_storage_stats PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
endif()
