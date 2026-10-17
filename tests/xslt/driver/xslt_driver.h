#pragma once

#include "cli_command.h"

/*
 * The XSLT 4.0 test-suite driver program, sxml-xslt-test (glossary:
 * xslt_test_driver; D44, D117, docs/CLI.md "The QT4 and XSLT 4.0
 * conformance drivers"). A developer tool built on the CLI framework,
 * never shipped, a sibling of sxml-qt4 whose library it links for
 * documents, environments, test lists, reasons and reports. A command is
 * one command_<name>.c file plus one entry in program_table.c.
 */

/* The table of the driver. Static data; valid for the whole run. */
const sxml_cli_program *sxml_xslt_test_program(void);

/* "list": the selected test cases with their environments, invocations,
   plans and applicability (command_list.c). */
extern const sxml_cli_command sxml_xslt_test_list_command;

/* "run": the selected test cases run and reported (command_run.c). */
extern const sxml_cli_command sxml_xslt_test_run_command;
