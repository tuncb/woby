# Research-only dependencies: never linked into the viewer.
include(FetchContent)
FetchContent_Declare(obj_bench_fast
    GIT_REPOSITORY https://github.com/thisistherk/fast_obj.git
    GIT_TAG d620667f10a548dee94dbc8c144bb22f79162176
    SOURCE_SUBDIR unused)
FetchContent_Declare(obj_bench_tiny
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG 45636bdcef1a4fec140346b90c0b50bf0bc3e23b
    SOURCE_SUBDIR unused)
FetchContent_MakeAvailable(obj_bench_fast obj_bench_tiny)

set(probe_dir "${CMAKE_CURRENT_BINARY_DIR}/obj-probe")
set(probe_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/obj_loading")
file(MAKE_DIRECTORY "${probe_dir}")

# Fail when production code changes enough to invalidate an instrumentation point.
function(obj_probe_replace variable before after)
    string(FIND "${${variable}}" "${before}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "OBJ probe instrumentation point missing: ${before}")
    endif()
    string(REPLACE "${before}" "${after}" contents "${${variable}}")
    set(${variable} "${contents}" PARENT_SCOPE)
endfunction()
foreach(source IN ITEMS obj_mesh model_mesh)
    set(input "${CMAKE_CURRENT_SOURCE_DIR}/src/${source}.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
    file(READ "${input}" contents)
    string(PREPEND contents "#include \"probe.h\"\n")
    if(source STREQUAL obj_mesh)
        obj_probe_replace(contents "auto result = parseObj(path);" "obj_probe::start();\n    auto result = parseObj(path);\n    obj_probe::mark(\"parse_ms\");")
        obj_probe_replace(contents "const auto& attrib = result.attributes;" "obj_probe::mark(\"triangulate_ms\");\n    const auto& attrib = result.attributes;")
        obj_probe_replace(contents "VertexIndexTable vertexMap;" "obj_probe::mark(\"source_copy_ms\");\n    VertexIndexTable vertexMap;")
        obj_probe_replace(contents "mesh.sourceData = std::move(source);" "obj_probe::mark(\"vertex_map_ms\");\n    mesh.sourceData = std::move(source);")
    else()
        obj_probe_replace(contents "mesh.bounds = calculateBounds(mesh.vertices);" "obj_probe::mark(\"normals_ms\");\n    mesh.bounds = calculateBounds(mesh.vertices);\n    obj_probe::mark(\"bounds_ms\");")
        obj_probe_replace(contents "compactMesh(mesh.vertices, mesh.indices);" "compactMesh(mesh.vertices, mesh.indices);\n    obj_probe::mark(\"compact_ms\");")
    endif()
    file(CONFIGURE OUTPUT "${probe_dir}/${source}.cpp" CONTENT "${contents}" @ONLY)
endforeach()

set(rapid_header "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/rapidobj.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rapid_header}")
file(READ "${rapid_header}" contents)
obj_probe_replace(contents "FILE_ATTRIBUTE_READONLY | FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED"
    "FILE_ATTRIBUTE_READONLY | FILE_FLAG_OVERLAPPED")
file(CONFIGURE OUTPUT "${probe_dir}/cached/rapidobj/rapidobj.hpp" CONTENT "${contents}" @ONLY)

add_library(woby_obj_competitors OBJECT "${probe_tests}/fast.cpp" "${probe_tests}/tiny.cpp")
# The pinned tinyobj header's fast_float shim has a non-constexpr distance()
# called from a C++20 constexpr function (MSVC C3615). Its supported C++17
# path retains fast_float and every runtime optimization without patching it.
set_target_properties(woby_obj_competitors PROPERTIES CXX_STANDARD 17)
target_include_directories(woby_obj_competitors SYSTEM PRIVATE "${obj_bench_fast_SOURCE_DIR}" "${obj_bench_tiny_SOURCE_DIR}")
target_link_libraries(woby_obj_competitors PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
target_compile_definitions(woby_obj_competitors PRIVATE _CRT_SECURE_NO_WARNINGS)
woby_enable_project_warnings(woby_obj_competitors)
if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|amd64|x86_64")
    # Enables the real AVX2 implementation selected by __AVX2__, not just SSE2.
    set_source_files_properties("${probe_tests}/tiny.cpp" PROPERTIES COMPILE_OPTIONS /arch:AVX2)
endif()

foreach(mode IN ITEMS native cached)
    set(target "woby_obj_benchmark_${mode}")
    add_executable(${target} "${probe_tests}/main.cpp" "${probe_tests}/rapid.cpp"
        "${probe_dir}/obj_mesh.cpp" "${probe_dir}/model_mesh.cpp" $<TARGET_OBJECTS:woby_obj_competitors>)
    target_include_directories(${target} PRIVATE "${probe_tests}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    if(mode STREQUAL cached)
        target_include_directories(${target} SYSTEM PRIVATE "${probe_dir}/cached")
    else()
        target_include_directories(${target} SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rapidobj/include")
    endif()
    target_compile_definitions(${target} PRIVATE OBJ_PROBE_IO="${mode}")
    target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json meshoptimizer::meshoptimizer Threads::Threads)
    if(WIN32)
        target_link_libraries(${target} PRIVATE psapi)
    endif()
    woby_enable_project_warnings(${target})
    list(APPEND woby_runtime_targets ${target})
endforeach()
if(BUILD_TESTING)
    add_test(NAME woby_obj_benchmark_contract COMMAND "${Python3_EXECUTABLE}"
        "${probe_tests}/contract_test.py" "$<TARGET_FILE:woby_obj_benchmark_native>")
endif()
