# Tests of the CLI framework and the sxml program (docs/CLI.md, D44), added
# by apps/CMakeLists.txt.

# Unit tests: tests/cli/<module>_test.c, one executable each, registered as
# ctest test <module> (as sxml_add_test does for the library, D16).
foreach(cli_module cli_output cli_suggestion cli_arguments cli_run)
    add_executable(${cli_module}_test "${cli_module}_test.c")
    target_link_libraries(${cli_module}_test PRIVATE sxml_program sxml_test_runner)
    sxml_apply_project_flags(${cli_module}_test)
    add_test(NAME ${cli_module} COMMAND ${cli_module}_test)
endforeach()

# End-to-end tests of the sxml executable. Each test runs
# `sxml ARGUMENT...` and compares standard output, standard error and the exit
# code with tests/cli/expected/<golden>.stdout and .stderr (a missing file
# means "nothing written"). Regenerate goldens after an intended change with
#   SXML_UPDATE_GOLDEN=1 ctest --preset debug -R '^cli/'
# and review the diff.

# sxml_add_cli_test(<name> EXIT_CODE <code> [GOLDEN <golden>] [ARGUMENTS <argument>...])
# Arguments are passed verbatim; empty arguments are not supported here (the
# unit tests in tests/cli_arguments_test.c cover them).
function(sxml_add_cli_test name)
    cmake_parse_arguments(PARSE_ARGV 1 cli "" "EXIT_CODE;GOLDEN" "ARGUMENTS")
    if(NOT DEFINED cli_EXIT_CODE)
        message(FATAL_ERROR "sxml_add_cli_test(${name}): EXIT_CODE is required")
    endif()
    if(NOT cli_GOLDEN)
        set(cli_GOLDEN "${name}")
    endif()
    set(argument_text "")
    foreach(argument IN LISTS cli_ARGUMENTS)
        string(APPEND argument_text " [==[${argument}]==]")
    endforeach()
    set(emulator_text "")
    foreach(emulator_part IN LISTS CMAKE_CROSSCOMPILING_EMULATOR)
        string(APPEND emulator_text " [==[${emulator_part}]==]")
    endforeach()
    set(golden_base "${CMAKE_CURRENT_SOURCE_DIR}/expected/${cli_GOLDEN}")
    set(script "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${name}.cmake")
    file(GENERATE OUTPUT "${script}" CONTENT
"set(PROGRAM [==[$<TARGET_FILE:sxml_cli>]==])
set(EMULATOR${emulator_text})
set(ARGUMENTS${argument_text})
set(EXPECTED_EXIT_CODE ${cli_EXIT_CODE})
set(EXPECTED_STDOUT_FILE [==[${golden_base}.stdout]==])
set(EXPECTED_STDERR_FILE [==[${golden_base}.stderr]==])
include([==[${CMAKE_CURRENT_SOURCE_DIR}/run-cli-test.cmake]==])
")
    add_test(NAME cli/${name} COMMAND "${CMAKE_COMMAND}" -P "${script}")
endfunction()

# Help on every level (docs/CLI.md, "Syntax").
sxml_add_cli_test(help_long_option EXIT_CODE 0 GOLDEN program_help ARGUMENTS --help)
sxml_add_cli_test(help_short_option EXIT_CODE 0 GOLDEN program_help ARGUMENTS -h)
sxml_add_cli_test(help_command EXIT_CODE 0 GOLDEN program_help ARGUMENTS help)
sxml_add_cli_test(help_wins_over_rest EXIT_CODE 0 GOLDEN program_help ARGUMENTS --help --bogus)
sxml_add_cli_test(help_for_version EXIT_CODE 0 GOLDEN version_help ARGUMENTS help version)
sxml_add_cli_test(version_help_option EXIT_CODE 0 GOLDEN version_help ARGUMENTS version --help)
sxml_add_cli_test(version_help_after_error EXIT_CODE 0 GOLDEN version_help ARGUMENTS version --bogus -h)
sxml_add_cli_test(help_for_help EXIT_CODE 0 ARGUMENTS help help)

# The version, three ways.
sxml_add_cli_test(version_option EXIT_CODE 0 GOLDEN version_text ARGUMENTS --version)
sxml_add_cli_test(version_command EXIT_CODE 0 GOLDEN version_text ARGUMENTS version)
sxml_add_cli_test(version_format_text EXIT_CODE 0 GOLDEN version_text ARGUMENTS version --format=text)
sxml_add_cli_test(version_format_json EXIT_CODE 0 GOLDEN version_json ARGUMENTS version --format=json)
sxml_add_cli_test(version_format_separate_value EXIT_CODE 0 GOLDEN version_json ARGUMENTS version --format json)
sxml_add_cli_test(version_after_double_dash EXIT_CODE 0 GOLDEN version_text ARGUMENTS -- version)

# Usage errors: exit code 2, diagnostics on standard error only.
sxml_add_cli_test(no_arguments EXIT_CODE 2)
sxml_add_cli_test(unknown_command EXIT_CODE 2 ARGUMENTS verison)
sxml_add_cli_test(unknown_command_without_suggestion EXIT_CODE 2 ARGUMENTS transmogrify)
sxml_add_cli_test(unknown_program_option EXIT_CODE 2 ARGUMENTS --verison)
sxml_add_cli_test(version_option_with_value EXIT_CODE 2 ARGUMENTS --version=1)
sxml_add_cli_test(version_option_with_command EXIT_CODE 2 ARGUMENTS --version version)
sxml_add_cli_test(unknown_option EXIT_CODE 2 ARGUMENTS version --fromat=json)
sxml_add_cli_test(unknown_short_option EXIT_CODE 2 ARGUMENTS version -x)
sxml_add_cli_test(abbreviated_option EXIT_CODE 2 ARGUMENTS version --form=json)
sxml_add_cli_test(invalid_choice EXIT_CODE 2 ARGUMENTS version --format=JSON)
sxml_add_cli_test(missing_value EXIT_CODE 2 ARGUMENTS version --format)
sxml_add_cli_test(repeated_option EXIT_CODE 2 ARGUMENTS version --format=json --format=text)
sxml_add_cli_test(unexpected_argument EXIT_CODE 2 ARGUMENTS version extra)
sxml_add_cli_test(standard_input_is_an_argument EXIT_CODE 2 ARGUMENTS version -)
sxml_add_cli_test(help_unknown_command EXIT_CODE 2 ARGUMENTS help verison)
sxml_add_cli_test(help_extra_argument EXIT_CODE 2 ARGUMENTS help version extra)
