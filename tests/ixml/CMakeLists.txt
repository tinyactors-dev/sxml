# ixml/suite: the iXML community test suite (third_party/ixml-tests,
# packet 7.18, D215), run by tests/ixml/run-ixml-tests.sh with the sxml
# command and checked against a baseline that only goes up
# (tests/ixml/suite-baseline.txt), as the conformance slices are
# (tests/qt4/driver/run-slice-test.cmake). It needs a POSIX shell and a
# native sxml, so it is not registered for WASI or Windows.
if(TARGET sxml_cli AND UNIX AND NOT CMAKE_CROSSCOMPILING)
    add_test(NAME ixml/suite
        COMMAND "${CMAKE_COMMAND}"
            "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/run-ixml-tests.sh"
            "-DPROGRAM=$<TARGET_FILE:sxml_cli>"
            "-DBASELINE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/suite-baseline.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run-suite-test.cmake")
endif()
