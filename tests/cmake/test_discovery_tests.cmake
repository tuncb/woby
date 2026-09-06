cmake_minimum_required(VERSION 3.24)

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef run_id)
set(work "${TEST_ROOT}/discovery-${run_id}")
file(MAKE_DIRECTORY "${work}/Debug" "${work}/Release")
file(WRITE "${work}/executable" "binary placeholder")
file(WRITE "${work}/config.cmake" "# configuration placeholder")
configure_file("${DOCTEST_SCRIPT}" "${work}/doctestAddTests.cmake" COPYONLY)
file(WRITE "${work}/fake_doctest.cmake"
"file(APPEND [==[${work}/calls.txt]==] \"called\\n\")
execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E echo \"a discovered test\")
")

function(discover configuration)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DTEST_TARGET=fake_tests"
        "-DTEST_EXECUTABLE=${work}/executable"
        "-DTEST_EXECUTOR=${CMAKE_COMMAND};-P;${work}/fake_doctest.cmake"
        "-DTEST_WORKING_DIR=${work}"
        "-DCTEST_FILE=${work}/${configuration}/tests.cmake"
        "-DDISCOVERY_CONFIG=${work}/config.cmake"
        "-DDOCTEST_SCRIPT=${work}/doctestAddTests.cmake"
        -P "${DISCOVERY_SCRIPT}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Discovery failed: ${output}${error}")
    endif()
endfunction()

function(expect_calls expected)
    file(STRINGS "${work}/calls.txt" calls)
    list(LENGTH calls count)
    if(NOT count EQUAL expected)
        message(FATAL_ERROR "Expected ${expected} discovery calls, got ${count}")
    endif()
endfunction()

discover(Debug)
expect_calls(1)
file(READ "${work}/Debug/tests.cmake" test_list)
if(NOT test_list MATCHES "a discovered test")
    message(FATAL_ERROR "Discovery did not register the fake test")
endif()
discover(Debug)
expect_calls(1)
discover(Release)
expect_calls(2)
discover(Debug)
expect_calls(2)
file(REMOVE "${work}/Debug/tests.cmake")
discover(Debug)
expect_calls(3)
file(WRITE "${work}/executable" "changed executable")
discover(Debug)
expect_calls(4)
discover(Debug)
expect_calls(4)
file(APPEND "${work}/config.cmake" "\n# changed configuration")
discover(Debug)
expect_calls(5)
file(APPEND "${work}/doctestAddTests.cmake" "\n# changed discovery parser")
discover(Debug)
expect_calls(6)
