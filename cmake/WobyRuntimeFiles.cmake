include_guard(GLOBAL)

function(woby_stage_assets target runtime_directory)
    file(GLOB_RECURSE asset_names CONFIGURE_DEPENDS LIST_DIRECTORIES false
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/*")
    set(asset_sources)
    foreach(name IN LISTS asset_names)
        list(APPEND asset_sources "${CMAKE_CURRENT_SOURCE_DIR}/assets/${name}")
    endforeach()
    get_target_property(shader_sources ${target} WOBY_SHADER_OUTPUTS)
    if(shader_sources)
        foreach(source IN LISTS shader_sources)
            file(RELATIVE_PATH name "${CMAKE_CURRENT_BINARY_DIR}/assets" "${source}")
            list(APPEND asset_sources "${source}")
            list(APPEND asset_names "${name}")
        endforeach()
    endif()

    set(staging_directory "${CMAKE_CURRENT_BINARY_DIR}/runtime-staging")
    file(MAKE_DIRECTORY "${staging_directory}")
    set(manifest "${staging_directory}/${target}-assets-$<CONFIG>.cmake")
    set(stamp "${staging_directory}/${target}-assets-$<CONFIG>.stamp")
    set(outputs)
    foreach(name IN LISTS asset_names)
        list(APPEND outputs "${runtime_directory}/assets/${name}")
    endforeach()
    set(refresh_timestamps OFF)
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        set(refresh_timestamps ON)
    endif()
    file(GENERATE OUTPUT "${manifest}" CONTENT
"set(stage_root [==[${runtime_directory}/assets]==])
set(stage_sources [==[${asset_sources}]==])
set(stage_names [==[${asset_names}]==])
set(stage_state [==[${staging_directory}/${target}-assets-$<CONFIG>.state]==])
set(stage_stamp [==[${stamp}]==])
set(stage_refresh_timestamps ${refresh_timestamps})
")
    set(primary_outputs "${stamp}")
    set(byproducts BYPRODUCTS ${outputs})
    if(CMAKE_GENERATOR MATCHES "^Visual Studio")
        # MSBuild does not reliably rebuild for a deleted BYPRODUCT. Register
        # the deployed files as primary outputs so deletion triggers repair.
        list(APPEND primary_outputs ${outputs})
        set(byproducts)
    endif()
    add_custom_command(OUTPUT ${primary_outputs}
        ${byproducts}
        COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${manifest}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageRuntimeFiles.cmake"
        DEPENDS ${asset_sources} "${manifest}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageRuntimeFiles.cmake"
        COMMENT "Staging runtime assets"
        VERBATIM)
    add_custom_target(${target}_assets DEPENDS "${stamp}")
    add_dependencies(${target} ${target}_assets)
endfunction()

function(woby_stage_runtime_dlls runtime_directory)
    if(NOT WIN32)
        return()
    endif()
    set(staging_directory "${CMAKE_CURRENT_BINARY_DIR}/runtime-staging")
    file(MAKE_DIRECTORY "${staging_directory}")
    set(dll_lists)
    foreach(target IN LISTS ARGN)
        list(APPEND dll_lists "$<TARGET_RUNTIME_DLLS:${target}>")
    endforeach()
    set(manifest "${staging_directory}/dlls-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${manifest}" CONTENT
"set(stage_root [==[${runtime_directory}]==])
set(stage_sources [==[${dll_lists}]==])
list(REMOVE_DUPLICATES stage_sources)
list(FILTER stage_sources EXCLUDE REGEX \"^$\")
set(stage_names)
foreach(source IN LISTS stage_sources)
    cmake_path(GET source FILENAME name)
    list(APPEND stage_names \"\${name}\")
endforeach()
set(stage_state [==[${staging_directory}/dlls-$<CONFIG>.state]==])
")
    # TARGET_RUNTIME_DLLS is evaluated at generation time and cannot be used in
    # OUTPUT. A lightweight native CMake check repairs missing/changed DLLs on
    # every build, without launching PowerShell or scanning executable imports.
    # One shared target prevents app/test builds racing to copy the same DLL.
    add_custom_target(woby_runtime_dlls
        COMMAND "${CMAKE_COMMAND}" "-DMANIFEST=${manifest}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/StageRuntimeFiles.cmake"
        VERBATIM)
    foreach(target IN LISTS ARGN)
        add_dependencies(${target} woby_runtime_dlls)
    endforeach()
endfunction()
