/*
 * The command table of the sxml program (docs/CLI.md). Only commands that
 * work are listed: planned commands are documented in docs/CLI.md and added
 * here together with their code.
 */
#include "program.h"

static const sxml_cli_command *const sxml_program_commands[] = {
    &sxml_version_command,
};

static const sxml_cli_program sxml_program_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("sxml"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "command-line interface to the sxml XML library"),
    .commands = sxml_program_commands,
    .command_count =
        sizeof sxml_program_commands / sizeof sxml_program_commands[0],
    .bug_report_location =
        SXML_BYTE_VIEW_INITIALIZER("https://github.com/dhamidi/sxml/issues"),
};

const sxml_cli_program *sxml_program(void) { return &sxml_program_entry; }
