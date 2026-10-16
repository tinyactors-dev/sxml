#pragma once

#include "cli_command.h"

/*
 * The sxml program: its command table and command functions (docs/CLI.md).
 *
 * A command is one command_<name>.c file holding its function and its
 * options, plus one entry in the table in program.c. Command functions use
 * only the public sxml API and the CLI framework; they never touch stdio.
 */

/* The table of the sxml program. Static data; valid for the whole run. */
const sxml_cli_program *sxml_program(void);

/* The "check" command (command_check.c). */
extern const sxml_cli_command sxml_check_command;

/* The "version" command (command_version.c). */
extern const sxml_cli_command sxml_version_command;
