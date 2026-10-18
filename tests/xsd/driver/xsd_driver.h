#pragma once

#include "cli_command.h"

/*
 * The XSD 1.1 test-suite driver program, sxml-xsd-test (glossary:
 * xsd_test_driver; D44, D165, D168, docs/CLI.md "The QT4 and XSLT 4.0
 * conformance drivers"). A developer tool built on the CLI framework,
 * never shipped, a sibling of sxml-qt4 and sxml-xslt-test whose library
 * (tests/qt4/driver) it links for documents, test lists, the test
 * selection, the test budget, reasons and reports. A command is one
 * command_<name>.c file plus one entry in program_table.c.
 */

/* The table of the driver. Static data; valid for the whole run. */
const sxml_cli_program *sxml_xsd_test_program(void);

/* "list": the selected tests with their documents, expected outcomes and
   applicability (command_list.c). */
extern const sxml_cli_command sxml_xsd_test_list_command;

/* "run": the selected tests run and reported (command_run.c). */
extern const sxml_cli_command sxml_xsd_test_run_command;
