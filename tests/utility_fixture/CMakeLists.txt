# Match the upstream utility compiler mode in an isolated directory, including
# removing CMake's default MSVC /EHsc before disabling exception handling.
if(MSVC OR CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()
add_library(woby_utility_allocation_fixture STATIC ../utility_allocation_fixture.cpp)
target_include_directories(woby_utility_allocation_fixture SYSTEM PUBLIC
    "${woby_ngapi_SOURCE_DIR}/include" "${woby_ngapi_SOURCE_DIR}/utility/include")
target_include_directories(woby_utility_allocation_fixture SYSTEM PRIVATE "${woby_ngapi_SOURCE_DIR}/utility/src")
target_compile_definitions(woby_utility_allocation_fixture PUBLIC WOBY_UTILITY_ALLOCATION_TESTS)
NoGraphicsAPI_disable_exceptions(woby_utility_allocation_fixture)
if(MSVC)
    target_compile_options(woby_utility_allocation_fixture PRIVATE /W4 /permissive-)
else()
    target_compile_options(woby_utility_allocation_fixture PRIVATE -Wall -Wextra -Wpedantic)
endif()
