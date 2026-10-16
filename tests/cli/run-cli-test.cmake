# Runs the sxml executable once and compares its standard output, standard
# error and exit code with golden files (docs/CLI.md, D44). Included by the
# per-test scripts that tests/cli/CMakeLists.txt generates, which set:
#
#   PROGRAM               path of the sxml executable
#   EMULATOR              command that runs it (Node.js for WASI), may be empty
#   ARGUMENTS             the command-line arguments, a list
#   EXPECTED_EXIT_CODE    the exit code
#   EXPECTED_STDOUT_FILE  golden standard output; a missing file means empty
#   EXPECTED_STDERR_FILE  golden standard error; a missing file means empty
#
# With the environment variable SXML_UPDATE_GOLDEN=1 the golden files are
# rewritten from the actual output instead (then review the diff).

execute_process(
    COMMAND ${EMULATOR} "${PROGRAM}" ${ARGUMENTS}
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
    RESULT_VARIABLE actual_exit_code)

if("$ENV{SXML_UPDATE_GOLDEN}" STREQUAL "1")
    foreach(stream stdout stderr)
        string(TOUPPER "${stream}" stream_upper)
        if(actual_${stream} STREQUAL "")
            file(REMOVE "${EXPECTED_${stream_upper}_FILE}")
        else()
            file(WRITE "${EXPECTED_${stream_upper}_FILE}" "${actual_${stream}}")
        endif()
    endforeach()
endif()

set(failures "")
if(NOT actual_exit_code STREQUAL EXPECTED_EXIT_CODE)
    string(APPEND failures
        "exit code: expected ${EXPECTED_EXIT_CODE}, got ${actual_exit_code}\n")
endif()
foreach(stream stdout stderr)
    string(TOUPPER "${stream}" stream_upper)
    set(expected "")
    if(EXISTS "${EXPECTED_${stream_upper}_FILE}")
        file(READ "${EXPECTED_${stream_upper}_FILE}" expected)
    endif()
    if(NOT actual_${stream} STREQUAL expected)
        string(APPEND failures
            "${stream} differs from ${EXPECTED_${stream_upper}_FILE}\n"
            "--- expected ---\n${expected}--- actual ---\n${actual_${stream}}--- end ---\n")
    endif()
endforeach()

if(NOT failures STREQUAL "")
    string(REPLACE ";" " " shown_arguments "${ARGUMENTS}")
    message(FATAL_ERROR "sxml ${shown_arguments}\n${failures}")
endif()
