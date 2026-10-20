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
