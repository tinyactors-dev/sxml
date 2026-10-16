#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cli_command.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Parsing a command line against a command table (docs/CLI.md, "Syntax").
 *
 * - Long options: "--name", "--name=value", "--name value". Names must match
 *   exactly; there are no abbreviations.
 * - Short options: "-q", combined "-qv", with a value "-o FILE" or "-oFILE".
 *   A value-taking option takes the rest of its argument, or the next
 *   argument, even when that starts with "-".
 * - "--" ends options; "-" alone and every argument not starting with "-"
 *   are positional. Options and positionals may be mixed, unless the
 *   configuration stops at the first positional (POSIX order, used for the
 *   options before a command name).
 * - "-h" and "--help" request help and win over every other argument before
 *   "--", including arguments that are in error.
 *
 * The result is an sxml_cli_arguments object: the options and positionals
 * found, which the command function queries.
 */

/*
 * The "-h, --help" option implied on every command: not part of any table,
 * listed first in every help text.
 */
const sxml_cli_option *sxml_cli_help_option(void);

typedef struct sxml_cli_arguments_configuration {
    /* The command whose table the arguments are parsed against. Borrowed:
       must outlive the arguments object. */
    const sxml_cli_command *command;
    /* The arguments after the command name. Borrowed: the views and the
       bytes they point to must outlive the arguments object. */
    sxml_cli_argument_list argument_list;
    /* True to treat every argument after the first positional as
       positional, as POSIX getopt does; false to allow options after
       positionals, as GNU getopt does. */
    bool stops_at_first_positional;
} sxml_cli_arguments_configuration;

/*
 * Writes the memory requirements of the arguments object for configuration;
 * they grow with the bytes in argument_list. Returns
 * SXML_STATUS_ARITHMETIC_OVERFLOW when they do not fit in size_t.
 */
SXML_NODISCARD sxml_status sxml_cli_arguments_memory_requirements(
    const sxml_cli_arguments_configuration *configuration,
    sxml_memory_requirements *requirements);

/*
 * Places an arguments object in memory and writes its address to
 * *arguments. Nothing is parsed until sxml_cli_arguments_parse. Returns the
 * failures of sxml_cli_arguments_memory_requirements and
 * sxml_memory_view_check_requirements.
 */
SXML_NODISCARD sxml_status sxml_cli_arguments_initialize(
    sxml_memory_view memory,
    const sxml_cli_arguments_configuration *configuration,
    sxml_cli_arguments **arguments);

/* Does nothing: an arguments object acquires nothing. Accepts NULL. */
void sxml_cli_arguments_finalize(sxml_cli_arguments *arguments);

/*
 * Allocates an arguments object through allocator, which is stored and must
 * outlive it, and initializes it.
 */
SXML_NODISCARD sxml_status sxml_cli_arguments_create(
    sxml_allocator allocator,
    const sxml_cli_arguments_configuration *configuration,
    sxml_cli_arguments **arguments);

/* Releases an arguments object made by create. Accepts NULL. */
void sxml_cli_arguments_destroy(sxml_cli_arguments *arguments);

/* ------------------------------------------------------------- Parsing */

/* What is wrong with a command line. */
typedef enum sxml_cli_usage_error_kind {
    /* No command name was given. */
    SXML_CLI_USAGE_ERROR_MISSING_COMMAND,
    /* The command name is not in the table. */
    SXML_CLI_USAGE_ERROR_UNKNOWN_COMMAND,
    /* An option is not in the table. */
    SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION,
    /* A value-taking option is the last argument. */
    SXML_CLI_USAGE_ERROR_MISSING_VALUE,
    /* A flag was given a value with "--flag=value". */
    SXML_CLI_USAGE_ERROR_UNEXPECTED_VALUE,
    /* A choice option's value is not one of its choices. */
    SXML_CLI_USAGE_ERROR_INVALID_CHOICE,
    /* An option that may be given at most once was given again. */
    SXML_CLI_USAGE_ERROR_REPEATED_OPTION,
    /* A required positional argument is missing. */
    SXML_CLI_USAGE_ERROR_MISSING_POSITIONAL,
    /* There are more positional arguments than the command takes. */
    SXML_CLI_USAGE_ERROR_UNEXPECTED_POSITIONAL,
} sxml_cli_usage_error_kind;

/*
 * A usage error. Every view is borrowed from the command line or the table.
 */
typedef struct sxml_cli_usage_error {
    sxml_cli_usage_error_kind kind;
    /* What the user typed that is wrong: the option name without dashes
       (UNKNOWN_OPTION), the value (INVALID_CHOICE, UNEXPECTED_VALUE), the
       argument (UNKNOWN_COMMAND, UNEXPECTED_POSITIONAL); empty otherwise. */
    sxml_byte_view argument;
    /* For UNKNOWN_OPTION: true when it was a short option ("-x"). */
    bool is_short_option;
    /* The option concerned, for MISSING_VALUE, UNEXPECTED_VALUE,
       INVALID_CHOICE and REPEATED_OPTION; NULL otherwise. */
    const sxml_cli_option *option;
    /* The positional concerned, for MISSING_POSITIONAL; NULL otherwise. */
    const sxml_cli_positional *positional;
    /* The closest valid name or value (without dashes); empty for none. */
    sxml_byte_view suggestion;
} sxml_cli_usage_error;

typedef enum sxml_cli_parse_outcome_kind {
    /* The arguments are valid and can be queried. */
    SXML_CLI_PARSE_OUTCOME_PARSED,
    /* "-h" or "--help" was given: show help, ignore everything else. */
    SXML_CLI_PARSE_OUTCOME_HELP_REQUESTED,
    /* The command line is wrong; usage_error says how. */
    SXML_CLI_PARSE_OUTCOME_USAGE_ERROR,
} sxml_cli_parse_outcome_kind;

typedef struct sxml_cli_parse_outcome {
    sxml_cli_parse_outcome_kind kind;
    /* Meaningful only for SXML_CLI_PARSE_OUTCOME_USAGE_ERROR. */
    sxml_cli_usage_error usage_error;
} sxml_cli_parse_outcome;

/*
 * Parses the configured argument list into arguments and writes the outcome
 * to *outcome. A usage error is an outcome, not a failure. Parsing again
 * starts over. Returns SXML_STATUS_OK; there is no failure, since the object
 * was sized for its argument list.
 */
SXML_NODISCARD sxml_status sxml_cli_arguments_parse(
    sxml_cli_arguments *arguments, sxml_cli_parse_outcome *outcome);

/* -------------------------------------------------------------- Queries */

/* Which option, and which of its occurrences, to look up. */
typedef struct sxml_cli_option_lookup {
    /* The option's long name without dashes, such as "format". */
    sxml_byte_view long_name;
    /* Which occurrence, from 0; for an option given at most once, 0. */
    size_t occurrence_index;
} sxml_cli_option_lookup;

typedef struct sxml_cli_option_match {
    /* True when the option was given at least occurrence_index + 1 times. */
    bool is_present;
    /* How often the option was given. */
    size_t occurrence_count;
    /* The value of that occurrence; when it is absent, the option's
       default value (empty when it has none). Empty for a flag. Borrowed
       from the command line or the table. */
    sxml_byte_view value;
} sxml_cli_option_match;

/*
 * Looks up an option of the parsed arguments and writes what was found to
 * *match. Returns SXML_STATUS_INVALID_ARGUMENT, leaving *match untouched,
 * when the command declares no option with that long name: a mistyped name
 * in a command function fails loudly instead of reading as "absent".
 */
SXML_NODISCARD sxml_status sxml_cli_arguments_find_option(
    const sxml_cli_arguments *arguments, sxml_cli_option_lookup lookup,
    sxml_cli_option_match *match);

/* The number of positional arguments found. */
size_t sxml_cli_arguments_positional_count(const sxml_cli_arguments *arguments);

/*
 * Writes the positional argument at positional_index (from 0, in command
 * line order) to *positional. Returns SXML_STATUS_INDEX_OUT_OF_RANGE,
 * leaving *positional untouched, when there is none.
 */
SXML_NODISCARD sxml_status sxml_cli_arguments_get_positional(
    const sxml_cli_arguments *arguments, size_t positional_index,
    sxml_byte_view *positional);
