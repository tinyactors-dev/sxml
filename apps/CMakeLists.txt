# The CLI framework and the sxml command-line program (docs/CLI.md, D44).
#
#   sxml_cli_framework  apps/cli_framework/: command tables, argument parsing,
#                       help, suggestions, output streams. Never touches stdio.
#   sxml_program        apps/sxml/ without main: the command table and the
#                       commands, linked by the executable and by tests.
#   sxml_cli            the executable, named "sxml" (main.c and the platform
#                       layer platform_stdio.c, platform_file_system.c and
#                       platform_file_writer.c).
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

# The CLDR data the sxml command serves to the UCA collation feature
# (D213, D220) and the localization feature (D221): the CLDR root and the
# languages of D208 from the vendored CLDR subset, generated into the build
# directory. collation/root.xml is left out: its own types (search, eor,
# emoji) use rules sxml does not support.
set(SXML_CLDR_DATA_DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/cldr/common")
set(SXML_CLDR_DATA_PATHS
    uca/allkeys_CLDR.txt
    collation/de.xml collation/de_AT.xml collation/en.xml collation/en_US.xml
    collation/en_US_POSIX.xml collation/es.xml collation/fr.xml collation/fr_CA.xml
    collation/it.xml collation/nl.xml
    main/root.xml main/de.xml main/de_AT.xml main/en.xml main/es.xml main/fr.xml
    main/it.xml main/nl.xml
    rbnf/de.xml rbnf/en.xml rbnf/es.xml rbnf/fr.xml rbnf/it.xml rbnf/nl.xml
    supplemental/supplementalData.xml supplemental/ordinals.xml)
set(SXML_CLDR_DATA_INPUTS "")
set(SXML_CLDR_DATA_FILES "")
foreach(cldr_path IN LISTS SXML_CLDR_DATA_PATHS)
    if(NOT SXML_CLDR_DATA_INPUTS STREQUAL "")
        string(APPEND SXML_CLDR_DATA_INPUTS "|")
    endif()
    string(APPEND SXML_CLDR_DATA_INPUTS "${cldr_path}=${SXML_CLDR_DATA_DIRECTORY}/${cldr_path}")
    list(APPEND SXML_CLDR_DATA_FILES "${SXML_CLDR_DATA_DIRECTORY}/${cldr_path}")
endforeach()
set(SXML_CLDR_DATA_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/sxml_cldr_data.c")
add_custom_command(
    OUTPUT "${SXML_CLDR_DATA_SOURCE}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUTS=${SXML_CLDR_DATA_INPUTS}"
        "-DOUTPUT=${SXML_CLDR_DATA_SOURCE}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/sxml/embed-cldr-data.cmake"
    DEPENDS ${SXML_CLDR_DATA_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/sxml/embed-cldr-data.cmake"
    COMMENT "Embedding the CLDR data for the sxml command"
    VERBATIM)

add_library(sxml_program STATIC
    "${SXML_CLDR_DATA_SOURCE}"
    sxml/command_check.c
    sxml/command_serialize.c
    sxml/command_transform.c
    sxml/command_validate.c
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
    sxml/platform_file_system.c
    sxml/platform_file_writer.c
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
