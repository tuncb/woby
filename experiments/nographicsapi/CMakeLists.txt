if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The NoGraphicsAPI prototype currently tests Windows x64 Vulkan only")
endif()
find_package(SDL3 3.4 CONFIG REQUIRED)
find_program(WOBY_SLANGC slangc HINTS "$ENV{SLANG_ROOT}/bin" REQUIRED)
find_program(WOBY_SPIRV_VAL spirv-val HINTS "$ENV{VULKAN_SDK}/Bin" REQUIRED)
execute_process(COMMAND "${WOBY_SLANGC}" -version OUTPUT_VARIABLE slang_version ERROR_VARIABLE slang_error)
message(STATUS "Prototype Slang: ${WOBY_SLANGC} ${slang_version}${slang_error}")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" slang_version_number "${slang_version}${slang_error}")
if(NOT slang_version_number OR slang_version_number VERSION_LESS 2026.18.3)
    message(FATAL_ERROR "Use standalone Slang >= 2026.18.3; set WOBY_SLANGC to slangc.exe (the SDK bundled compiler may be older)")
endif()

include("${PROJECT_SOURCE_DIR}/cmake/NoGraphicsAPI.cmake")

set(shader_outputs)
foreach(entry_stage IN ITEMS "sceneVertex,vertex" "markerVertex,vertex" "sceneFragment,fragment"
        "pickSingle,compute" "pickMsaa,compute" "compositeVertex,vertex" "compositeFragment,fragment"
        "uiVertex,vertex" "uiFragment,fragment")
    string(REPLACE "," ";" pair "${entry_stage}")
    list(GET pair 0 entry)
    list(GET pair 1 stage)
    if(entry MATCHES "^(scene|marker)")
        set(shader_group NG_SCENE)
    elseif(entry MATCHES "^pick")
        set(shader_group NG_PICK)
    elseif(entry MATCHES "^ui")
        set(shader_group NG_UI)
    else()
        set(shader_group NG_COMPOSITE)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${entry}.spv")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND "${WOBY_SLANGC}" "${CMAKE_CURRENT_SOURCE_DIR}/prototype.slang"
            -target spirv -profile spirv_1_5 -emit-spirv-directly -fvk-use-entrypoint-name
            -entry ${entry} -stage ${stage} -fvk-use-c-layout -matrix-layout-row-major
            -D${shader_group}=1
            -I "${woby_ngapi_SOURCE_DIR}/include" -I "${woby_ngapi_SOURCE_DIR}/utility/include"
            -capability spvDescriptorHeapEXT -o "${output}"
        COMMAND "${WOBY_SPIRV_VAL}" --target-env vulkan1.4 --scalar-block-layout "${output}"
        DEPENDS prototype.slang shared.h VERBATIM)
    list(APPEND shader_outputs "${output}")
endforeach()
add_custom_target(woby_ngapi_shaders DEPENDS ${shader_outputs})
add_library(woby_ngapi_logic STATIC logic.cpp ../../src/marker_pick_logic.cpp)
target_include_directories(woby_ngapi_logic PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(woby_ngapi_logic PUBLIC woby_math)
woby_enable_project_warnings(woby_ngapi_logic)

add_executable(woby_nographicsapi_prototype main.cpp renderer.cpp imgui_renderer.cpp)
target_link_libraries(woby_nographicsapi_prototype PRIVATE woby_ngapi_logic
    NoGraphicsAPI::NoGraphicsAPI NoGraphicsAPIUtility::allocators
    NoGraphicsAPIUtility::textures NoGraphicsAPIUtility::uploads NoGraphicsAPIUtility::types
    SDL3::SDL3 imgui::imgui)
target_compile_definitions(woby_nographicsapi_prototype PRIVATE NOMINMAX
    WOBY_NGAPI_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}/shaders")
add_dependencies(woby_nographicsapi_prototype woby_ngapi_shaders)
woby_enable_project_warnings(woby_nographicsapi_prototype)
if(BUILD_TESTING)
    find_package(doctest CONFIG REQUIRED)
    add_executable(woby_ngapi_logic_tests logic_tests.cpp)
    target_link_libraries(woby_ngapi_logic_tests PRIVATE woby_ngapi_logic doctest::doctest)
    woby_enable_project_warnings(woby_ngapi_logic_tests)
    add_test(NAME woby_ngapi_logic COMMAND woby_ngapi_logic_tests)
    # Explicit opt-in: unsupported CI GPUs should still run the CPU tests.
    option(WOBY_TEST_NOGRAPHICSAPI_GPU "Run the NoGraphicsAPI hardware acceptance tests" OFF)
    if(WOBY_TEST_NOGRAPHICSAPI_GPU)
        add_test(NAME woby_ngapi_gpu COMMAND woby_nographicsapi_prototype --self-test)
        add_test(NAME woby_ngapi_window COMMAND woby_nographicsapi_prototype --window-test)
        set_tests_properties(woby_ngapi_gpu woby_ngapi_window PROPERTIES TIMEOUT 120 LABELS graphics
            ENVIRONMENT "VK_LOADER_LAYERS_DISABLE=~implicit~")
    endif()
endif()
