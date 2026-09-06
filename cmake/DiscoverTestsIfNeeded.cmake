# Called from CTest's include file, so discovery also works after building only
# woby_tests. Keep each configuration's test list separate.
if(NOT EXISTS "${CTEST_FILE}"
   OR "${TEST_EXECUTABLE}" IS_NEWER_THAN "${CTEST_FILE}"
   OR "${DISCOVERY_CONFIG}" IS_NEWER_THAN "${CTEST_FILE}"
   OR "${DOCTEST_SCRIPT}" IS_NEWER_THAN "${CTEST_FILE}"
   OR "${CMAKE_CURRENT_LIST_FILE}" IS_NEWER_THAN "${CTEST_FILE}")
    set(TEST_SPEC)
    set(TEST_EXTRA_ARGS)
    set(TEST_PROPERTIES)
    set(TEST_ADD_LABELS OFF)
    set(TEST_PREFIX)
    set(TEST_SUFFIX)
    set(TEST_LIST "${TEST_TARGET}_TESTS")
    set(TEST_JUNIT_OUTPUT_DIR)
    # Reuse doctest's parser, escaping rules and per-case CTest registration.
    include("${DOCTEST_SCRIPT}")
endif()
