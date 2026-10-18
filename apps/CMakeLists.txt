# The CLI framework and the sxml command-line program (docs/CLI.md, D44).
#
#   sxml_cli_framework  apps/cli_framework/: command tables, argument parsing,
#                       help, suggestions, output streams. Never touches stdio.
#   sxml_program        apps/sxml/ without main: the command table and the
#                       commands, linked by the executable and by tests.
#   sxml_cli            the executable, named "sxml" (main.c and the platform
#                       layer platform_stdio.c).
#
# Application code: it links only the public library interface (include/sxml/)
# and never sees src/.

add_library(sxml_cli_framework STATIC
    cli_framework/cli_arguments.c
    cli_framework/cli_command.c
    cli_framework/cli_help.c
    cli_framework/cli_output.c
    cli_framework/cli_run.c
    cli_framework/cli_suggestion.c
)
target_include_directories(sxml_cli_framework PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cli_framework")
target_link_libraries(sxml_cli_framework PUBLIC sxml)
sxml_apply_project_flags(sxml_cli_framework)

add_library(sxml_program STATIC
    sxml/command_check.c
    sxml/command_serialize.c
    sxml/command_transform.c
    sxml/command_version.c
    sxml/command_xpath.c
    sxml/command_xquery.c
    sxml/program.c
    sxml/query_command.c
)
target_include_directories(sxml_program PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sxml")
target_link_libraries(sxml_program PUBLIC sxml_cli_framework)
sxml_apply_project_flags(sxml_program)

add_executable(sxml_cli
    sxml/main.c
    sxml/platform_stdio.c
)
target_link_libraries(sxml_cli PRIVATE sxml_program)
set_target_properties(sxml_cli PROPERTIES OUTPUT_NAME sxml)
sxml_apply_project_flags(sxml_cli)

# Tests of the framework and of the executable live in tests/cli/ so that the
# lint rules for tests apply to them; they are registered from here because
# they need these targets and the test runner from tests/.
if(SXML_BUILD_TESTS)
    add_subdirectory("${PROJECT_SOURCE_DIR}/tests/cli" "${PROJECT_BINARY_DIR}/tests/cli")
endif()
