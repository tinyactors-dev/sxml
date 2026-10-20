# tree-sitter/suite: the tests of the vendored tree-sitter grammars
# (third_party/tree-sitter-grammars/LANGUAGE/test: corpus trees and
# highlight assertions; packet 7.22, D285), run by
# tests/tree-sitter/run-tree-sitter-tests.sh with the sxml command and
# checked against a baseline that only goes up
# (tests/tree-sitter/suite-baseline.txt), as ixml/suite is. It needs a
# POSIX shell and a native sxml, so it is not registered for WASI or
# Windows.
if(TARGET sxml_cli AND UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME tree-sitter/suite
        COMMAND "${CMAKE_COMMAND}"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/run-tree-sitter-tests.sh"
            "-DPROGRAM=$<TARGET_FILE:sxml_cli>"
            "-DBASELINE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/suite-baseline.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-suite-test.cmake")
endif()

# tree-sitter/run-time-language (packet 7.23, D288): a tree-sitter
# language loaded at run time from a shared library, built here from the
# vendored JSON grammar with the entry point tree_sitter_testjson, so no
# network is needed (tests/tree-sitter/run-language-test.cmake).
if(TARGET sxml_cli AND UNIX AND NOT CMAKE_CROSSCOMPILING)
    set(json_grammar "${PROJECT_SOURCE_DIR}/third_party/tree-sitter-grammars/json")
    add_library(sxml_test_code_language MODULE "${json_grammar}/src/parser.c")
    target_include_directories(sxml_test_code_language PRIVATE "${json_grammar}/src")
    target_compile_definitions(sxml_test_code_language PRIVATE
        "tree_sitter_json=tree_sitter_testjson")
    set_target_properties(sxml_test_code_language PROPERTIES
        PREFIX "lib" OUTPUT_NAME "tree-sitter-testjson")
    sxml_apply_third_party_flags(sxml_test_code_language)
    add_dependencies(sxml_cli sxml_test_code_language)
    add_test(NAME tree-sitter/run-time-language
        COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=$<TARGET_FILE:sxml_cli>"
            "-DLIBRARY=$<TARGET_FILE:sxml_test_code_language>"
            "-DQUERIES=${json_grammar}/queries"
            "-DWORK_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/run-time-language"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-language-test.cmake")
endif()
