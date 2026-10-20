# Runs tests/tree-sitter/run-tree-sitter-tests.sh (SCRIPT) with the sxml
# command (PROGRAM) and fails when fewer tests pass than BASELINE_FILE
# records, or when the driver stopped on a file. The baseline only goes
# up: when more pass, the change that made them pass raises the number in
# the file (packet 7.22, D285; after tests/ixml/run-suite-test.cmake).
execute_process(
    COMMAND sh "${SCRIPT}" --program "${PROGRAM}"
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
    RESULT_VARIABLE actual_exit_code)
message("${actual_stdout}")
if(NOT actual_stderr STREQUAL "")
    message("${actual_stderr}")
endif()
if(NOT (actual_exit_code STREQUAL "0" OR actual_exit_code STREQUAL "1"))
    message(FATAL_ERROR "${SCRIPT} exited with ${actual_exit_code}")
endif()
if(NOT actual_stdout MATCHES "total: tests=[0-9]+ passed=([0-9]+) [^\n]*\nstopped-files=([0-9]+)")
    message(FATAL_ERROR "no total line in the report")
endif()
set(passed_count "${CMAKE_MATCH_1}")
if(NOT CMAKE_MATCH_2 STREQUAL "0")
    message(FATAL_ERROR "the driver stopped on ${CMAKE_MATCH_2} files")
endif()
file(READ "${BASELINE_FILE}" baseline_text)
string(STRIP "${baseline_text}" baseline_count)
if(passed_count LESS baseline_count)
    message(FATAL_ERROR
        "${passed_count} tree-sitter tests passed, fewer than the baseline ${baseline_count} "
        "(${BASELINE_FILE})")
endif()
if(passed_count GREATER baseline_count)
    message("${passed_count} tree-sitter tests passed, more than the baseline "
            "${baseline_count}: raise ${BASELINE_FILE}")
endif()
