find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(detector_profile_sources mesh_comparison mesh_topology mesh_intersections
    mesh_duplicates mesh_degenerates surface_mesh_quality difference_mesh)
set(detector_profile_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(name IN ITEMS main.cpp scope.h query_counts.h predicate_counts.h generate.py CMakeLists.txt)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${name}" "${CMAKE_CURRENT_BINARY_DIR}/harness/${name}" COPYONLY)
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/harness/compile-options-$<CONFIG>.txt"
    CONTENT "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\nGenerator: ${CMAKE_GENERATOR}\nRelease flags: ${CMAKE_CXX_FLAGS_RELEASE}\nLinker flags: ${CMAKE_EXE_LINKER_FLAGS_RELEASE}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/generate.py")
foreach(name IN LISTS detector_profile_sources)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/${name}.cpp")
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}" "${detector_profile_generated}" COMMAND_ERROR_IS_FATAL ANY)
foreach(target IN ITEMS woby_detector_probe woby_detector_profile woby_detector_queries woby_detector_tokenref woby_detector_max3)
    add_executable(${target} main.cpp "${PROJECT_SOURCE_DIR}/experiments/obj-profile/profile.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/experiments/obj-profile")
    target_link_libraries(${target} PRIVATE woby_core nlohmann_json::nlohmann_json
        manifold::manifold Boost::multiprecision psapi dbghelp)
    target_compile_options(${target} PRIVATE /Zi /Zo)
    target_link_options(${target} PRIVATE /DEBUG:FULL /INCREMENTAL:NO
        "$<$<CONFIG:Release>:/OPT:REF>" "$<$<CONFIG:Release>:/OPT:ICF>"
        "/MAP:${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    woby_enable_project_warnings(${target})
endforeach()
foreach(name IN LISTS detector_profile_sources)
    target_sources(woby_detector_profile PRIVATE "${detector_profile_generated}/${name}.cpp")
endforeach()
target_compile_definitions(woby_detector_profile PRIVATE DETECTOR_PROFILE_SCOPES=1)
target_sources(woby_detector_queries PRIVATE "${detector_profile_generated}/mesh_comparison_counted.cpp"
    "${detector_profile_generated}/mesh_intersections_counted.cpp")
target_compile_definitions(woby_detector_queries PRIVATE DETECTOR_PROFILE_SCOPES=1 DETECTOR_PROFILE_QUERY_COUNTERS=1)
target_sources(woby_detector_tokenref PRIVATE "${detector_profile_generated}/surface_mesh_quality_tokenref.cpp")
target_compile_definitions(woby_detector_tokenref PRIVATE DETECTOR_PROFILE_SCOPES=1 DETECTOR_PROFILE_TOKENREF=1)
target_sources(woby_detector_max3 PRIVATE "${detector_profile_generated}/mesh_comparison_max3.cpp")
target_compile_definitions(woby_detector_max3 PRIVATE DETECTOR_PROFILE_SCOPES=1 DETECTOR_PROFILE_MAX3=1)
if(BUILD_TESTING)
    add_test(NAME woby_detector_profile_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests.py" "$<TARGET_FILE:woby_detector_probe>"
        "$<TARGET_FILE:woby_detector_profile>" "$<TARGET_FILE:woby_detector_queries>" "$<TARGET_FILE:woby_detector_tokenref>" "$<TARGET_FILE:woby_detector_max3>")
    set_tests_properties(woby_detector_profile_contract PROPERTIES LABELS "unit;cpu" TIMEOUT 120)
endif()
