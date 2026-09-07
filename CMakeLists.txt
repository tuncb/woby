cmake_minimum_required(VERSION 3.24)

project(woby_obj_viewer VERSION 0.10.0 LANGUAGES CXX)

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

find_package(SDL3 CONFIG REQUIRED)
find_package(bgfx CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
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
add_library(woby_update STATIC src/update.cpp src/update_archive.cpp)
target_link_libraries(woby_update PUBLIC woby_update_core PRIVATE CURL::libcurl ${woby_archive_target})
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

add_executable(woby
    src/console.cpp
    src/console.h
    src/comparison_report.cpp
    src/comparison_legend.cpp
    src/mesh_comparison.cpp
    src/comparison_scene.cpp
    src/comparison_view.cpp
    src/control_importers.cpp
    src/control_importers.h
    src/control_scene.cpp
    src/control_scene.h
    src/background_load.cpp
    src/background_load.h
    src/bgfx_helpers.cpp
    src/bgfx_helpers.h
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
    src/hover_pick.cpp
    src/hover_pick.h
    src/imgui_bgfx.cpp
    src/imgui_bgfx.h
    src/main.cpp
    src/settings_dialog.cpp
    src/settings_dialog.h
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
    src/model_mesh.cpp
    src/model_mesh.h
    src/obj_mesh.cpp
    src/obj_mesh.h
    src/performance_log.cpp
    src/performance_log.h
    src/scene_lifecycle.cpp
    src/scene_lifecycle.h
    src/scene_file.cpp
    src/scene_file.h
    src/scene_renderer.cpp
    src/scene_renderer.h
    src/scene_screenshot.cpp
    src/scene_screenshot.h
    src/scene_objects.cpp
    src/scene_objects.h
    src/stl_mesh.cpp
    src/stl_mesh.h
    src/ui_operations.cpp
    src/ui_property_operations.cpp
    src/ui_operations.h
    src/ui_state.cpp
    src/ui_state.h
    src/ui_layout.h
)

target_compile_features(woby PRIVATE cxx_std_20)
if(WIN32)
    set_target_properties(woby PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_include_directories(woby PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(woby PRIVATE WOBY_VERSION="${PROJECT_VERSION}")

target_link_libraries(woby PRIVATE
    woby_update
    woby_automation
    SDL3::SDL3
    ${CMAKE_DL_LIBS}
    bgfx::bgfx
    tinyobjloader::tinyobjloader
    meshoptimizer::meshoptimizer
    imgui::imgui
    spdlog::spdlog
)

if(UNIX AND NOT APPLE)
    set_target_properties(woby PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

foreach(optional_bgfx_target IN ITEMS bgfx::bx bgfx::bimg)
    if(TARGET "${optional_bgfx_target}")
        target_link_libraries(woby PRIVATE "${optional_bgfx_target}")
    endif()
endforeach()

woby_enable_project_warnings(woby)
woby_enable_enum_switch_warnings(woby)
woby_enable_precompiled_headers(woby)

include(cmake/BgfxShaders.cmake)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_mesh
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_mesh
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/comparison.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_comparison
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)
woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/comparison.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_comparison
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/color.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_color
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/color.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_color
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/point_sprite.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_point_sprite
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/point_sprite.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_point_sprite
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_imgui
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_imgui
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

set(woby_resource_directory "${WOBY_RUNTIME_DIRECTORY}")
if(APPLE)
    include(cmake/WobyMacOSBundle.cmake)
    woby_configure_macos_bundle(woby "${WOBY_RUNTIME_DIRECTORY}" woby_resource_directory
        "$<TARGET_FILE_DIR:SDL3::SDL3>")
endif()
woby_stage_assets(woby "${woby_resource_directory}")
add_dependencies(woby woby-update-helper)
set(woby_runtime_targets woby woby-update-helper)

include(CTest)

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME woby_update_packaging COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/.github/scripts/package_manifest_tests.py")
    add_test(NAME woby_update_helper_smoke COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_helper_smoke.py" "$<TARGET_FILE:woby>")
    set_tests_properties(woby_update_helper_smoke PROPERTIES TIMEOUT 180)
    find_package(doctest CONFIG REQUIRED)
    include(doctest)

    add_executable(woby_tests
        tests/update_tests.cpp
        src/comparison_report.cpp
        src/mesh_comparison.cpp
        src/comparison_scene.cpp
        tests/mesh_comparison_tests.cpp
        src/control_importers.cpp
        src/control_importers.h
        src/control_scene.cpp
        src/control_scene.h
        src/background_load.cpp
        src/background_load.h
        src/camera_sdl.cpp
        tests/keyboard_input_tests.cpp
        src/settings_dialog.cpp
        src/ui_icon_controls.cpp
        src/camera.cpp
        src/command_line.cpp
        src/command_line.h
        src/file_discovery.cpp
        src/file_discovery.h
        src/hash_utils.cpp
        src/hash_utils.h
        src/hover_pick.cpp
        src/hover_pick.h
        src/importer_host.cpp
        src/importer_host.h
        src/model_load.cpp
        src/model_load.h
        src/utf8_path.h
        src/model_mesh.cpp
        src/model_mesh.h
        src/obj_mesh.cpp
        src/obj_mesh.h
        src/performance_log.cpp
        src/performance_log.h
        src/scene_lifecycle.cpp
        src/scene_lifecycle.h
        src/scene_file.cpp
        src/scene_file.h
        src/scene_objects.cpp
        src/scene_objects.h
        src/scene_renderer.cpp
        src/scene_renderer.h
        src/stl_mesh.cpp
        src/stl_mesh.h
        src/ui_operations.cpp
        src/ui_property_operations.cpp
        src/ui_operations.h
        src/ui_state.cpp
        src/ui_state.h
        src/ui_layout.h
        tests/scene_lifecycle_tests.cpp
        tests/control_tests.cpp
        tests/importer_tests.cpp
        tests/automation_tests.cpp
        tests/background_load_tests.cpp
        tests/dummy_tests.cpp
        tests/command_line_tests.cpp
        tests/file_discovery_tests.cpp
        tests/hover_pick_tests.cpp
        tests/model_mesh_tests.cpp
        tests/obj_mesh_tests.cpp
        tests/scene_file_edge_tests.cpp
        tests/stl_mesh_tests.cpp
        tests/ui_operations_tests.cpp
    )

    target_compile_features(woby_tests PRIVATE cxx_std_20)
    target_include_directories(woby_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(woby_tests PRIVATE
        woby_update
        ${woby_archive_target}
        woby_automation
        doctest::doctest
        imgui::imgui
        ${CMAKE_DL_LIBS}
        bgfx::bgfx
        tinyobjloader::tinyobjloader
        meshoptimizer::meshoptimizer
        spdlog::spdlog
    )

    foreach(optional_bgfx_target IN ITEMS bgfx::bx bgfx::bimg)
        if(TARGET "${optional_bgfx_target}")
            target_link_libraries(woby_tests PRIVATE "${optional_bgfx_target}")
        endif()
    endforeach()

    woby_enable_project_warnings(woby_tests)
    woby_enable_precompiled_headers(woby_tests <doctest/doctest.h>)
    # doctest's implementation macro must take effect before its first include.
    set_source_files_properties(tests/dummy_tests.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

    add_library(woby_importer_fixture SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_bad_abi SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_conflict SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_reserved SHARED tests/importer_fixture.cpp)
    add_library(woby_importer_missing_export SHARED tests/importer_missing_export.cpp)
    target_compile_definitions(woby_importer_bad_abi PRIVATE WOBY_TEST_BAD_ABI)
    target_compile_definitions(woby_importer_conflict PRIVATE WOBY_TEST_CONFLICT)
    target_compile_definitions(woby_importer_reserved PRIVATE WOBY_TEST_RESERVED)
    foreach(fixture IN ITEMS woby_importer_fixture woby_importer_bad_abi woby_importer_missing_export woby_importer_conflict woby_importer_reserved)
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
    add_dependencies(woby_tests woby_importer_fixture woby_importer_bad_abi woby_importer_missing_export woby_off_importer woby_importer_conflict woby_importer_reserved)
    target_compile_definitions(woby_tests PRIVATE
        WOBY_TEST_IMPORTER="$<TARGET_FILE:woby_importer_fixture>"
        WOBY_TEST_CONFLICT_IMPORTER="$<TARGET_FILE:woby_importer_conflict>"
        WOBY_TEST_RESERVED_IMPORTER="$<TARGET_FILE:woby_importer_reserved>"
        WOBY_TEST_BAD_IMPORTER="$<TARGET_FILE:woby_importer_bad_abi>"
        WOBY_TEST_MISSING_EXPORT="$<TARGET_FILE:woby_importer_missing_export>"
        WOBY_TEST_OFF_IMPORTER="$<TARGET_FILE:woby_off_importer>"
    )

    include(cmake/WobyTestDiscovery.cmake)
    woby_discover_tests(woby_tests)
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
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
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

woby_stage_runtime_dlls("${WOBY_RUNTIME_DIRECTORY}" ${woby_runtime_targets})
