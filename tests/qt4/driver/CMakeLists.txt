# The QT4 test-suite driver, sxml-qt4 (docs/QT4_CATALOG.md, D44, D45, D83):
# a developer tool on the CLI framework that reads the vendored QT4 catalog
# with sxml's own XML parser, through the public API only. Joins the build
# from the root CMakeLists.txt once this file exists.
#
#   sxml_qt4_driver_library  every *.c here except main.c and *_test.c: the
#                            catalog, environments, applicability, test
#                            lists and the commands. A new module needs no
#                            edit here.
#   sxml_qt4_driver          the executable "sxml-qt4": main.c and the stdio
#                            platform layer of the sxml program.
#   qt4_driver               ctest unit tests (driver_test.c), over
#                            in-memory files.
#   qt4/<name>               end-to-end golden tests of sxml-qt4.
#   qt4/slice                the torture slice, against a baseline that
#                            only goes up (run-slice-test.cmake).

file(GLOB SXML_QT4_DRIVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
list(FILTER SXML_QT4_DRIVER_SOURCES EXCLUDE REGEX "/(main|[a-z_]*_test)\\.c$")
list(SORT SXML_QT4_DRIVER_SOURCES)

add_library(sxml_qt4_driver_library STATIC ${SXML_QT4_DRIVER_SOURCES})
target_include_directories(sxml_qt4_driver_library PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sxml_qt4_driver_library PUBLIC sxml_cli_framework)
sxml_apply_project_flags(sxml_qt4_driver_library)

# The platform layer is the sxml program's: the one file allowed to touch
# stdio (D44).
add_executable(sxml_qt4_driver
    main.c
    "${PROJECT_SOURCE_DIR}/apps/sxml/platform_stdio.c"
)
target_include_directories(sxml_qt4_driver PRIVATE "${PROJECT_SOURCE_DIR}/apps/sxml")
target_link_libraries(sxml_qt4_driver PRIVATE sxml_qt4_driver_library)
set_target_properties(sxml_qt4_driver PROPERTIES OUTPUT_NAME sxml-qt4)
sxml_apply_project_flags(sxml_qt4_driver)

add_executable(qt4_driver_test driver_test.c)
target_link_libraries(qt4_driver_test PRIVATE sxml_qt4_driver_library sxml_test_runner)
sxml_apply_project_flags(qt4_driver_test)
add_test(NAME qt4_driver COMMAND qt4_driver_test)

# sxml_add_qt4_test(<name> EXIT_CODE <code> [GOLDEN <golden>]
#                   [IN_SOURCE_ROOT] [ARGUMENTS <argument>...])
# Runs sxml-qt4 and compares standard output, standard error and the exit
# code with expected/<golden>.stdout and .stderr, through the CLI tests'
# tests/cli/run-cli-test.cmake (SXML_UPDATE_GOLDEN=1 rewrites them). The
# driver runs in input/, which holds a small suite, or with IN_SOURCE_ROOT
# in the repository root, where the default --catalog finds the vendored
# suite. Paths are relative, so the tests run unchanged under WASI, whose
# only visible directory is the working directory.
function(sxml_add_qt4_test name)
    cmake_parse_arguments(PARSE_ARGV 1 qt4 "IN_SOURCE_ROOT" "EXIT_CODE;GOLDEN" "ARGUMENTS")
    if(NOT DEFINED qt4_EXIT_CODE)
        message(FATAL_ERROR "sxml_add_qt4_test(${name}): EXIT_CODE is required")
    endif()
    if(NOT qt4_GOLDEN)
        set(qt4_GOLDEN "${name}")
    endif()
    set(working_directory "${CMAKE_CURRENT_SOURCE_DIR}/input")
    if(qt4_IN_SOURCE_ROOT)
        set(working_directory "${PROJECT_SOURCE_DIR}")
    endif()
    set(argument_text "")
    foreach(argument IN LISTS qt4_ARGUMENTS)
        string(APPEND argument_text " [==[${argument}]==]")
    endforeach()
    set(emulator_text "")
    foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        string(APPEND emulator_text " [==[${emulator_part}]==]")
    endforeach()
    set(golden_base "${CMAKE_CURRENT_SOURCE_DIR}/expected/${qt4_GOLDEN}")
    set(script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${name}.cmake")
    file(GENERATE OUTPUT "${script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_qt4_driver>]==])
set(EMULATOR${emulator_text})
set(ARGUMENTS${argument_text})
set(EXPECTED_EXIT_CODE ${qt4_EXIT_CODE})
set(EXPECTED_STDOUT_FILE [==[${golden_base}.stdout]==])
set(EXPECTED_STDERR_FILE [==[${golden_base}.stderr]==])
set(WORKING_DIRECTORY [==[${working_directory}]==])
include([==[${PROJECT_SOURCE_DIR}/tests/cli/run-cli-test.cmake]==])
")
    add_test(NAME qt4/${name} COMMAND "${CMAKE_COMMAND}" -P "${script}")
endfunction()

# qt4/slice: the torture slice run and checked; fails when fewer test cases
# pass than tests/qt4/slice-baseline.txt records (packet 1.25).
set(slice_script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/slice.cmake")
set(slice_emulator_text "")
foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
    string(APPEND slice_emulator_text " [==[${emulator_part}]==]")
endforeach()
file(GENERATE OUTPUT "${slice_script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_qt4_driver>]==])
set(EMULATOR${slice_emulator_text})
set(WORKING_DIRECTORY [==[${PROJECT_SOURCE_DIR}]==])
set(BASELINE_FILE [==[${PROJECT_SOURCE_DIR}/tests/qt4/slice-baseline.txt]==])
include([==[${CMAKE_CURRENT_SOURCE_DIR}/run-slice-test.cmake]==])
")
add_test(NAME qt4/slice COMMAND "${CMAKE_COMMAND}" -P "${slice_script}")

# sxml-qt4 run over the small suite of input/run-catalog.xml: every
# assertion kind passing and failing, and every outcome.
sxml_add_qt4_test(run_pass EXIT_CODE 0
    ARGUMENTS run --catalog=run-catalog.xml --test-set=run-pass)
sxml_add_qt4_test(run_fail EXIT_CODE 1
    ARGUMENTS run --catalog=run-catalog.xml --test-set=run-fail)
sxml_add_qt4_test(run_json EXIT_CODE 0
    ARGUMENTS run --catalog=run-catalog.xml --format=json --test-set=run-pass)
sxml_add_qt4_test(run_unknown_test_set EXIT_CODE 1
    ARGUMENTS run --catalog=run-catalog.xml --test-set=no-such-set)
sxml_add_qt4_test(run_help EXIT_CODE 0 ARGUMENTS run --help)

# The torture slice against the vendored suite: all 198 test cases resolve
# and apply to XP40 (packet 1.24).
sxml_add_qt4_test(list_slice EXIT_CODE 0 IN_SOURCE_ROOT
    ARGUMENTS list tests/qt4/torture-slice.txt)

# The small suite in input/: every kind of environment, dependency and
# problem.
sxml_add_qt4_test(list_test_set EXIT_CODE 0
    ARGUMENTS list --catalog=catalog.xml --test-set=set-basic)
sxml_add_qt4_test(list_test_set_xquery EXIT_CODE 0
    ARGUMENTS list --catalog=catalog.xml --spec=XQ40 --test-set=set-basic)
sxml_add_qt4_test(list_spec_replacement EXIT_CODE 0
    ARGUMENTS list --catalog=catalog.xml --test-set=set-xquery)
sxml_add_qt4_test(list_unknown_environment EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml --test-set=set-bad-environment)
sxml_add_qt4_test(list_test_list EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml list.txt)
sxml_add_qt4_test(list_malformed_test_list EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml malformed-list.txt)
sxml_add_qt4_test(list_whole_catalog EXIT_CODE 3
    ARGUMENTS list --catalog=catalog.xml)
sxml_add_qt4_test(list_unknown_test_set EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml --test-set=no-such-set)
sxml_add_qt4_test(list_missing_catalog EXIT_CODE 3
    ARGUMENTS list --catalog=missing/catalog.xml)
sxml_add_qt4_test(list_missing_test_list EXIT_CODE 3
    ARGUMENTS list --catalog=catalog.xml missing-list.txt)
sxml_add_qt4_test(list_both_selections EXIT_CODE 2
    ARGUMENTS list --catalog=catalog.xml --test-set=set-basic list.txt)
sxml_add_qt4_test(list_help EXIT_CODE 0 ARGUMENTS list --help)
sxml_add_qt4_test(program_help EXIT_CODE 0 ARGUMENTS --help)
