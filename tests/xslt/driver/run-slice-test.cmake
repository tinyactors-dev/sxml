# Runs sxml-xslt-test run over SLICE_FILE and fails when fewer test cases
# pass than BASELINE_FILE records (glossary: slice_baseline). Imitates
# tests/qt4/driver/run-slice-test.cmake.
# The baseline only goes up: when more pass, the test says so, and the
# change that made them pass raises the number in the file. Set by the
# script tests/xslt/driver/CMakeLists.txt generates:
#
#   PROGRAM            path of sxml-xslt-test
#   EMULATOR           command that runs it (Node.js for WASI), may be empty
#   WORKING_DIRECTORY  the repository root, where the default catalog is
#   SLICE_FILE         tests/xslt/slice.txt, relative to WORKING_DIRECTORY
#   BASELINE_FILE      tests/xslt/slice-baseline.txt
#
# Exit codes 0 and 1 (some test case failed) are a finished run; any other
# (a crash, a sanitizer report, an unreadable file) fails the test.

execute_process(
    COMMAND ${EMULATOR} "${PROGRAM}" run "${SLICE_FILE}"
    WORKING_DIRECTORY "${WORKING_DIRECTORY}"
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
    RESULT_VARIABLE actual_exit_code)

message("${actual_stdout}")
if(NOT actual_stderr STREQUAL "")
    message("${actual_stderr}")
endif()
if(NOT (actual_exit_code STREQUAL "0" OR actual_exit_code STREQUAL "1"))
    message(FATAL_ERROR "sxml-xslt-test run exited with ${actual_exit_code}")
endif()
if(NOT actual_stdout MATCHES "\ntotal: ([0-9]+) test cases?: ([0-9]+) passed")
    message(FATAL_ERROR "no total line in the report")
endif()
set(passed_count "${CMAKE_MATCH_2}")
file(READ "${BASELINE_FILE}" baseline_text)
string(STRIP "${baseline_text}" baseline_count)
if(passed_count LESS baseline_count)
    message(FATAL_ERROR
        "${passed_count} slice test cases passed, fewer than the baseline "
        "${baseline_count} (${BASELINE_FILE})")
endif()
if(passed_count GREATER baseline_count)
    message("${passed_count} slice test cases passed: raise the baseline "
            "${baseline_count} in ${BASELINE_FILE}")
endif()
