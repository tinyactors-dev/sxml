# The XSLT 4.0 test-suite driver, sxml-xslt-test (docs/QT4_CATALOG.md,
# D44, D117): a developer tool on the CLI framework, a sibling of sxml-qt4
# whose library it links for documents, environments, test lists, reasons
# and reports. Joins the build from the root CMakeLists.txt, after
# tests/qt4/driver, once this file exists.
#
#   sxml_xslt_driver_library  every *.c here except main.c and *_test.c.
#                             A new module needs no edit here.
#   sxml_xslt_driver          the executable "sxml-xslt-test".
#   xslt_driver               ctest unit tests (xslt_driver_test.c), over
#                             in-memory files.
#   xslt/<name>               end-to-end golden tests of sxml-xslt-test.
#   xslt/slice                the XSLT slice, against a baseline that only
#                             goes up (run-slice-test.cmake).

file(GLOB SXML_XSLT_DRIVER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
list(FILTER SXML_XSLT_DRIVER_SOURCES EXCLUDE REGEX "/(main|[a-z_]*_test)\\.c$")
list(SORT SXML_XSLT_DRIVER_SOURCES)

add_library(sxml_xslt_driver_library STATIC ${SXML_XSLT_DRIVER_SOURCES})
target_include_directories(sxml_xslt_driver_library PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sxml_xslt_driver_library PUBLIC sxml_qt4_driver_library)
sxml_apply_project_flags(sxml_xslt_driver_library)

add_executable(sxml_xslt_driver
    main.c
    "${PROJECT_SOURCE_DIR}/apps/sxml/platform_stdio.c"
)
target_include_directories(sxml_xslt_driver PRIVATE "${PROJECT_SOURCE_DIR}/apps/sxml")
target_link_libraries(sxml_xslt_driver PRIVATE sxml_xslt_driver_library)
set_target_properties(sxml_xslt_driver PROPERTIES OUTPUT_NAME sxml-xslt-test)
sxml_apply_project_flags(sxml_xslt_driver)

add_executable(xslt_driver_test xslt_driver_test.c)
target_link_libraries(xslt_driver_test PRIVATE sxml_xslt_driver_library sxml_test_runner)
sxml_apply_project_flags(xslt_driver_test)
add_test(NAME xslt_driver COMMAND xslt_driver_test)

# sxml_add_xslt_test(<name> EXIT_CODE <code> [GOLDEN <golden>]
#                    [IN_SOURCE_ROOT] [ARGUMENTS <argument>...])
# As sxml_add_qt4_test in tests/qt4/driver/CMakeLists.txt: runs
# sxml-xslt-test in input/ (a small suite) or, with IN_SOURCE_ROOT, in the
# repository root, and compares standard output, standard error and the
# exit code with expected/<golden>.stdout and .stderr
# (SXML_UPDATE_GOLDEN=1 rewrites them).
function(sxml_add_xslt_test name)
    cmake_parse_arguments(PARSE_ARGV 1 xslt "IN_SOURCE_ROOT" "EXIT_CODE;GOLDEN" "ARGUMENTS")
    if(NOT DEFINED xslt_EXIT_CODE)
        message(FATAL_ERROR "sxml_add_xslt_test(${name}): EXIT_CODE is required")
    endif()
    if(NOT xslt_GOLDEN)
        set(xslt_GOLDEN "${name}")
    endif()
    set(working_directory "${CMAKE_CURRENT_SOURCE_DIR}/input")
    if(xslt_IN_SOURCE_ROOT)
        set(working_directory "${PROJECT_SOURCE_DIR}")
    endif()
    set(argument_text "")
    foreach(argument IN LISTS xslt_ARGUMENTS)
        string(APPEND argument_text " [==[${argument}]==]")
    endforeach()
    set(emulator_text "")
    foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        string(APPEND emulator_text " [==[${emulator_part}]==]")
    endforeach()
    set(golden_base "${CMAKE_CURRENT_SOURCE_DIR}/expected/${xslt_GOLDEN}")
    set(script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${name}.cmake")
    file(GENERATE OUTPUT "${script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_xslt_driver>]==])
set(EMULATOR${emulator_text})
set(ARGUMENTS${argument_text})
set(EXPECTED_EXIT_CODE ${xslt_EXIT_CODE})
set(EXPECTED_STDOUT_FILE [==[${golden_base}.stdout]==])
set(EXPECTED_STDERR_FILE [==[${golden_base}.stderr]==])
set(WORKING_DIRECTORY [==[${working_directory}]==])
include([==[${PROJECT_SOURCE_DIR}/tests/cli/run-cli-test.cmake]==])
")
    add_test(NAME xslt/${name} COMMAND "${CMAKE_COMMAND}" -P "${script}")
endfunction()

# xslt/slice: the XSLT slice run and checked; fails when fewer test cases
# pass than tests/xslt/slice-baseline.txt records.
set(slice_script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/slice.cmake")
set(slice_emulator_text "")
foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
    string(APPEND slice_emulator_text " [==[${emulator_part}]==]")
endforeach()
file(GENERATE OUTPUT "${slice_script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_xslt_driver>]==])
set(EMULATOR${slice_emulator_text})
set(WORKING_DIRECTORY [==[${PROJECT_SOURCE_DIR}]==])
set(SLICE_FILE tests/xslt/slice.txt)
set(BASELINE_FILE [==[${PROJECT_SOURCE_DIR}/tests/xslt/slice-baseline.txt]==])
include([==[${CMAKE_CURRENT_SOURCE_DIR}/run-slice-test.cmake]==])
")
add_test(NAME xslt/slice COMMAND "${CMAKE_COMMAND}" -P "${slice_script}")

# The small suite in input/: every kind of module, parameter, invocation,
# output, dependency, assertion and problem.
sxml_add_xslt_test(list_test_set EXIT_CODE 0
    ARGUMENTS list --catalog=catalog.xml --test-set=set-basic)
sxml_add_xslt_test(list_dependencies EXIT_CODE 0
    ARGUMENTS list --catalog=catalog.xml --test-set=set-dependencies)
sxml_add_xslt_test(list_problems EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml --test-set=set-problems)
sxml_add_xslt_test(list_test_list EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml list.txt)
sxml_add_xslt_test(list_unknown_test_set EXIT_CODE 1
    ARGUMENTS list --catalog=catalog.xml --test-set=no-such-set)
sxml_add_xslt_test(list_missing_catalog EXIT_CODE 3
    ARGUMENTS list --catalog=missing/catalog.xml)
sxml_add_xslt_test(list_both_selections EXIT_CODE 2
    ARGUMENTS list --catalog=catalog.xml --test-set=set-basic list.txt)
sxml_add_xslt_test(list_help EXIT_CODE 0 ARGUMENTS list --help)
sxml_add_xslt_test(run_test_set EXIT_CODE 0
    ARGUMENTS run --catalog=catalog.xml --test-set=set-basic)
sxml_add_xslt_test(run_json EXIT_CODE 0
    ARGUMENTS run --catalog=catalog.xml --format=json --test-set=set-dependencies)
sxml_add_xslt_test(run_problems EXIT_CODE 1
    ARGUMENTS run --catalog=catalog.xml --test-set=set-problems)
sxml_add_xslt_test(run_help EXIT_CODE 0 ARGUMENTS run --help)
sxml_add_xslt_test(program_help EXIT_CODE 0 ARGUMENTS --help)

# The slice against the vendored suite: every test case resolves, plans
# and is in scope.
sxml_add_xslt_test(list_slice EXIT_CODE 0 IN_SOURCE_ROOT
    ARGUMENTS list tests/xslt/slice.txt)
