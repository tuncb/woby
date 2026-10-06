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
target_link_libraries(woby_parser_benchmark PRIVATE Threads::Threads nlohmann_json::nlohmann_json)
woby_enable_project_warnings(woby_parser_benchmark)
if(MSVC)
    target_compile_options(woby_parser_benchmark PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(woby_parser_benchmark PRIVATE $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()
