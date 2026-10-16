find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(parser_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/rapidobj.hpp"
    "${PROJECT_SOURCE_DIR}/third_party/rapidobj/include/rapidobj/prototype.hpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}" "${parser_generated}" COMMAND_ERROR_IS_FATAL ANY)
add_executable(woby_parser_benchmark benchmark.cpp)
target_include_directories(woby_parser_benchmark SYSTEM PRIVATE "${parser_generated}")
set(color_reference "${CMAKE_CURRENT_BINARY_DIR}/color-reference")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate.py"
    "${PROJECT_SOURCE_DIR}" "${color_reference}" --color-reference COMMAND_ERROR_IS_FATAL ANY)
add_executable(woby_parser_color_reference EXCLUDE_FROM_ALL benchmark.cpp)
target_include_directories(woby_parser_color_reference SYSTEM PRIVATE "${color_reference}")
foreach(target IN ITEMS woby_parser_benchmark woby_parser_color_reference)
    target_link_libraries(${target} PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(${target} PRIVATE psapi)
    endif()
    woby_enable_project_warnings(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    endif()
endforeach()

if(WIN32 AND MSVC)
    set(allocation_generated "${CMAKE_CURRENT_BINARY_DIR}/allocation-generated")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/micro_generate.py"
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/parser_micro.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmark.cpp")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${PROJECT_SOURCE_DIR}/experiments/loading-audit/micro_generate.py"
        "${PROJECT_SOURCE_DIR}" "${parser_generated}" "${allocation_generated}" COMMAND_ERROR_IS_FATAL ANY)
    add_executable(woby_parser_allocation_audit "${allocation_generated}/benchmark.cpp")
    target_include_directories(woby_parser_allocation_audit SYSTEM PRIVATE "${allocation_generated}")
    target_include_directories(woby_parser_allocation_audit PRIVATE "${PROJECT_SOURCE_DIR}/experiments/loading-audit")
    target_link_libraries(woby_parser_allocation_audit PRIVATE Threads::Threads nlohmann_json::nlohmann_json psapi)
    woby_enable_project_warnings(woby_parser_allocation_audit)
    target_compile_options(woby_parser_allocation_audit PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(woby_parser_allocation_audit PRIVATE $<$<CONFIG:Release>:/DEBUG>)
endif()

set(WOBY_PARSER_FASTFLOAT_HEADER "" CACHE FILEPATH "Optional standalone fast_float header for a research-only parser comparison")
if(WOBY_PARSER_FASTFLOAT_HEADER)
    set(modern_generated "${CMAKE_CURRENT_BINARY_DIR}/modern-generated")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/modern_float.py" "${WOBY_PARSER_FASTFLOAT_HEADER}")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/modern_float.py"
        "${parser_generated}" "${modern_generated}" "${WOBY_PARSER_FASTFLOAT_HEADER}" COMMAND_ERROR_IS_FATAL ANY)
    add_executable(woby_parser_benchmark_modern benchmark.cpp)
    target_include_directories(woby_parser_benchmark_modern SYSTEM PRIVATE "${modern_generated}")
    target_link_libraries(woby_parser_benchmark_modern PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
    if(WIN32)
        target_link_libraries(woby_parser_benchmark_modern PRIVATE psapi)
    endif()
    woby_enable_project_warnings(woby_parser_benchmark_modern)
    if(MSVC)
        target_compile_options(woby_parser_benchmark_modern PRIVATE $<$<CONFIG:Release>:/Zi>)
        target_link_options(woby_parser_benchmark_modern PRIVATE $<$<CONFIG:Release>:/DEBUG>)
    endif()
endif()
if(BUILD_TESTING)
    add_test(NAME woby_parser_input_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests.py" --executable "$<TARGET_FILE:woby_parser_benchmark>")
    set_tests_properties(woby_parser_input_contract PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    if(TARGET woby_parser_allocation_audit)
        add_test(NAME woby_parser_allocation_accounting COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/experiments/loading-audit/allocation_tests.py"
            "$<TARGET_FILE:woby_parser_benchmark>" "$<TARGET_FILE:woby_parser_allocation_audit>")
        set_tests_properties(woby_parser_allocation_accounting PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    endif()
    if(TARGET woby_parser_benchmark_modern)
        add_test(NAME woby_parser_modern_input_contract COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests.py" --executable "$<TARGET_FILE:woby_parser_benchmark_modern>")
        set_tests_properties(woby_parser_modern_input_contract PROPERTIES LABELS "unit;cpu" TIMEOUT 60)
    endif()
endif()
