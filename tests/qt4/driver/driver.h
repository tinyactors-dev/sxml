#pragma once

#include "cli_command.h"

/*
 * The QT4 test-suite driver program, sxml-qt4 (glossary: qt4_driver; D44,
 * docs/CLI.md "The QT4 and XSLT 4.0 conformance drivers"). A developer tool
 * built on the CLI framework, never shipped. A command is one
 * command_<name>.c file plus one entry in the table in program_table.c, as
 * in apps/sxml.
 */

/* The table of the driver. Static data; valid for the whole run. */
const sxml_cli_program *sxml_qt4_program(void);

/* "list": the selected test cases with their resolved environments
   (command_list.c). */
extern const sxml_cli_command sxml_qt4_list_command;

/* "run": the selected test cases run and checked, with a report
   (command_run.c). */
extern const sxml_cli_command sxml_qt4_run_command;
