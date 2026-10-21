/*
 * The command table of the XSD 1.1 driver (xsd_driver.h). Imitates
 * tests/xslt/driver/program_table.c.
 */
#include "xsd_driver.h"

static const sxml_cli_command *const sxml_xsd_test_program_commands[] = {
    &sxml_xsd_test_list_command,
    &sxml_xsd_test_run_command,
};

static const sxml_cli_program sxml_xsd_test_program_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("sxml-xsd-test"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "the W3C XSD 1.1 test-suite driver of sxml (a developer tool)"),
    .commands = sxml_xsd_test_program_commands,
    .command_count = sizeof sxml_xsd_test_program_commands /
                     sizeof sxml_xsd_test_program_commands[0],
    .bug_report_location =
        SXML_BYTE_VIEW_INITIALIZER("https://github.com/tinyactors-dev/sxml/issues"),
};

const sxml_cli_program *sxml_xsd_test_program(void) {
    return &sxml_xsd_test_program_entry;
}
