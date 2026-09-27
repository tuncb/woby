# Isolated experiment: no alternate rendering mode is linked into the viewer.
if(NOT BUILD_TESTING)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WobyPython.cmake")
endif()
set(vertex_probe_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/vertex_drawing")
set(vertex_probe_dir "${CMAKE_CURRENT_BINARY_DIR}/vertex-probe")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${vertex_probe_tests}/generate.py" "${CMAKE_CURRENT_SOURCE_DIR}/src/scene_renderer.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${vertex_probe_tests}/generate.py"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${vertex_probe_dir}" COMMAND_ERROR_IS_FATAL ANY)
add_executable(woby_vertex_benchmark "${vertex_probe_tests}/main.cpp"
    src/model_mesh.cpp src/obj_mesh.cpp src/bgfx_helpers.cpp)
target_include_directories(woby_vertex_benchmark PRIVATE "${vertex_probe_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(woby_vertex_benchmark SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
target_link_libraries(woby_vertex_benchmark PRIVATE bgfx::bgfx nlohmann_json::nlohmann_json Threads::Threads)
foreach(dependency IN ITEMS bgfx::bx bgfx::bimg)
    if(TARGET "${dependency}")
        target_link_libraries(woby_vertex_benchmark PRIVATE "${dependency}")
    endif()
endforeach()
if(WIN32)
    target_link_libraries(woby_vertex_benchmark PRIVATE psapi)
endif()
target_compile_definitions(woby_vertex_benchmark PRIVATE
    VERTEX_PROBE_ASSETS="${CMAKE_CURRENT_BINARY_DIR}/assets")
woby_enable_project_warnings(woby_vertex_benchmark)
woby_compile_bgfx_shader(woby_vertex_benchmark
    SOURCE "${vertex_probe_tests}/point_pull.vert.sc" TYPE vertex OUTPUT_NAME vs_point_pull
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc")
woby_compile_bgfx_shader(woby_vertex_benchmark
    SOURCE "${vertex_probe_tests}/point_pull_flat.vert.sc" TYPE vertex OUTPUT_NAME vs_point_pull_flat
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc")
get_target_property(vertex_probe_shaders woby_vertex_benchmark WOBY_SHADER_OUTPUTS)
add_custom_target(woby_vertex_shaders DEPENDS ${vertex_probe_shaders})
add_dependencies(woby_vertex_benchmark woby_vertex_shaders)
# Reuse the production shader build without making private copies.
add_dependencies(woby_vertex_benchmark woby)
list(APPEND woby_runtime_targets woby_vertex_benchmark)
if(BUILD_TESTING)
    add_test(NAME woby_vertex_drawing_analysis COMMAND "${Python3_EXECUTABLE}"
        "${vertex_probe_tests}/analysis_tests.py")
    if(WIN32)
        add_test(NAME woby_vertex_drawing_contract COMMAND "${Python3_EXECUTABLE}"
            "${vertex_probe_tests}/contract_test.py" "$<TARGET_FILE:woby_vertex_benchmark>")
        set_tests_properties(woby_vertex_drawing_contract PROPERTIES LABELS graphics TIMEOUT 120)
    endif()
endif()
