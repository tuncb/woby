add_library(mesh_memory_trace STATIC
    trace.cpp example.cpp pipeline.cpp ui_operations.cpp
    ${PROJECT_SOURCE_DIR}/src/obj_mesh.cpp
    ${PROJECT_SOURCE_DIR}/src/model_mesh.cpp
    ${PROJECT_SOURCE_DIR}/src/obj_freeform.cpp
    ${PROJECT_SOURCE_DIR}/src/freeform.cpp
    ${PROJECT_SOURCE_DIR}/src/freeform_trim.cpp)
target_include_directories(mesh_memory_trace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
target_include_directories(mesh_memory_trace SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include")
target_link_libraries(mesh_memory_trace PRIVATE CDT::CDT Threads::Threads)
woby_enable_project_warnings(mesh_memory_trace)

add_executable(mesh_memory_lab main.cpp ui.cpp gpu_runtime.cpp
    ${PROJECT_SOURCE_DIR}/src/console.cpp
    ${PROJECT_SOURCE_DIR}/src/imgui_graphics.cpp
    ${PROJECT_SOURCE_DIR}/src/graphics_helpers.cpp)
if(WIN32)
    set_target_properties(mesh_memory_lab PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_link_libraries(mesh_memory_lab PRIVATE mesh_memory_trace woby_graphics woby_math SDL3::SDL3 imgui::imgui)
add_dependencies(mesh_memory_lab woby_assets)
woby_enable_project_warnings(mesh_memory_lab)

if(BUILD_TESTING)
    find_package(doctest CONFIG REQUIRED)
    add_executable(mesh_memory_lab_tests trace_tests.cpp ${PROJECT_SOURCE_DIR}/tests/dummy_tests.cpp)
    target_link_libraries(mesh_memory_lab_tests PRIVATE mesh_memory_trace doctest::doctest)
    target_compile_definitions(mesh_memory_lab_tests PRIVATE MESH_LAB_SAMPLE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/samples")
    woby_enable_project_warnings(mesh_memory_lab_tests)
    add_test(NAME mesh_memory_lab_unit COMMAND mesh_memory_lab_tests)
    if(WIN32)
        add_test(NAME mesh_memory_lab_windows_launch COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_launch_tests.py" "$<TARGET_FILE:mesh_memory_lab>")
        set_tests_properties(mesh_memory_lab_windows_launch PROPERTIES TIMEOUT 45)
    endif()
    if(WOBY_TEST_HEADLESS)
        add_test(NAME mesh_memory_lab_gpu COMMAND mesh_memory_lab --smoke)
        set_tests_properties(mesh_memory_lab_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    endif()
endif()
