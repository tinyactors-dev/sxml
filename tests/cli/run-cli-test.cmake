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
#   WORKING_DIRECTORY     where the program runs (tests/cli/input)
#
# and, for a test that writes files (WORKSPACE in tests/cli/CMakeLists.txt):
#
#   WORKSPACE_DIRECTORY   an empty directory to run in instead, made anew
#   WORKSPACE_INPUTS      files of WORKING_DIRECTORY copied into it first
#   WORKSPACE_EXISTING    files made in it first, with the text "old"
#   EXPECTED_FILES_FILE   golden list of the files in it after the run,
#                         inputs left out unless changed: "== PATH" and
#                         the contents
#
# With the environment variable SXML_UPDATE_GOLDEN=1 the golden files are
# rewritten from the actual output instead (then review the diff).

if(DEFINED WORKSPACE_DIRECTORY)
    file(REMOVE_RECURSE "${WORKSPACE_DIRECTORY}")
    file(MAKE_DIRECTORY "${WORKSPACE_DIRECTORY}")
    foreach(input IN LISTS WORKSPACE_INPUTS)
        get_filename_component(input_directory "${WORKSPACE_DIRECTORY}/${input}" DIRECTORY)
        file(MAKE_DIRECTORY "${input_directory}")
        file(COPY_FILE "${WORKING_DIRECTORY}/${input}" "${WORKSPACE_DIRECTORY}/${input}")
    endforeach()
    foreach(existing IN LISTS WORKSPACE_EXISTING)
        get_filename_component(existing_directory "${WORKSPACE_DIRECTORY}/${existing}" DIRECTORY)
        file(MAKE_DIRECTORY "${existing_directory}")
        file(WRITE "${WORKSPACE_DIRECTORY}/${existing}" "old\n")
    endforeach()
    set(INPUT_DIRECTORY "${WORKING_DIRECTORY}")
    set(WORKING_DIRECTORY "${WORKSPACE_DIRECTORY}")
endif()

execute_process(
    COMMAND ${EMULATOR} "${PROGRAM}" ${ARGUMENTS}
    WORKING_DIRECTORY "${WORKING_DIRECTORY}"
    OUTPUT_VARIABLE actual_stdout
    ERROR_VARIABLE actual_stderr
    RESULT_VARIABLE actual_exit_code)

set(actual_files "")
if(DEFINED WORKSPACE_DIRECTORY)
    file(GLOB_RECURSE written_files LIST_DIRECTORIES false RELATIVE "${WORKSPACE_DIRECTORY}"
        "${WORKSPACE_DIRECTORY}/*")
    list(SORT written_files)
    foreach(written IN LISTS written_files)
        file(READ "${WORKSPACE_DIRECTORY}/${written}" written_contents)
        set(is_unchanged_input FALSE)
        list(FIND WORKSPACE_INPUTS "${written}" input_index)
        if(NOT input_index EQUAL -1)
            file(READ "${INPUT_DIRECTORY}/${written}" input_contents)
            if(input_contents STREQUAL written_contents)
                set(is_unchanged_input TRUE)
            endif()
        endif()
        if(NOT is_unchanged_input)
            string(APPEND actual_files "== ${written}\n${written_contents}")
        endif()
    endforeach()
endif()

if("$ENV{SXML_UPDATE_GOLDEN}" STREQUAL "1" AND DEFINED WORKSPACE_DIRECTORY)
    if(actual_files STREQUAL "")
        file(REMOVE "${EXPECTED_FILES_FILE}")
    else()
        file(WRITE "${EXPECTED_FILES_FILE}" "${actual_files}")
    endif()
endif()
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

if(DEFINED WORKSPACE_DIRECTORY)
    set(expected "")
    if(EXISTS "${EXPECTED_FILES_FILE}")
        file(READ "${EXPECTED_FILES_FILE}" expected)
    endif()
    if(NOT actual_files STREQUAL expected)
        string(APPEND failures
            "written files differ from ${EXPECTED_FILES_FILE}\n"
            "--- expected ---\n${expected}--- actual ---\n${actual_files}--- end ---\n")
    endif()
endif()

if(NOT failures STREQUAL "")
    string(REPLACE ";" " " shown_arguments "${ARGUMENTS}")
    message(FATAL_ERROR "sxml ${shown_arguments}\n${failures}")
endif()
