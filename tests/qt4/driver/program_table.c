/*
 * The command table of the QT4 driver (driver.h). Imitates
 * apps/sxml/program.c: only commands that work are listed; packet 1.25
 * adds "run".
 */
#include "driver.h"

static const sxml_cli_command *const sxml_qt4_program_commands[] = {
    &sxml_qt4_list_command,
};

static const sxml_cli_program sxml_qt4_program_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("sxml-qt4"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "the W3C QT4 test-suite driver of sxml (a developer tool)"),
    .commands = sxml_qt4_program_commands,
    .command_count =
        sizeof sxml_qt4_program_commands / sizeof sxml_qt4_program_commands[0],
    .bug_report_location =
        SXML_BYTE_VIEW_INITIALIZER("https://github.com/dhamidi/sxml/issues"),
};

const sxml_cli_program *sxml_qt4_program(void) {
    return &sxml_qt4_program_entry;
}
