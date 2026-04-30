cmake_minimum_required(VERSION 3.24)

project(woby_obj_viewer VERSION 0.3.0 LANGUAGES CXX)

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

add_executable(woby
    src/bgfx_helpers.cpp
    src/bgfx_helpers.h
    src/camera.cpp
    src/camera.h
    src/camera_sdl.cpp
    src/command_line.cpp
    src/command_line.h
    src/file_discovery.cpp
    src/file_discovery.h
    src/imgui_bgfx.cpp
    src/imgui_bgfx.h
    src/main.cpp
    src/obj_mesh.cpp
    src/obj_mesh.h
    src/scene_file.cpp
    src/scene_file.h
    src/ui_operations.cpp
    src/ui_operations.h
    src/ui_state.cpp
    src/ui_state.h
)

target_compile_features(woby PRIVATE cxx_std_20)

target_compile_definitions(woby PRIVATE WOBY_VERSION="${PROJECT_VERSION}")

target_link_libraries(woby PRIVATE
    SDL3::SDL3
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
        src/camera.cpp
        src/command_line.cpp
        src/command_line.h
        src/file_discovery.cpp
        src/file_discovery.h
        src/scene_file.cpp
        src/scene_file.h
        src/ui_operations.cpp
        src/ui_operations.h
        src/ui_state.cpp
        src/ui_state.h
        tests/dummy_tests.cpp
        tests/command_line_tests.cpp
        tests/file_discovery_tests.cpp
        tests/ui_operations_tests.cpp
    )

    target_compile_features(woby_tests PRIVATE cxx_std_20)
    target_include_directories(woby_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(woby_tests PRIVATE doctest::doctest bgfx::bgfx)

    foreach(optional_bgfx_target IN ITEMS bgfx::bx bgfx::bimg)
        if(TARGET "${optional_bgfx_target}")
            target_link_libraries(woby_tests PRIVATE "${optional_bgfx_target}")
        endif()
    endforeach()

    woby_enable_project_warnings(woby_tests)

    doctest_discover_tests(woby_tests)
endif()
