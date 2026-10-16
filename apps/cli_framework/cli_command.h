#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cli_output.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Declarative command tables for command-line programs (docs/CLI.md, D44).
 *
 * A program is a static const table: its name, a summary, and its commands.
 * Each command lists its options, its positional arguments, examples and the
 * function that runs it. The framework parses the command line against the
 * table (cli_arguments.h), writes help and usage errors from it (cli_help.h)
 * and dispatches to the command (cli_run.h). Help and usage text are
 * generated from the same table that drives parsing, so they cannot
 * disagree with what the program does.
 *
 * Every text in a table is ASCII or UTF-8 in a byte view with static storage
 * duration. "-h"/"--help" is implied on every command and the "help" command
 * is built in; a table must not declare either (sxml_cli_program_validate).
 */

/* ------------------------------------------------------------ Exit codes */

/* The process exit status of a command-line program (docs/CLI.md). */
typedef enum sxml_cli_exit_code {
    /* The operation ran and its answer is positive. */
    SXML_CLI_EXIT_SUCCESS = 0,
    /* The operation ran and failed on its input: a document is not
       well-formed, a query raised an error. */
    SXML_CLI_EXIT_OPERATION_FAILED = 1,
    /* The command line is wrong: unknown command or option, missing or
       extra argument, bad option value. */
    SXML_CLI_EXIT_USAGE_ERROR = 2,
    /* A file could not be read or written, or a write to standard output
       or standard error failed. */
    SXML_CLI_EXIT_INPUT_OUTPUT_ERROR = 3,
    /* Out of memory, a limit was exceeded, or a bug. */
    SXML_CLI_EXIT_INTERNAL_ERROR = 4,
} sxml_cli_exit_code;

/* --------------------------------------------------------------- Tables */

/* A read-only array of texts. */
typedef struct sxml_cli_text_list {
    const sxml_byte_view *texts;
    size_t text_count;
} sxml_cli_text_list;

/* The command-line arguments after the program name, as byte views. */
typedef struct sxml_cli_argument_list {
    const sxml_byte_view *arguments;
    size_t argument_count;
} sxml_cli_argument_list;

/* What follows an option on the command line. */
typedef enum sxml_cli_value_kind {
    /* Nothing: the option is a flag. */
    SXML_CLI_VALUE_KIND_NONE,
    /* Any text, the empty text included. */
    SXML_CLI_VALUE_KIND_TEXT,
    /* One of the texts in the option's choices, exactly. */
    SXML_CLI_VALUE_KIND_CHOICE,
} sxml_cli_value_kind;

/* How often an option may be given. */
typedef enum sxml_cli_option_multiplicity {
    /* Zero times or once; giving it twice is a usage error. */
    SXML_CLI_OPTION_AT_MOST_ONCE,
    /* Any number of times; every value is kept, in order. */
    SXML_CLI_OPTION_REPEATABLE,
} sxml_cli_option_multiplicity;

typedef struct sxml_cli_option {
    /* Name without the leading "--", such as "format". Required. */
    sxml_byte_view long_name;
    /* ASCII letter or digit used as "-x", or 0 for none. */
    uint8_t short_name;
    sxml_cli_value_kind value_kind;
    /* Placeholder shown in help, such as "FORMAT"; empty for a flag. */
    sxml_byte_view value_name;
    /* The allowed values of a SXML_CLI_VALUE_KIND_CHOICE option; empty
       for other kinds. */
    sxml_cli_text_list choices;
    /* The value used when the option is absent, shown in help; empty for
       none. Must be one of choices for a choice option; empty for a flag. */
    sxml_byte_view default_value;
    sxml_cli_option_multiplicity multiplicity;
    /* One line of help, without a final period. */
    sxml_byte_view summary;
} sxml_cli_option;

/* How many arguments one positional argument declaration takes. */
typedef enum sxml_cli_positional_multiplicity {
    SXML_CLI_POSITIONAL_EXACTLY_ONE,
    SXML_CLI_POSITIONAL_OPTIONAL,
    SXML_CLI_POSITIONAL_ONE_OR_MORE,
    SXML_CLI_POSITIONAL_ZERO_OR_MORE,
} sxml_cli_positional_multiplicity;

/*
 * A positional argument. Required ones (exactly one) come first; at most one
 * that is optional or takes several, and it is the last.
 */
typedef struct sxml_cli_positional {
    /* Placeholder, such as "FILE". */
    sxml_byte_view name;
    sxml_cli_positional_multiplicity multiplicity;
    /* One line of help, without a final period. */
    sxml_byte_view summary;
} sxml_cli_positional;

typedef struct sxml_cli_invocation sxml_cli_invocation;

/*
 * Runs a command whose arguments have been parsed. Returns the exit code.
 * The function writes its results to invocation->standard_output and its
 * diagnostics to invocation->standard_error.
 */
typedef sxml_cli_exit_code (*sxml_cli_command_function)(
    const sxml_cli_invocation *invocation);

typedef struct sxml_cli_command {
    /* The verb typed after the program name, such as "version". */
    sxml_byte_view name;
    /* One line for the command list, without a final period. */
    sxml_byte_view summary;
    /* Paragraphs for the command's help, lines separated by "\n", without
       a final line feed; empty for none. */
    sxml_byte_view description;
    const sxml_cli_option *options;
    size_t option_count;
    const sxml_cli_positional *positionals;
    size_t positional_count;
    /* Complete command lines, such as "sxml version --format=json". */
    sxml_cli_text_list examples;
    sxml_cli_command_function run;
} sxml_cli_command;

typedef struct sxml_cli_program {
    /* The program name used in usage lines and diagnostics, such as "sxml";
       never taken from argv[0], so output does not depend on how the
       program was started. */
    sxml_byte_view name;
    /* One line for the top of the help, without a final period. */
    sxml_byte_view summary;
    /* The commands in the order help lists them, after the built-in
       "help". Pointers, so each command can be defined in its own file. */
    const sxml_cli_command *const *commands;
    size_t command_count;
    /* Where to report bugs, shown at the end of the help; empty for none. */
    sxml_byte_view bug_report_location;
} sxml_cli_program;

/* ---------------------------------------------------------- File reader */

/* One file to read. */
typedef struct sxml_cli_file_read_request {
    /* The file name as given on the command line; "-" is standard input.
       Borrowed for the call. */
    sxml_byte_view path;
    /* Provides the memory of the contents. */
    sxml_allocator allocator;
} sxml_cli_file_read_request;

/*
 * The interface through which commands read files (glossary: file_reader;
 * D44: only the platform layer touches stdio). The default implementation
 * is the platform layer's (apps/sxml/platform.h).
 */
typedef struct sxml_cli_file_reader_operations {
    /* Reads the whole file into memory from request->allocator and writes
       it to *contents; the caller releases contents->bytes through that
       allocator (empty contents may have NULL bytes). Returns
       SXML_STATUS_INPUT_OUTPUT_FAILED when the file cannot be opened or
       read, and the allocator's failures. */
    sxml_status (*read_file)(void *state,
                             const sxml_cli_file_read_request *request,
                             sxml_byte_view *contents);
} sxml_cli_file_reader_operations;

/* Passed by value; operations NULL means no file can be read. */
typedef struct sxml_cli_file_reader {
    void *state;
    const sxml_cli_file_reader_operations *operations;
} sxml_cli_file_reader;

/*
 * Reads a file through reader. Returns SXML_STATUS_INPUT_OUTPUT_FAILED for
 * a reader without operations, and the reader's failures; *contents is
 * untouched on failure.
 */
SXML_NODISCARD sxml_status sxml_cli_file_reader_read(
    sxml_cli_file_reader reader, const sxml_cli_file_read_request *request,
    sxml_byte_view *contents);

/* ----------------------------------------------------------- Invocation */

typedef struct sxml_cli_arguments sxml_cli_arguments;

/* What a command function receives. Every pointer is borrowed for the call. */
struct sxml_cli_invocation {
    const sxml_cli_program *program;
    const sxml_cli_command *command;
    /* The parsed arguments of command (cli_arguments.h). */
    const sxml_cli_arguments *arguments;
    /* Results. A failed write makes the program exit with
       SXML_CLI_EXIT_INPUT_OUTPUT_ERROR unless the command reports a usage
       or internal error. */
    sxml_cli_writer *standard_output;
    /* Diagnostics. */
    sxml_cli_writer *standard_error;
    /* For memory the command needs; outlives the call. */
    sxml_allocator allocator;
    /* The version text the program reports, such as "0.0.0". */
    sxml_byte_view version_text;
    /* Reads the files named on the command line. */
    sxml_cli_file_reader file_reader;
};

/*
 * Writes a usage error found by the command itself, such as two arguments
 * that exclude each other, to standard error:
 *
 *     sxml xquery: give QUERY-FILE or --expression, not both
 *     Try 'sxml xquery --help'.
 *
 * description is one line without a final period. Returns
 * SXML_CLI_EXIT_USAGE_ERROR for the command to return.
 */
sxml_cli_exit_code sxml_cli_invocation_report_usage_error(
    const sxml_cli_invocation *invocation, sxml_byte_view description);

/* ----------------------------------------------------------- Validation */

/* What is wrong with a table, for the test that checks it. */
typedef struct sxml_cli_table_problem {
    /* One line, static text. */
    sxml_byte_view description;
    /* The command concerned; empty for the program itself. */
    sxml_byte_view command_name;
    /* The option or positional concerned; empty for none. */
    sxml_byte_view item_name;
} sxml_cli_table_problem;

/*
 * Checks the rules stated on the table types above: names are not empty and
 * unique, no command is named "help" and no option "help" or "-h", short
 * names are ASCII letters or digits, flags have no value name, choices and
 * defaults agree with the value kind, positionals are ordered as described,
 * and every command has a function.
 *
 * Returns SXML_STATUS_OK, or SXML_STATUS_INVALID_ARGUMENT and fills
 * *problem for the first rule broken.
 */
SXML_NODISCARD sxml_status sxml_cli_program_validate(
    const sxml_cli_program *program, sxml_cli_table_problem *problem);

/*
 * Finds the command called name in program, or the built-in help command
 * for "help". Writes it to *command and returns true; returns false and
 * leaves *command untouched when there is none.
 */
bool sxml_cli_program_find_command(const sxml_cli_program *program,
                                   sxml_byte_view name,
                                   const sxml_cli_command **command);

/* The built-in "help [COMMAND]" command (cli_run.c). */
const sxml_cli_command *sxml_cli_help_command(void);

/*
 * The arguments before the command name, described as a command named after
 * nothing (cli_run.c): the option "--version" and the positionals
 * "COMMAND [ARGUMENT...]". Parsed in POSIX order, so the first positional
 * ends sxml's own options.
 */
const sxml_cli_command *sxml_cli_root_command(void);
