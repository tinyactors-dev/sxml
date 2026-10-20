# tree-sitter/run-time-language (packet 7.23, D288): the sxml command
# loads a tree-sitter language at run time from a shared library
# (--code-language=LIBRARY) with the queries beside it. LIBRARY is the
# JSON grammar of third_party/tree-sitter-grammars built as a module whose
# entry point is tree_sitter_testjson, so its name is "testjson", a
# language sxml does not include; QUERIES is JSON's queries directory.
# Checks:
#   1. the language is listed and parses, with the highlights of its query;
#   2. without a queries directory it parses without highlights;
#   3. a query that does not fit the grammar is reported (exit 1);
#   4. a file that is no library is reported (exit 3).
set(work "${WORK_DIRECTORY}")
file(REMOVE_RECURSE "${work}")
get_filename_component(library_name "${LIBRARY}" NAME)
file(MAKE_DIRECTORY "${work}/good/queries" "${work}/bare" "${work}/bad/queries")
file(COPY "${LIBRARY}" DESTINATION "${work}/good")
file(COPY "${LIBRARY}" DESTINATION "${work}/bare")
file(COPY "${LIBRARY}" DESTINATION "${work}/bad")
file(COPY "${QUERIES}/highlights.scm" DESTINATION "${work}/good/queries")
file(WRITE "${work}/bad/queries/highlights.scm" "(no_such_node) @error\n")
file(WRITE "${work}/not-a-library.so" "not a library\n")

function(run_case name expected_exit expected_output)
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        WORKING_DIRECTORY "${work}"
        OUTPUT_VARIABLE actual_stdout
        ERROR_VARIABLE actual_stderr
        RESULT_VARIABLE actual_exit)
    set(actual "${actual_stdout}${actual_stderr}")
    if(NOT actual_exit STREQUAL "${expected_exit}")
        message(FATAL_ERROR "${name}: exit ${actual_exit}, expected ${expected_exit}:\n${actual}")
    endif()
    string(FIND "${actual}" "${expected_output}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "${name}: expected\n${expected_output}\ngot\n${actual}")
    endif()
endfunction()

run_case("loaded" 0
    "true\n<document xmlns:sxml=\"urn:sxml\"><array>[<number highlight=\"number\">1</number>, <true highlight=\"constant.builtin\">true</true>]</array></document>"
    xpath "--code-language=good/${library_name}" --method=xml
    "sxml:code-languages() = 'testjson', sxml:parse-code('[1, true]', 'testjson')")
run_case("without queries" 0
    "<document xmlns:sxml=\"urn:sxml\"><array>[<number>1</number>, <true>true</true>]</array></document>"
    xpath "--code-language=bare/${library_name}" --method=xml
    "sxml:parse-code('[1, true]', 'testjson')")
run_case("query that does not fit" 1
    "the highlights query does not fit the grammar at byte 1"
    xpath "--code-language=bad/${library_name}" "sxml:code-languages()")
run_case("no library" 3
    "--code-language=not-a-library.so: cannot load the library"
    xpath "--code-language=not-a-library.so" "sxml:code-languages()")
file(REMOVE_RECURSE "${work}")
