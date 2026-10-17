/*
 * The command table of the XSLT 4.0 driver (xslt_driver.h). Imitates
 * tests/qt4/driver/program_table.c.
 */
#include "xslt_driver.h"

static const sxml_cli_command *const sxml_xslt_test_program_commands[] = {
    &sxml_xslt_test_list_command,
    &sxml_xslt_test_run_command,
};

static const sxml_cli_program sxml_xslt_test_program_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("sxml-xslt-test"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "the W3C XSLT 4.0 test-suite driver of sxml (a developer tool)"),
    .commands = sxml_xslt_test_program_commands,
    .command_count = sizeof sxml_xslt_test_program_commands /
                     sizeof sxml_xslt_test_program_commands[0],
    .bug_report_location =
        SXML_BYTE_VIEW_INITIALIZER("https://github.com/dhamidi/sxml/issues"),
};

const sxml_cli_program *sxml_xslt_test_program(void) {
    return &sxml_xslt_test_program_entry;
}
