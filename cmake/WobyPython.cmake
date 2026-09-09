# Use the same exact uv-managed interpreter as `uv run` and CI, even when an
# existing CMake cache or an activated environment selected another Python.
get_filename_component(woby_python_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${woby_python_root}/.python-version")
file(READ "${woby_python_root}/.python-version" WOBY_PYTHON_VERSION)
string(STRIP "${WOBY_PYTHON_VERSION}" WOBY_PYTHON_VERSION)
if(NOT WOBY_PYTHON_VERSION MATCHES "^3\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR ".python-version must pin an exact Python 3 major.minor.patch version.")
endif()

find_program(WOBY_UV_EXECUTABLE NAMES uv DOC "uv Python manager")
if(NOT WOBY_UV_EXECUTABLE)
    message(FATAL_ERROR "Python tests require uv. Install uv, then run 'uv python install' in ${woby_python_root}.")
endif()
execute_process(
    COMMAND "${WOBY_UV_EXECUTABLE}" python find --system --managed-python --no-python-downloads "${WOBY_PYTHON_VERSION}"
    WORKING_DIRECTORY "${woby_python_root}"
    RESULT_VARIABLE woby_python_result
    OUTPUT_VARIABLE woby_python_executable
    ERROR_VARIABLE woby_python_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT woby_python_result STREQUAL "0")
    message(FATAL_ERROR "Cannot find uv-managed Python ${WOBY_PYTHON_VERSION}. Run 'uv python install' in ${woby_python_root}.\n${woby_python_error}")
endif()
set(Python3_EXECUTABLE "${woby_python_executable}" CACHE FILEPATH "Pinned uv-managed Python interpreter" FORCE)
find_package(Python3 "${WOBY_PYTHON_VERSION}" EXACT REQUIRED COMPONENTS Interpreter)
