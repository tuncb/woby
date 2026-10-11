include_guard(GLOBAL)

# The viewer and standalone detector probes must use the same arithmetic
# settings. Source properties are local to the target's CMake directory.
function(woby_add_intersection_predicates target)
    set(source "${PROJECT_SOURCE_DIR}/src/mesh_intersection_predicates.cpp")
    target_sources(${target} PRIVATE "${source}")
    target_link_libraries(${target} PRIVATE CDT::CDT)
    set_source_files_properties("${source}" TARGET_DIRECTORY ${target} PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON)
    if(MSVC)
        set_source_files_properties("${source}" TARGET_DIRECTORY ${target} PROPERTIES
            COMPILE_OPTIONS /fp:strict
            VS_SETTINGS "Optimization=MaxSpeed;InlineFunctionExpansion=AnySuitable;BasicRuntimeChecks=Default")
    else()
        set_source_files_properties("${source}" TARGET_DIRECTORY ${target} PROPERTIES
            COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off;-frounding-math")
    endif()
endfunction()
