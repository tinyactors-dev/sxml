# The XSD 1.1 test-suite driver, sxml-xsd-test (docs/QT4_CATALOG.md, "The
# XSD test suite"; D44, D165, D168): a developer tool on the CLI framework,
# a sibling of sxml-qt4 and sxml-xslt-test whose library (tests/qt4/driver)
# it links for documents, test lists, the test selection, the test budget,
# reasons, reports and the slice test. Joins the build from the root
# CMakeLists.txt, after tests/xslt/driver, once this file exists.
#
#   sxml_xsd_driver_library  every *.c here except main.c and *_test.c.
#                            A new module needs no edit here.
#   sxml_xsd_driver          the executable "sxml-xsd-test".
#   xsd_driver               ctest unit tests (xsd_driver_test.c), over
#                            in-memory files.
#   xsd/<name>               end-to-end golden tests of sxml-xsd-test.
#   xsd/slice                the XSD slice, against a baseline that only
#                            goes up (tests/qt4/driver/run-slice-test.cmake).

file(GLOB SXML_XSD_DRIVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
list(FILTER SXML_XSD_DRIVER_SOURCES EXCLUDE REGEX "/(main|[a-z_]*_test)\\.c$")
list(SORT SXML_XSD_DRIVER_SOURCES)

add_library(sxml_xsd_driver_library STATIC ${SXML_XSD_DRIVER_SOURCES})
target_include_directories(sxml_xsd_driver_library PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sxml_xsd_driver_library PUBLIC sxml_qt4_driver_library)
sxml_apply_project_flags(sxml_xsd_driver_library)

add_executable(sxml_xsd_driver
    main.c
    "${PROJECT_SOURCE_DIR}/apps/sxml/platform_stdio.c"
)
target_include_directories(sxml_xsd_driver PRIVATE "${PROJECT_SOURCE_DIR}/apps/sxml")
target_link_libraries(sxml_xsd_driver PRIVATE sxml_xsd_driver_library)
set_target_properties(sxml_xsd_driver PROPERTIES OUTPUT_NAME sxml-xsd-test)
sxml_apply_project_flags(sxml_xsd_driver)

add_executable(xsd_driver_test xsd_driver_test.c)
target_link_libraries(xsd_driver_test PRIVATE sxml_xsd_driver_library sxml_test_runner)
sxml_apply_project_flags(xsd_driver_test)
add_test(NAME xsd_driver COMMAND xsd_driver_test)

# sxml_add_xsd_test(<name> EXIT_CODE <code> [GOLDEN <golden>]
#                   [IN_SOURCE_ROOT] [ARGUMENTS <argument>...])
# As sxml_add_xslt_test in tests/xslt/driver/CMakeLists.txt: runs
# sxml-xsd-test in input/ (a small suite) or, with IN_SOURCE_ROOT, in the
# repository root, and compares standard output, standard error and the
# exit code with expected/<golden>.stdout and .stderr
# (SXML_UPDATE_GOLDEN=1 rewrites them).
function(sxml_add_xsd_test name)
    cmake_parse_arguments(PARSE_ARGV 1 xsd "IN_SOURCE_ROOT" "EXIT_CODE;GOLDEN" "ARGUMENTS")
    if(NOT DEFINED xsd_EXIT_CODE)
        message(FATAL_ERROR "sxml_add_xsd_test(${name}): EXIT_CODE is required")
    endif()
    if(NOT xsd_GOLDEN)
        set(xsd_GOLDEN "${name}")
    endif()
    set(working_directory "${CMAKE_CURRENT_SOURCE_DIR}/input")
    if(xsd_IN_SOURCE_ROOT)
        set(working_directory "${PROJECT_SOURCE_DIR}")
    endif()
    set(argument_text "")
    foreach(argument IN LISTS xsd_ARGUMENTS)
        string(APPEND argument_text " [==[${argument}]==]")
    endforeach()
    set(emulator_text "")
    foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        string(APPEND emulator_text " [==[${emulator_part}]==]")
    endforeach()
    set(golden_base "${CMAKE_CURRENT_SOURCE_DIR}/expected/${xsd_GOLDEN}")
    set(script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${name}.cmake")
    file(GENERATE OUTPUT "${script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_xsd_driver>]==])
set(EMULATOR${emulator_text})
set(ARGUMENTS${argument_text})
set(EXPECTED_EXIT_CODE ${xsd_EXIT_CODE})
set(EXPECTED_STDOUT_FILE [==[${golden_base}.stdout]==])
set(EXPECTED_STDERR_FILE [==[${golden_base}.stderr]==])
set(WORKING_DIRECTORY [==[${working_directory}]==])
include([==[${PROJECT_SOURCE_DIR}/tests/cli/run-cli-test.cmake]==])
")
    add_test(NAME xsd/${name} COMMAND "${CMAKE_COMMAND}" -P "${script}")
endfunction()

# xsd/slice: the XSD slice run and checked; fails when fewer tests pass
# than tests/xsd/slice-baseline.txt records.
set(slice_script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/slice.cmake")
set(slice_emulator_text "")
foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
    string(APPEND slice_emulator_text " [==[${emulator_part}]==]")
endforeach()
file(GENERATE OUTPUT "${slice_script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_xsd_driver>]==])
set(EMULATOR${slice_emulator_text})
set(WORKING_DIRECTORY [==[${PROJECT_SOURCE_DIR}]==])
set(SLICE_FILE tests/xsd/slice.txt)
set(BASELINE_FILE [==[${PROJECT_SOURCE_DIR}/tests/xsd/slice-baseline.txt]==])
include([==[${PROJECT_SOURCE_DIR}/tests/qt4/driver/run-slice-test.cmake]==])
")
add_test(NAME xsd/slice COMMAND "${CMAKE_COMMAND}" -P "${slice_script}")

# The small suite in input/: schema and instance tests, a group without a
# schema test, every reason a test does not apply, and the problems.
sxml_add_xsd_test(list_test_set EXIT_CODE 0
    ARGUMENTS list --catalog=suite.xml --test-set=one-basic)
sxml_add_xsd_test(list_versions EXIT_CODE 0
    ARGUMENTS list --catalog=suite.xml --test-set=one-versions)
sxml_add_xsd_test(list_problems EXIT_CODE 1
    ARGUMENTS list --catalog=suite.xml --test-set=two-problems)
sxml_add_xsd_test(list_test_list EXIT_CODE 1
    ARGUMENTS list --catalog=suite.xml list.txt)
sxml_add_xsd_test(list_unknown_test_set EXIT_CODE 1
    ARGUMENTS list --catalog=suite.xml --test-set=no-such-set)
sxml_add_xsd_test(list_missing_catalog EXIT_CODE 3
    ARGUMENTS list --catalog=missing/suite.xml)
sxml_add_xsd_test(list_both_selections EXIT_CODE 2
    ARGUMENTS list --catalog=suite.xml --test-set=one-basic list.txt)
sxml_add_xsd_test(list_help EXIT_CODE 0 ARGUMENTS list --help)
# Schema tests compile (packet 5.4); until packet 5.7 instance tests end in
# "not run" and, for a missing document, an error, hence exit code 1 where
# there is one.
sxml_add_xsd_test(run_test_set EXIT_CODE 0
    ARGUMENTS run --catalog=suite.xml --test-set=one-basic)
sxml_add_xsd_test(run_json EXIT_CODE 0
    ARGUMENTS run --catalog=suite.xml --format=json --test-set=one-versions)
sxml_add_xsd_test(run_problems EXIT_CODE 1
    ARGUMENTS run --catalog=suite.xml --test-set=two-problems)
sxml_add_xsd_test(run_help EXIT_CODE 0 ARGUMENTS run --help)
sxml_add_xsd_test(program_help EXIT_CODE 0 ARGUMENTS --help)

# The slice against the vendored suite: every test resolves and is in
# scope.
sxml_add_xsd_test(list_slice EXIT_CODE 0 IN_SOURCE_ROOT
    ARGUMENTS list tests/xsd/slice.txt)
