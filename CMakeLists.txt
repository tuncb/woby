cmake_minimum_required(VERSION 3.24)

project(woby_obj_viewer VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

find_package(SDL3 CONFIG REQUIRED)
find_package(bgfx CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_executable(woby_viewer
    src/bgfx_helpers.cpp
    src/bgfx_helpers.h
    src/imgui_bgfx.cpp
    src/imgui_bgfx.h
    src/main.cpp
    src/obj_mesh.cpp
    src/obj_mesh.h
)

target_compile_features(woby_viewer PRIVATE cxx_std_20)

target_compile_definitions(woby_viewer PRIVATE WOBY_VERSION="${PROJECT_VERSION}")

target_link_libraries(woby_viewer PRIVATE
    SDL3::SDL3
    bgfx::bgfx
    tinyobjloader::tinyobjloader
    meshoptimizer::meshoptimizer
    imgui::imgui
)

if(UNIX AND NOT APPLE)
    set_target_properties(woby_viewer PROPERTIES
        BUILD_RPATH "$ORIGIN"
        INSTALL_RPATH "$ORIGIN"
    )
endif()

foreach(optional_bgfx_target IN ITEMS bgfx::bx bgfx::bimg)
    if(TARGET "${optional_bgfx_target}")
        target_link_libraries(woby_viewer PRIVATE "${optional_bgfx_target}")
    endif()
endforeach()

if(MSVC)
    target_compile_options(woby_viewer PRIVATE /W4 /permissive-)
else()
    target_compile_options(woby_viewer PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_target(woby_assets
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${CMAKE_CURRENT_BINARY_DIR}/assets"
    COMMENT "Copying runtime assets"
    VERBATIM
)

include(cmake/BgfxShaders.cmake)

woby_compile_bgfx_shader(woby_viewer
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_mesh
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby_viewer
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mesh.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_mesh
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby_viewer
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui.vert.sc"
    TYPE vertex
    OUTPUT_NAME vs_imgui
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

woby_compile_bgfx_shader(woby_viewer
    SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/imgui.frag.sc"
    TYPE fragment
    OUTPUT_NAME fs_imgui
    VARYING "${CMAKE_CURRENT_SOURCE_DIR}/shaders/varying.def.sc"
)

add_dependencies(woby_viewer woby_assets)

add_custom_command(TARGET woby_viewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_BINARY_DIR}/assets"
            "$<TARGET_FILE_DIR:woby_viewer>/assets"
    VERBATIM
)

if(WIN32 AND TARGET SDL3::SDL3-shared)
    add_custom_command(TARGET woby_viewer POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:SDL3::SDL3-shared>"
                "$<TARGET_FILE_DIR:woby_viewer>"
        VERBATIM
    )
endif()
