# Called from CTest's include file, so discovery also works after building only
# woby_tests. Keep each configuration's test list separate.
if(NOT EXISTS "${CTEST_FILE}"
   OR "${TEST_EXECUTABLE}" IS_NEWER_THAN "${CTEST_FILE}"
   OR "${DISCOVERY_CONFIG}" IS_NEWER_THAN "${CTEST_FILE}"
   OR "${DOCTEST_SCRIPT}" IS_NEWER_THAN "${CTEST_FILE}"
   OR (TEST_GROUP_BY_SOURCE AND "${TEST_GROUP_SCRIPT}" IS_NEWER_THAN "${CTEST_FILE}")
   OR "${CMAKE_CURRENT_LIST_FILE}" IS_NEWER_THAN "${CTEST_FILE}")
    if(TEST_GROUP_BY_SOURCE)
        set(discovery_arguments)
        foreach(slow_test IN LISTS TEST_SLOW_TESTS)
            string(REPLACE ";" "\\;" slow_argument "--slow-test=${slow_test}")
            list(APPEND discovery_arguments "${slow_argument}")
        endforeach()
        execute_process(COMMAND "${TEST_PYTHON_EXECUTABLE}" "${TEST_GROUP_SCRIPT}"
            "--target=${TEST_TARGET}" "--working-directory=${TEST_WORKING_DIR}"
            "--output=${CTEST_FILE}" ${discovery_arguments}
            -- ${TEST_EXECUTOR} "${TEST_EXECUTABLE}"
            RESULT_VARIABLE discovery_result OUTPUT_VARIABLE discovery_output ERROR_VARIABLE discovery_error)
        if(NOT discovery_result STREQUAL "0")
            message(FATAL_ERROR "Grouped test discovery failed:\n${discovery_output}${discovery_error}")
        endif()
    else()
        set(TEST_SPEC)
        set(TEST_EXTRA_ARGS)
        set(TEST_PROPERTIES)
        set(TEST_ADD_LABELS OFF)
        set(TEST_PREFIX)
        set(TEST_SUFFIX)
        set(TEST_LIST "${TEST_TARGET}_TESTS")
        set(TEST_JUNIT_OUTPUT_DIR)
        # Keep doctest's per-case registration for targets that need it.
        include("${DOCTEST_SCRIPT}")
    endif()
endif()
