cmake_minimum_required(VERSION 3.24)

option(WOBY_BUILD_HASH_BENCHMARKS "Build opt-in hash table and scene query benchmarks" OFF)
if(WOBY_BUILD_HASH_BENCHMARKS)
    list(APPEND VCPKG_MANIFEST_FEATURES hash-benchmarks)
endif()

if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version (Metal 4)")
endif()
project(woby_obj_viewer VERSION 0.26.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(WOBY_RUNTIME_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND WOBY_RUNTIME_DIRECTORY "/$<CONFIG>")
endif()

# Runtime DLLs are staged from CMake's transitive link graph below. Avoid adding
# vcpkg's unconditional PowerShell deployment events to every executable/DLL.
if(WIN32)
    set(VCPKG_APPLOCAL_DEPS OFF)
endif()

include(cmake/WobyWarnings.cmake)
include(cmake/WobyPrecompiledHeaders.cmake)
include(cmake/WobyRuntimeFiles.cmake)

find_package(SDL3 3.4 CONFIG REQUIRED)
include(cmake/NoGraphicsAPI.cmake)
include(cmake/BxMath.cmake)
find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(CDT CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS multiprecision unordered)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)
include(cmake/WobyUpdateDependencies.cmake)
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()

add_library(woby_update_core STATIC src/update_core.cpp src/update_platform.cpp src/update.h)
target_include_directories(woby_update_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(woby_update_core PUBLIC nlohmann_json::nlohmann_json)
if(WIN32)
    target_link_libraries(woby_update_core PRIVATE bcrypt shell32 ole32)
else()
    target_link_libraries(woby_update_core PRIVATE OpenSSL::Crypto)
endif()
woby_enable_project_warnings(woby_update_core)
add_library(woby_update STATIC src/update.cpp src/update_archive.cpp src/update_ui.cpp)
target_link_libraries(woby_update PUBLIC woby_update_core PRIVATE CURL::libcurl ${woby_archive_target} Threads::Threads)
woby_enable_project_warnings(woby_update)
add_executable(woby-update-helper src/update_helper.cpp)
target_compile_definitions(woby-update-helper PRIVATE WOBY_VERSION="${PROJECT_VERSION}")
target_link_libraries(woby-update-helper PRIVATE woby_update_core)
woby_enable_project_warnings(woby-update-helper)
if(UNIX AND NOT APPLE)
    set_target_properties(woby-update-helper PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
endif()

add_library(woby_automation STATIC
    src/control_protocol.cpp
    src/control_protocol.h
    src/automation.cpp
    src/automation.h
    src/automation_registry.cpp
    src/automation_registry.h
)
target_include_directories(woby_automation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(woby_automation PUBLIC httplib::httplib nlohmann_json::nlohmann_json Threads::Threads)
if(WIN32)
    target_link_libraries(woby_automation PRIVATE bcrypt advapi32 shell32 ole32)
endif()
woby_enable_project_warnings(woby_automation)
woby_enable_precompiled_headers(woby_automation <nlohmann/json.hpp>)

if(MSVC)
    # Keep large detector runs responsive in Debug. Its optimized source cannot
    # use the Debug PCH, which is compiled with runtime checks enabled.
    set_source_files_properties(src/mesh_intersections.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON
        VS_SETTINGS "Optimization=MaxSpeed;InlineFunctionExpansion=AnySuitable;BasicRuntimeChecks=Default")
endif()

add_executable(woby
    src/console.cpp
    src/console.h
    src/comparison_report.cpp
    src/comparison_legend.cpp
    src/surface_mesh_quality.cpp
    src/mesh_intersections.cpp
    src/mesh_topology.cpp
    src/mesh_degenerates.cpp
    src/mesh_duplicates.cpp
    src/mesh_comparison.cpp
    src/uv_quality.cpp
    src/uv_overlap.cpp
    src/comparison_scene.cpp
    src/ui_views.cpp
    src/analysis_presentation.cpp
    src/comparison_runtime.cpp
    src/comparison_gpu.cpp
    src/comparison_view.cpp
    src/uv_quality_view.cpp
    src/control_importers.cpp
    src/control_importers.h
    src/analysis_results.cpp
    src/analysis_results.h
    src/control_scene.cpp
    src/control_uv.cpp
    src/control_annotation.cpp
    src/control_scene.h
    src/background_load.cpp
    src/background_load.h
    src/graphics_helpers.cpp
    src/graphics_helpers.h
    src/renderer_startup.cpp
    src/renderer_startup.h
    src/camera.cpp
    src/camera.h
    src/scene_viewport.h
    src/camera_sdl.cpp
    src/command_line.cpp
    src/command_line.h
    src/file_discovery.cpp
    src/file_discovery.h
    src/hash_utils.cpp
    src/hash_utils.h
    src/marker_pick.cpp
    src/marker_pick.h
    src/marker_pick_logic.cpp
    src/marker_pick_logic.h
    src/hover_pick.cpp
    src/hover_pick.h
    src/surface_annotation.cpp
    src/ui_annotation_operations.cpp
    src/annotation_work.cpp
    src/annotation_command.cpp
    src/annotation_preparation.cpp
    src/annotation_ui.cpp
    src/scene_pick.cpp
    src/scene_pick.h
    src/scene_dimensions.cpp
    src/scene_dimensions.h
    src/scene_scale_overlay.cpp
    src/scene_scale_overlay.h
    src/imgui_graphics.cpp
    src/imgui_graphics.h
    src/main.cpp
    src/main_menu.cpp
    src/main_menu.h
    src/settings_dialog.cpp
    src/settings_dialog.h
    src/scene_inspector_queries.cpp
    src/scene_inspector.cpp
    src/scene_inspector.h
    src/ui_icon_controls.cpp
    src/ui_icon_controls.h
    src/importer_host.cpp
    src/importer_host.h
    src/model_load.cpp
    src/model_load.h
    src/utf8_path.h
    src/native_dialogs.cpp
    src/native_dialogs.h
    src/manual_path_dialog.cpp
    src/manual_path_dialog_ui.cpp
    src/manual_path_dialog.h
    src/model_mesh.cpp
    src/model_mesh.h
    src/obj_mesh.cpp
    src/obj_freeform.cpp
    src/freeform.cpp
    src/freeform_trim.cpp
    src/obj_mesh.h
    src/performance_log.cpp
    src/performance_log.h
    src/scene_history_load.cpp
    src/scene_history_load.h
    src/scene_history.cpp
    src/scene_history.h
    src/ui_history_controls.h
    src/scene_lifecycle.cpp
    src/scene_lifecycle.h
    src/scene_file.cpp
    src/scene_file.h
    src/scene_mesh_preparation.cpp
    src/scene_mesh_preparation.h
    src/scene_draw_plan.cpp
    src/scene_draw_plan.h
    src/scene_renderer.cpp
    src/freeform_gpu.cpp
    src/scene_renderer.h
    src/scene_screenshot.cpp
    src/scene_screenshot.h
    src/scene_objects.cpp
    src/scene_objects.h
    src/ui_operations.cpp
    src/ui_view_operations.cpp
    src/ui_property_operations.cpp
    src/ui_operations.h
    src/ui_state.cpp
    src/ui_state.h
    src/ui_layout.h
)

target_compile_features(woby PRIVATE cxx_std_20)
if(WIN32)
    set_target_properties(woby PROPERTIES WIN32_EXECUTABLE TRUE)
    enable_language(RC)
    target_sources(woby PRIVATE packaging/windows/woby.rc)
    target_include_directories(woby PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons")
endif()
target_include_directories(woby PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(woby SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")

target_compile_definitions(woby PRIVATE WOBY_VERSION="${PROJECT_VERSION}")

target_link_libraries(woby PRIVATE
    CDT::CDT
    Boost::multiprecision
    Boost::unordered
    woby_update
    woby_automation
    SDL3::SDL3
    ${CMAKE_DL_LIBS}
    woby_graphics
    Threads::Threads
    imgui::imgui
    spdlog::spdlog
)

if(UNIX AND NOT APPLE)
    set_target_properties(woby PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

foreach(optional_math_target IN ITEMS woby_math)
    if(TARGET "${optional_math_target}")
        target_link_libraries(woby PRIVATE "${optional_math_target}")
    endif()
endforeach()

woby_enable_project_warnings(woby)
woby_enable_enum_switch_warnings(woby)
woby_enable_precompiled_headers(woby)

set(woby_resource_directory "${WOBY_RUNTIME_DIRECTORY}")

if(APPLE)
    include(cmake/WobyMacOSBundle.cmake)
    woby_configure_macos_bundle(woby "${WOBY_RUNTIME_DIRECTORY}" woby_resource_directory
        "$<TARGET_FILE_DIR:SDL3::SDL3>")
    target_sources(woby PRIVATE packaging/icons/woby.icns)
    set_source_files_properties(packaging/icons/woby.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(woby PROPERTIES MACOSX_BUNDLE_ICON_FILE woby.icns)
endif()
woby_compile_graphics_shaders(woby)
woby_stage_assets(woby "${woby_resource_directory}")
add_dependencies(woby woby-update-helper)
set(woby_runtime_targets woby woby-update-helper)

include(CTest)

option(WOBY_BUILD_OVERLAY_PROTOTYPE "Build the fixed-resolution mesh overlay experiment" OFF)
if(WOBY_BUILD_OVERLAY_PROTOTYPE)
    add_subdirectory(experiments/mesh-overlays)
    list(APPEND woby_runtime_targets woby_overlay_prototype woby_point_prototype)
    if(BUILD_TESTING)
        list(APPEND woby_runtime_targets woby_overlay_tests)
    endif()
endif()

option(WOBY_BUILD_NOGRAPHICSAPI_PROTOTYPE "Build the Windows NoGraphicsAPI migration experiment" OFF)
if(WOBY_BUILD_NOGRAPHICSAPI_PROTOTYPE)
    add_subdirectory(experiments/nographicsapi)
    list(APPEND woby_runtime_targets woby_nographicsapi_prototype)
endif()

option(WOBY_BUILD_BENCHMARKS "Build standalone CPU performance workloads" OFF)
if(WOBY_BUILD_HASH_BENCHMARKS)
    find_package(unordered_dense CONFIG REQUIRED)
    add_executable(woby_hash_benchmarks tests/hash_table_benchmarks.cpp)
    target_include_directories(woby_hash_benchmarks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(woby_hash_benchmarks PRIVATE Boost::unordered unordered_dense::unordered_dense)
    woby_enable_project_warnings(woby_hash_benchmarks)
endif()
if(WOBY_BUILD_BENCHMARKS)
    add_executable(woby_benchmarks
        tests/performance_benchmarks.cpp
        src/model_mesh.cpp src/obj_mesh.cpp src/obj_freeform.cpp src/freeform.cpp src/freeform_trim.cpp src/mesh_comparison.cpp
        src/uv_quality.cpp src/uv_overlap.cpp
        src/mesh_duplicates.cpp src/mesh_degenerates.cpp src/mesh_topology.cpp src/mesh_intersections.cpp src/surface_mesh_quality.cpp)
    target_include_directories(woby_benchmarks PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(woby_benchmarks SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
    target_link_libraries(woby_benchmarks PRIVATE Threads::Threads CDT::CDT Boost::multiprecision)
    woby_enable_project_warnings(woby_benchmarks)
    list(APPEND woby_runtime_targets woby_benchmarks)
    if(WOBY_BUILD_HASH_BENCHMARKS)
        # Compile the real detector algorithms against alternate index adapters.
        # Generated copies keep experimental container code out of production.
        set(hash_probe_directory "${CMAKE_CURRENT_BINARY_DIR}/hash-probe")
        file(MAKE_DIRECTORY "${hash_probe_directory}")
        set(hash_probe_sources)
        foreach(source IN ITEMS mesh_comparison mesh_duplicates mesh_topology)
            set(input "${CMAKE_CURRENT_SOURCE_DIR}/src/${source}.cpp")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
            file(READ "${input}" contents)
            string(REPLACE "\"analysis_index.h\"" "\"benchmark_analysis_index.h\"" contents "${contents}")
            file(CONFIGURE OUTPUT "${hash_probe_directory}/${source}.cpp" CONTENT "${contents}" @ONLY)
            list(APPEND hash_probe_sources "${hash_probe_directory}/${source}.cpp")
        endforeach()
        foreach(backend IN ITEMS boost ankerl)
            set(target "woby_benchmarks_${backend}")
            add_executable(${target} tests/performance_benchmarks.cpp ${hash_probe_sources}
                src/model_mesh.cpp src/obj_mesh.cpp src/obj_freeform.cpp src/freeform.cpp src/freeform_trim.cpp src/mesh_degenerates.cpp
                src/uv_quality.cpp src/uv_overlap.cpp
                src/mesh_intersections.cpp src/surface_mesh_quality.cpp)
            target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/tests")
            target_include_directories(${target} SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
            target_compile_definitions(${target} PRIVATE "WOBY_BENCHMARK_${backend}=1")
            target_link_libraries(${target} PRIVATE Threads::Threads
                CDT::CDT Boost::multiprecision Boost::unordered unordered_dense::unordered_dense)
            woby_enable_project_warnings(${target})
            list(APPEND woby_runtime_targets ${target})
        endforeach()
    endif()
endif()

if(BUILD_TESTING)
    include(cmake/WobyPython.cmake)
    if(WOBY_BUILD_BENCHMARKS)
        add_test(NAME woby_detector_benchmarks COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/performance_benchmarks_tests.py" "$<TARGET_FILE:woby_benchmarks>")
        set_tests_properties(woby_detector_benchmarks PROPERTIES TIMEOUT 150)
    endif()
    option(WOBY_TEST_HEADLESS "Run headless integration tests (requires a NoGraphicsAPI-compatible GPU)" OFF)
    if(WOBY_TEST_HEADLESS AND (WIN32 OR (UNIX AND NOT APPLE)))
        add_test(NAME woby_headless COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_headless_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_headless_camera COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_camera_smoke.py" "$<TARGET_FILE:woby>" --headless)
        add_test(NAME woby_vertex_render COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_vertex_render_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_obj_primitives_render COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_obj_primitives_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_freeform_render COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_freeform_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_uv_grid_render COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_uv_grid_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_uv_quality_render COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_uv_quality_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_loading_preparation COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_loading_preparation_smoke.py" "$<TARGET_FILE:woby>")
        add_test(NAME woby_coordinate_origin COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_coordinate_origin_smoke.py" "$<TARGET_FILE:woby>")
        set_tests_properties(woby_headless woby_headless_camera woby_vertex_render woby_obj_primitives_render woby_freeform_render woby_uv_grid_render woby_uv_quality_render woby_loading_preparation woby_coordinate_origin PROPERTIES TIMEOUT 180 LABELS graphics)
    endif()
    add_test(NAME woby_python_selection COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-system-tests"
        "-DPYTHON_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/WobyPython.cmake"
        "-DPYTHON_PIN=${CMAKE_CURRENT_SOURCE_DIR}/.python-version"
        "-DUV_EXECUTABLE=${WOBY_UV_EXECUTABLE}"
        "-DEXPECTED_PYTHON=${Python3_EXECUTABLE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/python_tests.cmake")
    add_test(NAME woby_update_packaging COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/.github/scripts/package_manifest_tests.py")
    add_test(NAME woby_marker_shaders COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/marker_shader_tests.py"
        "${WOBY_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native/woby.slang"
        "${woby_ngapi_SOURCE_DIR}")
    add_executable(woby_update_viewer_fixture tests/update_viewer_fixture.cpp)
    target_compile_definitions(woby_update_viewer_fixture PRIVATE WOBY_VERSION="${PROJECT_VERSION}")
    target_link_libraries(woby_update_viewer_fixture PRIVATE woby_update_core)
    woby_enable_project_warnings(woby_update_viewer_fixture)
    add_test(NAME woby_update_helper_smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_helper_smoke.py" "$<TARGET_FILE:woby>"
        "$<TARGET_FILE:woby-update-helper>" "${woby_resource_directory}/assets"
        "$<TARGET_FILE:woby_update_viewer_fixture>")
    set_tests_properties(woby_update_helper_smoke PROPERTIES TIMEOUT 180 LABELS slow)
    add_test(NAME woby_update_helper_fixtures COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_helper_fixture_tests.py")
    find_package(doctest CONFIG REQUIRED)
    include(doctest)
    add_subdirectory(tests/utility_fixture)
    add_executable(woby_utility_allocation_tests tests/utility_allocation_tests.cpp)
    target_link_libraries(woby_utility_allocation_tests PRIVATE doctest::doctest woby_utility_allocation_fixture)
    woby_enable_project_warnings(woby_utility_allocation_tests)
    add_test(NAME woby_utility_allocations COMMAND woby_utility_allocation_tests)

    add_executable(woby_tests
        tests/graphics_tests.cpp
        tests/graphics_diagnostics_tests.cpp
        tests/shader_types_tests.cpp
        tests/frame_pacing_tests.cpp
        tests/manual_path_dialog_tests.cpp
        src/manual_path_dialog.cpp
        src/manual_path_dialog_ui.cpp
        src/native_dialogs.cpp
        tests/app_icon_tests.cpp
        tests/update_tests.cpp
        src/comparison_report.cpp
        src/ui_views.cpp
        src/analysis_presentation.cpp
        tests/analysis_presentation_tests.cpp
        src/comparison_runtime.cpp
        src/comparison_gpu.cpp
        src/comparison_view.cpp
        src/uv_quality_view.cpp
        src/comparison_legend.cpp
        src/surface_mesh_quality.cpp
        src/mesh_intersections.cpp
        src/mesh_topology.cpp
        src/mesh_degenerates.cpp
        src/mesh_duplicates.cpp
        src/mesh_comparison.cpp
        src/uv_quality.cpp
        src/uv_overlap.cpp
        src/comparison_scene.cpp
        tests/mesh_intersections_tests.cpp
        tests/mesh_topology_tests.cpp
        tests/mesh_degenerates_tests.cpp
        tests/mesh_duplicates_tests.cpp
        tests/mesh_comparison_tests.cpp
        src/control_importers.cpp
        src/control_importers.h
        src/analysis_results.cpp
        src/analysis_results.h
        src/control_scene.cpp
        src/control_uv.cpp
        src/control_annotation.cpp
        src/control_scene.h
        src/background_load.cpp
        src/background_load.h
        src/camera_sdl.cpp
        tests/keyboard_input_tests.cpp
        src/scene_inspector_queries.cpp
        src/scene_inspector.cpp
        tests/comparison_name_tests.cpp
        tests/comparison_inspector_tests.cpp
        tests/scene_inspector_queries_tests.cpp
        src/main_menu.cpp
        src/main_menu.h
        src/settings_dialog.cpp
        src/ui_icon_controls.cpp
        src/camera.cpp
        src/command_line.cpp
        src/command_line.h
        src/file_discovery.cpp
        src/file_discovery.h
        src/hash_utils.cpp
        src/hash_utils.h
        src/marker_pick.cpp
        src/marker_pick.h
        src/marker_pick_logic.cpp
        src/marker_pick_logic.h
        src/hover_pick.cpp
        src/hover_pick.h
        src/surface_annotation.cpp
        src/ui_annotation_operations.cpp
        src/annotation_work.cpp
        src/annotation_command.cpp
        src/annotation_preparation.cpp
        src/annotation_ui.cpp
        src/scene_pick.cpp
        src/scene_pick.h
        src/scene_dimensions.cpp
        src/scene_dimensions.h
        src/scene_scale_overlay.cpp
        src/scene_scale_overlay.h
        tests/scene_dimensions_tests.cpp
        src/importer_host.cpp
        src/importer_host.h
        src/model_load.cpp
        src/model_load.h
        src/utf8_path.h
        src/model_mesh.cpp
        src/model_mesh.h
        src/obj_mesh.cpp
        src/obj_freeform.cpp
        src/freeform.cpp
        src/freeform_trim.cpp
        src/obj_mesh.h
        src/performance_log.cpp
        src/performance_log.h
        src/scene_history_load.cpp
        src/scene_history_load.h
        src/scene_history.cpp
        src/scene_history.h
        src/ui_history_controls.h
        src/scene_lifecycle.cpp
        src/scene_lifecycle.h
        src/scene_file.cpp
        src/scene_file.h
        src/scene_objects.cpp
        src/scene_objects.h
        src/graphics_helpers.cpp
        src/graphics_helpers.h
        src/scene_mesh_preparation.cpp
        src/scene_mesh_preparation.h
        src/scene_draw_plan.cpp
        src/scene_draw_plan.h
        src/scene_renderer.cpp
        src/freeform_gpu.cpp
        src/scene_renderer.h
        src/ui_operations.cpp
        src/ui_view_operations.cpp
        src/ui_property_operations.cpp
        src/ui_operations.h
        src/ui_state.cpp
        src/ui_state.h
        src/ui_layout.h
        tests/scene_history_tests.cpp
        tests/scene_lifecycle_tests.cpp
        tests/analysis_results_tests.cpp
        tests/control_tests.cpp
        tests/importer_tests.cpp
        tests/importer_lines_tests.cpp
        tests/importer_geometry_tests.cpp
        tests/importer_point_ids_tests.cpp
        tests/automation_tests.cpp
        tests/background_load_tests.cpp
        tests/loading_preparation_tests.cpp
        tests/dummy_tests.cpp
        tests/command_line_tests.cpp
        tests/file_discovery_tests.cpp
        tests/marker_pick_tests.cpp
        tests/hover_pick_tests.cpp
        tests/surface_annotation_tests.cpp
        tests/scene_pick_tests.cpp
        tests/scene_renderer_tests.cpp
        tests/scene_edge_tests.cpp
        tests/coordinate_origin_tests.cpp
        tests/model_mesh_tests.cpp
        tests/obj_mesh_tests.cpp
        tests/freeform_tests.cpp
        tests/freeform_trim_tests.cpp
        tests/scene_file_edge_tests.cpp
        tests/ui_operations_tests.cpp
        tests/ui_view_tests.cpp
        tests/uv_grid_tests.cpp
        tests/uv_quality_tests.cpp
    )

    if(WOBY_BUILD_HASH_BENCHMARKS)
        target_sources(woby_tests PRIVATE tests/scene_query_benchmarks.cpp)
    endif()
    if(WOBY_TEST_HEADLESS AND (WIN32 OR (UNIX AND NOT APPLE)))
        target_sources(woby_tests PRIVATE tests/scene_edge_gpu_tests.cpp tests/marker_pick_gpu_tests.cpp tests/graphics_gpu_tests.cpp tests/freeform_gpu_tests.cpp)
        target_compile_definitions(NoGraphicsAPI PRIVATE WOBY_GPU_ALLOCATION_TESTS)
        target_compile_definitions(woby_tests PRIVATE WOBY_GPU_ALLOCATION_TESTS)
    endif()
    target_compile_features(woby_tests PRIVATE cxx_std_20)
    target_include_directories(woby_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native")
    target_include_directories(woby_tests SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
    if(WIN32)
        # Isolate the deterministic Windows I/O shim from the application's parser.
        add_executable(woby_rapidobj_reader_tests tests/rapidobj_reader_tests.cpp)
        target_include_directories(woby_rapidobj_reader_tests SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
        target_link_libraries(woby_rapidobj_reader_tests PRIVATE doctest::doctest Threads::Threads)
        woby_enable_project_warnings(woby_rapidobj_reader_tests)
        add_test(NAME woby_rapidobj_reader COMMAND woby_rapidobj_reader_tests)
        add_dependencies(woby_tests woby_rapidobj_reader_tests)
        find_package(Vulkan REQUIRED)
        add_executable(woby_native_allocation_tests tests/native_allocation_tests.cpp)
        target_include_directories(woby_native_allocation_tests SYSTEM PRIVATE
            "${woby_ngapi_SOURCE_DIR}/include" "${woby_ngapi_SOURCE_DIR}/src")
        target_link_libraries(woby_native_allocation_tests PRIVATE doctest::doctest Vulkan::Vulkan)
        woby_enable_project_warnings(woby_native_allocation_tests)
        add_test(NAME woby_native_allocations COMMAND woby_native_allocation_tests)
        add_dependencies(woby_tests woby_native_allocation_tests)
    endif()
    target_link_libraries(woby_tests PRIVATE
        CDT::CDT
        Boost::multiprecision
        Boost::unordered
        woby_update
        ${woby_archive_target}
        woby_automation
        doctest::doctest
        imgui::imgui
        ${CMAKE_DL_LIBS}
        woby_graphics
        NoGraphicsAPIUtility::types
        Threads::Threads
        spdlog::spdlog
    )

    foreach(optional_math_target IN ITEMS woby_math)
        if(TARGET "${optional_math_target}")
            target_link_libraries(woby_tests PRIVATE "${optional_math_target}")
        endif()
    endforeach()

    woby_enable_project_warnings(woby_tests)
    woby_enable_precompiled_headers(woby_tests <doctest/doctest.h>)
    # doctest's implementation macro must take effect before its first include.
    set_source_files_properties(tests/dummy_tests.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    add_library(woby_importer_fixture SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_hierarchy_fixture SHARED tests/importer_hierarchy_fixture.cpp)
    add_library(woby_importer_lines_fixture SHARED tests/importer_lines_fixture.cpp)
    add_library(woby_importer_geometry_fixture SHARED tests/importer_geometry_fixture.cpp)
    add_library(woby_importer_point_ids_fixture SHARED tests/importer_point_ids_fixture.cpp)
    add_library(woby_importer_bad_abi SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_conflict SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_reserved SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_missing_export SHARED tests/importer_missing_export.cpp)
    target_compile_definitions(woby_importer_bad_abi PRIVATE WOBY_TEST_BAD_ABI)
    target_compile_definitions(woby_importer_conflict PRIVATE WOBY_TEST_CONFLICT)
    target_compile_definitions(woby_importer_reserved PRIVATE WOBY_TEST_RESERVED)
    foreach(fixture IN ITEMS woby_importer_fixture woby_importer_hierarchy_fixture woby_importer_lines_fixture woby_importer_geometry_fixture woby_importer_point_ids_fixture woby_importer_bad_abi woby_importer_missing_export woby_importer_conflict woby_importer_reserved)
        target_include_directories(${fixture} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
        target_compile_definitions(${fixture} PRIVATE WOBY_IMPORTER_BUILD)
        target_compile_features(${fixture} PRIVATE cxx_std_20)
        set_target_properties(${fixture} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-plugins"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
        woby_enable_project_warnings(${fixture})
    endforeach()
    add_subdirectory(examples/off_importer)
    set_target_properties(woby_off_importer PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/example-plugins"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/example-plugins")
    woby_enable_project_warnings(woby_off_importer)
    if(WOBY_TEST_HEADLESS AND (WIN32 OR (UNIX AND NOT APPLE)))
        add_test(NAME woby_portable_importers COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/portable_importers_smoke.py" "$<TARGET_FILE:woby>"
            "$<TARGET_FILE:woby-update-helper>" "${woby_resource_directory}/assets" "$<TARGET_FILE:woby_off_importer>")
        set_tests_properties(woby_portable_importers PROPERTIES TIMEOUT 180 LABELS graphics)
        add_test(NAME woby_ctl_lines COMMAND "${WOBY_UV_EXECUTABLE}" run --python "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/ctl_lines_smoke.py" "$<TARGET_FILE:woby>"
            "$<TARGET_FILE:woby_importer_lines_fixture>")
        set_tests_properties(woby_ctl_lines PROPERTIES TIMEOUT 180 LABELS graphics)
    endif()
    add_dependencies(woby_tests woby woby_importer_fixture woby_importer_hierarchy_fixture woby_importer_lines_fixture woby_importer_geometry_fixture woby_importer_point_ids_fixture woby_importer_bad_abi woby_importer_missing_export woby_off_importer woby_importer_conflict woby_importer_reserved)
    target_compile_definitions(woby_tests PRIVATE
        WOBY_TEST_APP="$<TARGET_FILE:woby>"
        WOBY_TEST_ICON_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons"
        WOBY_TEST_ASSET_DIRECTORY="${woby_resource_directory}/assets"
        WOBY_TEST_IMPORTER="$<TARGET_FILE:woby_importer_fixture>"
        WOBY_TEST_HIERARCHY_IMPORTER="$<TARGET_FILE:woby_importer_hierarchy_fixture>"
        WOBY_TEST_LINES_IMPORTER="$<TARGET_FILE:woby_importer_lines_fixture>"
        WOBY_TEST_GEOMETRY_IMPORTER="$<TARGET_FILE:woby_importer_geometry_fixture>"
        WOBY_TEST_POINT_IDS_IMPORTER="$<TARGET_FILE:woby_importer_point_ids_fixture>"
        WOBY_TEST_CONFLICT_IMPORTER="$<TARGET_FILE:woby_importer_conflict>"
        WOBY_TEST_RESERVED_IMPORTER="$<TARGET_FILE:woby_importer_reserved>"
        WOBY_TEST_BAD_IMPORTER="$<TARGET_FILE:woby_importer_bad_abi>"
        WOBY_TEST_MISSING_EXPORT="$<TARGET_FILE:woby_importer_missing_export>"
        WOBY_TEST_OFF_IMPORTER="$<TARGET_FILE:woby_off_importer>"
    )

    include(cmake/WobyTestDiscovery.cmake)
    woby_discover_tests(woby_tests SLOW_TESTS
        "analysis measures both surfaces above the former triangle cap"
        "command history evicts results without ever reusing admitted retry keys")
    list(APPEND woby_runtime_targets woby_tests)

    add_test(NAME woby_runtime_staging COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-system-tests"
        "-DSTAGING_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeFiles.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/runtime_files_tests.cmake")
    add_test(NAME woby_test_discovery COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-system-tests"
        "-DDISCOVERY_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/DiscoverTestsIfNeeded.cmake"
        "-DDOCTEST_SCRIPT=${_DOCTEST_DISCOVER_TESTS_SCRIPT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_discovery_tests.cmake")
    add_test(NAME woby_runtime_build_graph COMMAND "${CMAKE_COMMAND}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-system-tests"
        "-DRUNTIME_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/WobyRuntimeFiles.cmake"
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        "-DTEST_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DTEST_CONFIG=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/runtime_build_tests.cmake")
    set_tests_properties(woby_runtime_build_graph PROPERTIES LABELS slow)
    add_test(NAME woby_test_suite_selection COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_suite_selection_tests.py"
        "${CMAKE_COMMAND}" "${CMAKE_CTEST_COMMAND}" "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_DOCTEST_DISCOVER_TESTS_SCRIPT}" "${CMAKE_GENERATOR}" "${CMAKE_GENERATOR_PLATFORM}")

    add_test(NAME woby_version COMMAND woby --version)
    set_tests_properties(woby_version PROPERTIES
        PASS_REGULAR_EXPRESSION "^${PROJECT_VERSION}\r?\n?$"
    )
    if(APPLE)
        add_test(NAME woby_macos_bundle COMMAND "${CMAKE_COMMAND}"
            "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/build-system-tests"
            "-DBUNDLE_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/WobyMacOSBundle.cmake"
            "-DRUNTIME_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/WobyRuntimeFiles.cmake"
            "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
            "-DTEST_CONFIG=$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/macos_bundle_tests.cmake")
    endif()
    if(WIN32)
        add_executable(woby_console_probe WIN32 tests/windows_console_probe.cpp src/console.cpp)
        add_executable(woby_console_parent tests/windows_console_probe.cpp)
        target_compile_definitions(woby_console_parent PRIVATE WOBY_CONSOLE_PARENT)
        foreach(target IN ITEMS woby_console_probe woby_console_parent)
            target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
            woby_enable_project_warnings(${target})
        endforeach()
        add_test(NAME woby_windows_launch COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/windows_launch_tests.py"
            "$<TARGET_FILE:woby>" "$<TARGET_FILE:woby_console_probe>"
            "$<TARGET_FILE:woby_console_parent>" "${PROJECT_VERSION}")
    endif()
endif()

option(WOBY_BUILD_OBJ_BENCHMARKS "Build pinned OBJ loader comparisons" OFF)
if(WOBY_BUILD_OBJ_BENCHMARKS)
    include(cmake/WobyObjBenchmarks.cmake)
    include(cmake/WobyMappingBenchmarks.cmake)
endif()

option(WOBY_BUILD_MESH_MEMORY_LAB "Build the native mesh memory visualization prototype" OFF)
if(WOBY_BUILD_MESH_MEMORY_LAB)
    add_subdirectory(experiments/mesh-memory-lab)
    list(APPEND woby_runtime_targets mesh_memory_lab)
    if(BUILD_TESTING)
        list(APPEND woby_runtime_targets mesh_memory_lab_tests)
    endif()
endif()

woby_stage_runtime_dlls("${WOBY_RUNTIME_DIRECTORY}" ${woby_runtime_targets})
