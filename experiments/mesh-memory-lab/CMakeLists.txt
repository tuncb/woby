add_library(mesh_memory_trace STATIC
    trace.cpp example.cpp pipeline.cpp ui_operations.cpp workflow.cpp
    ${PROJECT_SOURCE_DIR}/src/obj_mesh.cpp
    ${PROJECT_SOURCE_DIR}/src/model_mesh.cpp
    ${PROJECT_SOURCE_DIR}/src/obj_freeform.cpp
    ${PROJECT_SOURCE_DIR}/src/freeform.cpp
    ${PROJECT_SOURCE_DIR}/src/freeform_trim.cpp)
target_include_directories(mesh_memory_trace PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
target_include_directories(mesh_memory_trace SYSTEM PRIVATE "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include")
target_link_libraries(mesh_memory_trace PRIVATE CDT::CDT Threads::Threads nlohmann_json::nlohmann_json)
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

# Keep the library next to the executable, independent of the working directory.
file(GLOB workflow_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/workflows/*.meshflow")
set(workflow_outputs)
foreach(source IN LISTS workflow_files)
    get_filename_component(name "${source}" NAME)
    set(destination "${WOBY_RUNTIME_DIRECTORY}/workflows/${name}")
    add_custom_command(OUTPUT "${destination}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${WOBY_RUNTIME_DIRECTORY}/workflows"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source}" "${destination}"
        DEPENDS "${source}" VERBATIM)
    list(APPEND workflow_outputs "${destination}")
endforeach()
add_custom_target(mesh_memory_workflows DEPENDS ${workflow_outputs})
add_dependencies(mesh_memory_lab mesh_memory_workflows)

if(BUILD_TESTING)
    find_package(doctest CONFIG REQUIRED)
    add_executable(mesh_memory_lab_tests trace_tests.cpp workflow_tests.cpp ${PROJECT_SOURCE_DIR}/tests/dummy_tests.cpp)
    target_link_libraries(mesh_memory_lab_tests PRIVATE mesh_memory_trace doctest::doctest)
    target_compile_definitions(mesh_memory_lab_tests PRIVATE MESH_LAB_SAMPLE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/samples")
    target_compile_definitions(mesh_memory_lab_tests PRIVATE MESH_LAB_WORKFLOW_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/workflows")
    woby_enable_project_warnings(mesh_memory_lab_tests)
    add_test(NAME mesh_memory_lab_unit COMMAND mesh_memory_lab_tests)
    if(WIN32)
        add_test(NAME mesh_memory_lab_windows_launch COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/windows_launch_tests.py" "$<TARGET_FILE:mesh_memory_lab>")
        set_tests_properties(mesh_memory_lab_windows_launch PROPERTIES TIMEOUT 45)
    endif()
    if(WOBY_TEST_HEADLESS)
        add_test(NAME mesh_memory_lab_gpu COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/workflow_render_tests.py" "$<TARGET_FILE:mesh_memory_lab>"
            "${CMAKE_CURRENT_SOURCE_DIR}/workflows" --smoke)
        set_tests_properties(mesh_memory_lab_gpu PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
        add_test(NAME mesh_memory_lab_workflow_render COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/workflow_render_tests.py" "$<TARGET_FILE:mesh_memory_lab>"
            "${CMAKE_CURRENT_SOURCE_DIR}/workflows")
        set_tests_properties(mesh_memory_lab_workflow_render PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
    endif()
endif()
