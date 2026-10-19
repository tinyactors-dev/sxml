#pragma once

#include "cli_arguments.h"
#include "cli_command.h"
#include "cli_output.h"

/*
 * Help texts, usage lines and usage errors, generated from the command table
 * (docs/CLI.md, "Output"). Plain text without terminal escapes, lines ending
 * in "\n". Failures to write are recorded in the writer.
 */

/* What help is written for: the program, or one of its commands. */
typedef struct sxml_cli_help_request {
    const sxml_cli_program *program;
    /* NULL for the program's own help. */
    const sxml_cli_command *command;
} sxml_cli_help_request;

/* Writes the full help of the program or the command. */
void sxml_cli_write_help(sxml_cli_writer *writer,
                         sxml_cli_help_request request);

/*
 * Writes "Usage: sxml version [--format=FORMAT]" and a line feed. The
 * implied "--help" is left out; an option that may be repeated is followed
 * by "...".
 */
void sxml_cli_write_usage_line(sxml_cli_writer *writer,
                               sxml_cli_help_request request);

/* A usage error and where it was found. */
typedef struct sxml_cli_usage_error_report {
    const sxml_cli_program *program;
    /* The command whose arguments are wrong; NULL for the arguments before
       the command name. */
    const sxml_cli_command *command;
    sxml_cli_usage_error usage_error;
} sxml_cli_usage_error_report;

/*
 * Writes a usage error as three lines:
 *
 *     sxml version: unknown option '--fromat'; did you mean '--format'?
 *     Usage: sxml version [--format=FORMAT]
 *     Try 'sxml version --help'.
 *
 * An unknown long option of a command that is not close to one of its
 * options, but is an option of other commands, names them instead of a
 * suggestion: "sxml xpath: unknown option '--expression'; it is an option
 * of 'sxml xquery'".
 */
void sxml_cli_write_usage_error(sxml_cli_writer *writer,
                                const sxml_cli_usage_error_report *report);

/*
 * Writes the prefix of a diagnostic line: "sxml: " for the program, or
 * "sxml version: " for a command.
 */
void sxml_cli_write_diagnostic_prefix(sxml_cli_writer *writer,
                                      sxml_cli_help_request request);

/* Writes "Try 'sxml version --help'." or "Try 'sxml --help'." and a line feed. */
void sxml_cli_write_help_hint(sxml_cli_writer *writer,
                              sxml_cli_help_request request);
