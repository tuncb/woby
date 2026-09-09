cmake_minimum_required(VERSION 3.24)

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(work "${TEST_ROOT}/python selection ${run_id}")
file(MAKE_DIRECTORY "${work}/cmake")
configure_file("${PYTHON_MODULE}" "${work}/cmake/WobyPython.cmake" COPYONLY)
configure_file("${PYTHON_PIN}" "${work}/.python-version" COPYONLY)
file(WRITE "${work}/select.cmake" [=[
cmake_minimum_required(VERSION 3.24)
# Simulate the old cache pointing at an unrelated executable/environment.
set(Python3_EXECUTABLE "${CMAKE_COMMAND}" CACHE FILEPATH "Stale interpreter" FORCE)
set(_Python3_EXECUTABLE "${CMAKE_COMMAND}" CACHE INTERNAL "Stale discovery" FORCE)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/WobyPython.cmake")
file(TO_CMAKE_PATH "${Python3_EXECUTABLE}" selected)
file(TO_CMAKE_PATH "${EXPECTED_PYTHON}" expected)
if(NOT selected STREQUAL expected)
    message(FATAL_ERROR "Expected ${expected}, selected ${selected}")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" -S -c
    "import sys; print('.'.join(map(str, sys.version_info[:3])))"
    RESULT_VARIABLE result OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result STREQUAL "0" OR NOT version STREQUAL WOBY_PYTHON_VERSION)
    message(FATAL_ERROR "Selected interpreter does not match the pin: ${version}")
endif()
]=])

function(check_selection label expected_error)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DWOBY_UV_EXECUTABLE=${UV_EXECUTABLE}"
        "-DEXPECTED_PYTHON=${EXPECTED_PYTHON}"
        -P "${work}/select.cmake"
        WORKING_DIRECTORY "${work}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(expected_error STREQUAL "")
        if(NOT result STREQUAL "0")
            message(FATAL_ERROR "${label}: ${output}${error}")
        endif()
    elseif(result STREQUAL "0" OR NOT "${output}${error}" MATCHES "${expected_error}")
        message(FATAL_ERROR "${label}: expected '${expected_error}', got ${result}: ${output}${error}")
    endif()
endfunction()

check_selection("Replace stale Python cache in a path with spaces" "")
file(WRITE "${work}/.python-version" "3.13\n")
check_selection("Reject an unpinned interpreter" "must pin an exact Python 3")
file(WRITE "${work}/.python-version" "3.99.99\n")
check_selection("Missing interpreter fails without downloading or using system Python" "Run 'uv python install'")
message(STATUS "uv Python selection checks passed")
