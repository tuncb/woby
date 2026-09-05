cmake_minimum_required(VERSION 3.24)

project(woby_obj_viewer VERSION 0.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/WobyWarnings.cmake)

find_package(SDL3 CONFIG REQUIRED)
find_package(bgfx CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

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

add_executable(woby
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
    src/ui_operations.h
    src/ui_state.cpp
    src/ui_state.h
)

target_compile_features(woby PRIVATE cxx_std_20)
target_include_directories(woby PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_compile_definitions(woby PRIVATE WOBY_VERSION="${PROJECT_VERSION}")

target_link_libraries(woby PRIVATE
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

add_custom_target(woby_assets
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMENT "Copying runtime assets"
    VERBATIM
)

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

add_dependencies(woby woby_assets)

add_custom_command(TARGET woby POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_BINARY_DIR}/assets"
            "$<TARGET_FILE_DIR:woby>/assets"
    VERBATIM
)

if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET woby POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>"
                "$<TARGET_FILE_DIR:woby>"
        VERBATIM
    )
endif()

include(CTest)

if(BUILD_TESTING)
    find_package(doctest CONFIG REQUIRED)
    include(doctest)

    add_executable(woby_tests
        src/control_importers.cpp
        src/control_importers.h
        src/control_scene.cpp
        src/control_scene.h
        src/background_load.cpp
        src/background_load.h
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
        src/ui_operations.h
        src/ui_state.cpp
        src/ui_state.h
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
        woby_automation
        doctest::doctest
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

    doctest_discover_tests(woby_tests)

    add_test(NAME woby_version COMMAND woby --version)
    set_tests_properties(woby_version PROPERTIES
        PASS_REGULAR_EXPRESSION "^${PROJECT_VERSION}\r?\n?$"
    )
endif()
