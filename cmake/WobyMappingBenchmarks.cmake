# Research-only generated variants. Application sources remain unchanged.
if(NOT BUILD_TESTING)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WobyPython.cmake")
endif()
set(mapping_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/vertex_mapping")
set(mapping_dir "${CMAKE_CURRENT_BINARY_DIR}/mapping-probe")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${mapping_tests}/generate.py" "${mapping_tests}/legacy.h" "${mapping_tests}/diagnostics.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/obj_mesh.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/model_mesh.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/surface_annotation.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_state.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/scene_renderer.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${mapping_tests}/generate.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${mapping_dir}" COMMAND_ERROR_IS_FATAL ANY)
foreach(mode IN ITEMS baseline hybrid adaptive diagnostic split1 split8)
    set(target "woby_mapping_${mode}")
    add_executable(${target} "${mapping_tests}/main.cpp" "${mapping_dir}/obj_${mode}.cpp"
        "${mapping_dir}/model_mesh.cpp" "${mapping_dir}/point_ranges.cpp" "${mapping_dir}/annotation_cache.cpp"
        "${mapping_dir}/group_state.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/hash_utils.cpp")
    target_include_directories(${target} PRIVATE "${mapping_tests}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(${target} SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json Threads::Threads bgfx::bgfx)
    target_compile_definitions(${target} PRIVATE MAPPING_VARIANT="${mode}")
    if(MSVC)
        target_compile_options(${target} PRIVATE /Zc:__cplusplus /Zc:preprocessor)
    endif()
    if(WIN32)
        target_link_libraries(${target} PRIVATE psapi)
    endif()
    woby_enable_project_warnings(${target})
    list(APPEND woby_runtime_targets ${target})
endforeach()
if(BUILD_TESTING)
    add_test(NAME woby_mapping_contract COMMAND "${Python3_EXECUTABLE}"
        "${mapping_tests}/contract_test.py" "$<TARGET_FILE_DIR:woby_mapping_baseline>")
endif()
