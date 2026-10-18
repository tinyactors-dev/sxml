/*
 * Command-line parsing against a command table (cli_arguments.h). Imitates
 * the golden module src/memory.c for the lifecycle functions.
 *
 * Syntax follows POSIX.1-2024 XBD 12.2 "Utility Syntax Guidelines" for short
 * options and "--", and the GNU coding standards for long options and
 * argument permutation, without GNU's abbreviations (docs/CLI.md).
 */
#include "cli_arguments.h"

#include <stdalign.h>
#include <string.h>

#include "cli_suggestion.h"

typedef enum sxml_cli_entry_kind {
    SXML_CLI_ENTRY_KIND_OPTION,
    SXML_CLI_ENTRY_KIND_POSITIONAL,
} sxml_cli_entry_kind;

/* One option occurrence or positional argument, in command line order. */
typedef struct sxml_cli_entry {
    sxml_cli_entry_kind kind;
    /* Index into command->options; 0 for a positional. */
    size_t option_index;
    /* The option's value (empty for a flag) or the positional argument. */
    sxml_byte_view value;
} sxml_cli_entry;

struct sxml_cli_arguments {
    const sxml_cli_command *command;
    sxml_cli_argument_list argument_list;
    bool stops_at_first_positional;
    /* entry_capacity entries placed after this struct. */
    sxml_cli_entry *entries;
    size_t entry_capacity;
    size_t entry_count;
    /* The allocator create used; the null allocator after initialize. */
    sxml_allocator owning_allocator;
};

static const sxml_cli_option sxml_cli_help_option_entry = {
    .long_name = SXML_BYTE_VIEW_INITIALIZER("help"),
    .short_name = 'h',
    .value_kind = SXML_CLI_VALUE_KIND_NONE,
    .value_name = SXML_BYTE_VIEW_INITIALIZER(""),
    .choices = {.texts = NULL, .text_count = 0},
    .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
    .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
    .summary = SXML_BYTE_VIEW_INITIALIZER("Show this help and exit"),
};

const sxml_cli_option *sxml_cli_help_option(void) {
    return &sxml_cli_help_option_entry;
}

/* ------------------------------------------------------------ Lifecycle */

/*
 * Each argument yields at most one entry per byte after its leading "-"
 * (a cluster "-abc" gives three), and at least one; the sum of
 * max(1, byte_count) bounds the entries.
 */
static sxml_status sxml_cli_entry_capacity(sxml_cli_argument_list argument_list,
                                           size_t *entry_capacity) {
    size_t capacity = 0;
    for (size_t argument_index = 0;
         argument_index < argument_list.argument_count; argument_index += 1) {
        const size_t byte_count =
            argument_list.arguments[argument_index].byte_count;
        const sxml_status status =
            sxml_size_add(capacity, byte_count > 0 ? byte_count : 1, &capacity);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    *entry_capacity = capacity;
    return SXML_STATUS_OK;
}

/* The layout of an arguments object: the struct, then the entries. */
typedef struct sxml_cli_arguments_layout {
    sxml_memory_requirements requirements;
    size_t entries_offset;
    size_t entry_capacity;
} sxml_cli_arguments_layout;

static sxml_status sxml_cli_arguments_compute_layout(
    const sxml_cli_arguments_configuration *configuration,
    sxml_cli_arguments_layout *layout) {
    size_t entry_capacity = 0;
    sxml_status status =
        sxml_cli_entry_capacity(configuration->argument_list, &entry_capacity);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t entries_byte_count = 0;
    status = sxml_size_multiply(entry_capacity, sizeof(sxml_cli_entry),
                                &entries_byte_count);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    size_t header_offset = 0;
    status = sxml_memory_requirements_append(
        &requirements,
        (sxml_memory_requirements){
            .byte_count = sizeof(struct sxml_cli_arguments),
            .alignment = alignof(struct sxml_cli_arguments),
        },
        &header_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    size_t entries_offset = 0;
    status = sxml_memory_requirements_append(
        &requirements,
        (sxml_memory_requirements){
            .byte_count = entries_byte_count,
            .alignment = alignof(sxml_cli_entry),
        },
        &entries_offset);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *layout = (sxml_cli_arguments_layout){
        .requirements = requirements,
        .entries_offset = entries_offset,
        .entry_capacity = entry_capacity,
    };
    return SXML_STATUS_OK;
}

sxml_status sxml_cli_arguments_memory_requirements(
    const sxml_cli_arguments_configuration *configuration,
    sxml_memory_requirements *requirements) {
    sxml_cli_arguments_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .entries_offset = 0,
        .entry_capacity = 0,
    };
    const sxml_status status =
        sxml_cli_arguments_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    *requirements = layout.requirements;
    return SXML_STATUS_OK;
}

sxml_status sxml_cli_arguments_initialize(
    sxml_memory_view memory,
    const sxml_cli_arguments_configuration *configuration,
    sxml_cli_arguments **arguments) {
    sxml_cli_arguments_layout layout = {
        .requirements = {.byte_count = 0, .alignment = 1},
        .entries_offset = 0,
        .entry_capacity = 0,
    };
    sxml_status status =
        sxml_cli_arguments_compute_layout(configuration, &layout);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    status = sxml_memory_view_check_requirements(memory, layout.requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    /* memory_view_check_requirements has checked size and alignment, and
       the layout aligned the entries. */
    void *placement = memory.bytes;
    void *entries_placement = memory.bytes + layout.entries_offset;
    sxml_cli_arguments *new_arguments = placement;
    *new_arguments = (sxml_cli_arguments){
        .command = configuration->command,
        .argument_list = configuration->argument_list,
        .stops_at_first_positional = configuration->stops_at_first_positional,
        .entries = entries_placement,
        .entry_capacity = layout.entry_capacity,
        .entry_count = 0,
        .owning_allocator = {.state = NULL, .operations = NULL},
    };
    *arguments = new_arguments;
    return SXML_STATUS_OK;
}

void sxml_cli_arguments_finalize(sxml_cli_arguments *arguments) {
    /* The entries are part of the object's own memory. */
    (void)arguments;
}

sxml_status sxml_cli_arguments_create(
    sxml_allocator allocator,
    const sxml_cli_arguments_configuration *configuration,
    sxml_cli_arguments **arguments) {
    sxml_memory_requirements requirements = {.byte_count = 0, .alignment = 1};
    sxml_status status =
        sxml_cli_arguments_memory_requirements(configuration, &requirements);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    void *memory = NULL;
    status = sxml_allocator_allocate(allocator,
                                     (sxml_allocation_request){
                                         .byte_count = requirements.byte_count,
                                         .alignment = requirements.alignment,
                                     },
                                     &memory);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    sxml_cli_arguments *new_arguments = NULL;
    status = sxml_cli_arguments_initialize(
        (sxml_memory_view){
            .bytes = memory,
            .byte_count = requirements.byte_count,
        },
        configuration, &new_arguments);
    if (status != SXML_STATUS_OK) {
        sxml_allocator_release(allocator, memory);
        return status;
    }
    new_arguments->owning_allocator = allocator;
    *arguments = new_arguments;
    return SXML_STATUS_OK;
}

void sxml_cli_arguments_destroy(sxml_cli_arguments *arguments) {
    if (arguments == NULL) {
        return;
    }
    const sxml_allocator owning_allocator = arguments->owning_allocator;
    sxml_cli_arguments_finalize(arguments);
    sxml_allocator_release(owning_allocator, arguments);
}

/* -------------------------------------------------------------- Parsing */

/* The state of one parse. */
typedef struct sxml_cli_parser {
    sxml_cli_arguments *arguments;
    /* The argument being parsed; parse steps that take a value from the
       next argument advance it. */
    size_t argument_index;
    bool options_ended;
    bool has_usage_error;
    /* The first usage error; later ones are not reported. */
    sxml_cli_usage_error usage_error;
} sxml_cli_parser;

/* Whether a parse step found a help request. */
typedef enum sxml_cli_parse_step {
    SXML_CLI_PARSE_STEP_CONTINUE,
    SXML_CLI_PARSE_STEP_HELP_REQUESTED,
} sxml_cli_parse_step;

static const sxml_byte_view sxml_empty_view = {.bytes = NULL, .byte_count = 0};

static sxml_byte_view sxml_view_suffix(sxml_byte_view text,
                                       size_t byte_offset) {
    if (byte_offset >= text.byte_count) {
        return sxml_empty_view;
    }
    return (sxml_byte_view){
        .bytes = text.bytes + byte_offset,
        .byte_count = text.byte_count - byte_offset,
    };
}

static bool sxml_is_positional_token(sxml_byte_view argument) {
    /* "-" alone means standard input: an operand (POSIX guideline 13). */
    return argument.byte_count < 2 || argument.bytes[0] != '-';
}

static bool sxml_is_help_token(sxml_byte_view argument) {
    return sxml_byte_view_equals(argument, SXML_BYTE_VIEW_FROM_LITERAL("-h")) ||
           sxml_byte_view_equals(argument,
                                 SXML_BYTE_VIEW_FROM_LITERAL("--help"));
}

static sxml_cli_usage_error sxml_usage_error_of_kind(
    sxml_cli_usage_error_kind kind) {
    return (sxml_cli_usage_error){
        .kind = kind,
        .argument = sxml_empty_view,
        .is_short_option = false,
        .option = NULL,
        .positional = NULL,
        .suggestion = sxml_empty_view,
    };
}

static void sxml_cli_parser_report(sxml_cli_parser *parser,
                                   sxml_cli_usage_error usage_error) {
    if (parser->has_usage_error) {
        return;
    }
    parser->has_usage_error = true;
    parser->usage_error = usage_error;
}

static void sxml_cli_parser_append_entry(sxml_cli_parser *parser,
                                         sxml_cli_entry entry) {
    sxml_cli_arguments *arguments = parser->arguments;
    /* The capacity bounds the entries of any parse (sxml_cli_entry_capacity),
       so this branch is never taken; it guards the bound. */
    if (arguments->entry_count >= arguments->entry_capacity) {
        return;
    }
    arguments->entries[arguments->entry_count] = entry;
    arguments->entry_count += 1;
}

static size_t sxml_cli_arguments_count_option(
    const sxml_cli_arguments *arguments, size_t option_index) {
    size_t occurrence_count = 0;
    for (size_t entry_index = 0; entry_index < arguments->entry_count;
         entry_index += 1) {
        const sxml_cli_entry *entry = &arguments->entries[entry_index];
        if (entry->kind == SXML_CLI_ENTRY_KIND_OPTION &&
            entry->option_index == option_index) {
            occurrence_count += 1;
        }
    }
    return occurrence_count;
}

static bool sxml_text_list_contains(sxml_cli_text_list list,
                                    sxml_byte_view text) {
    for (size_t text_index = 0; text_index < list.text_count;
         text_index += 1) {
        if (sxml_byte_view_equals(list.texts[text_index], text)) {
            return true;
        }
    }
    return false;
}

/* An option occurrence with its value, about to be recorded. */
typedef struct sxml_cli_option_occurrence {
    size_t option_index;
    sxml_byte_view value;
} sxml_cli_option_occurrence;

static void sxml_cli_parser_record_option(
    sxml_cli_parser *parser, sxml_cli_option_occurrence occurrence) {
    const sxml_cli_option *option =
        &parser->arguments->command->options[occurrence.option_index];
    if (option->multiplicity == SXML_CLI_OPTION_AT_MOST_ONCE &&
        sxml_cli_arguments_count_option(parser->arguments,
                                        occurrence.option_index) > 0) {
        sxml_cli_usage_error usage_error =
            sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_REPEATED_OPTION);
        usage_error.option = option;
        sxml_cli_parser_report(parser, usage_error);
        return;
    }
    if (option->value_kind == SXML_CLI_VALUE_KIND_CHOICE &&
        !sxml_text_list_contains(option->choices, occurrence.value)) {
        sxml_cli_suggestion suggestion =
            sxml_cli_suggestion_start(occurrence.value);
        for (size_t choice_index = 0; choice_index < option->choices.text_count;
             choice_index += 1) {
            sxml_cli_suggestion_consider(&suggestion,
                                         option->choices.texts[choice_index]);
        }
        sxml_cli_usage_error usage_error =
            sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_INVALID_CHOICE);
        usage_error.argument = occurrence.value;
        usage_error.option = option;
        usage_error.suggestion = suggestion.best_candidate;
        sxml_cli_parser_report(parser, usage_error);
        return;
    }
    sxml_cli_parser_append_entry(parser,
                                 (sxml_cli_entry){
                                     .kind = SXML_CLI_ENTRY_KIND_OPTION,
                                     .option_index = occurrence.option_index,
                                     .value = occurrence.value,
                                 });
}

/*
 * Takes the next argument as the value of option, or reports a missing
 * value. Writes the value to *value and returns true when there is one.
 */
static bool sxml_cli_parser_take_next_value(sxml_cli_parser *parser,
                                            const sxml_cli_option *option,
                                            sxml_byte_view *value) {
    const sxml_cli_argument_list argument_list =
        parser->arguments->argument_list;
    if (parser->argument_index + 1 >= argument_list.argument_count) {
        sxml_cli_usage_error usage_error =
            sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_MISSING_VALUE);
        usage_error.option = option;
        sxml_cli_parser_report(parser, usage_error);
        return false;
    }
    parser->argument_index += 1;
    *value = argument_list.arguments[parser->argument_index];
    return true;
}

static bool sxml_cli_command_find_long_option(const sxml_cli_command *command,
                                              sxml_byte_view long_name,
                                              size_t *option_index) {
    for (size_t candidate_index = 0; candidate_index < command->option_count;
         candidate_index += 1) {
        if (sxml_byte_view_equals(command->options[candidate_index].long_name,
                                  long_name)) {
            *option_index = candidate_index;
            return true;
        }
    }
    return false;
}

static bool sxml_cli_command_find_short_option(const sxml_cli_command *command,
                                               uint8_t short_name,
                                               size_t *option_index) {
    for (size_t candidate_index = 0; candidate_index < command->option_count;
         candidate_index += 1) {
        if (command->options[candidate_index].short_name == short_name) {
            *option_index = candidate_index;
            return true;
        }
    }
    return false;
}

static void sxml_cli_parser_report_unknown_long_option(
    sxml_cli_parser *parser, sxml_byte_view name) {
    const sxml_cli_command *command = parser->arguments->command;
    sxml_cli_suggestion suggestion = sxml_cli_suggestion_start(name);
    sxml_cli_suggestion_consider(&suggestion,
                                 sxml_cli_help_option_entry.long_name);
    for (size_t candidate_index = 0; candidate_index < command->option_count;
         candidate_index += 1) {
        sxml_cli_suggestion_consider(
            &suggestion, command->options[candidate_index].long_name);
    }
    sxml_cli_usage_error usage_error =
        sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION);
    usage_error.argument = name;
    usage_error.suggestion = suggestion.best_candidate;
    sxml_cli_parser_report(parser, usage_error);
}

/* Parses "--name", "--name=value" or "--name value". */
static sxml_cli_parse_step sxml_cli_parser_parse_long_option(
    sxml_cli_parser *parser, sxml_byte_view argument) {
    const sxml_byte_view body = sxml_view_suffix(argument, 2);
    /* body is not empty: "--" alone ends options before this is called. */
    const uint8_t *equals_sign =
        body.byte_count == 0 ? NULL : memchr(body.bytes, '=', body.byte_count);
    const bool has_inline_value = equals_sign != NULL;
    const size_t name_byte_count =
        has_inline_value ? (size_t)(equals_sign - body.bytes) : body.byte_count;
    const sxml_byte_view name = {.bytes = body.bytes,
                                 .byte_count = name_byte_count};
    const sxml_byte_view inline_value =
        has_inline_value ? sxml_view_suffix(body, name_byte_count + 1)
                         : sxml_empty_view;

    const sxml_cli_option *option = NULL;
    size_t option_index = 0;
    if (sxml_byte_view_equals(name, sxml_cli_help_option_entry.long_name)) {
        option = &sxml_cli_help_option_entry;
    } else if (sxml_cli_command_find_long_option(parser->arguments->command,
                                                 name, &option_index)) {
        option = &parser->arguments->command->options[option_index];
    } else {
        sxml_cli_parser_report_unknown_long_option(parser, name);
        return SXML_CLI_PARSE_STEP_CONTINUE;
    }

    if (option->value_kind == SXML_CLI_VALUE_KIND_NONE) {
        if (has_inline_value) {
            sxml_cli_usage_error usage_error =
                sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_UNEXPECTED_VALUE);
            usage_error.argument = inline_value;
            usage_error.option = option;
            sxml_cli_parser_report(parser, usage_error);
            return SXML_CLI_PARSE_STEP_CONTINUE;
        }
        if (option == &sxml_cli_help_option_entry) {
            return SXML_CLI_PARSE_STEP_HELP_REQUESTED;
        }
        sxml_cli_parser_record_option(
            parser, (sxml_cli_option_occurrence){.option_index = option_index,
                                                 .value = sxml_empty_view});
        return SXML_CLI_PARSE_STEP_CONTINUE;
    }

    sxml_byte_view value = inline_value;
    if (!has_inline_value &&
        !sxml_cli_parser_take_next_value(parser, option, &value)) {
        return SXML_CLI_PARSE_STEP_CONTINUE;
    }
    sxml_cli_parser_record_option(
        parser, (sxml_cli_option_occurrence){.option_index = option_index,
                                             .value = value});
    return SXML_CLI_PARSE_STEP_CONTINUE;
}

/* Parses a cluster of short options: "-q", "-qv", "-oFILE", "-o FILE". */
static sxml_cli_parse_step sxml_cli_parser_parse_short_options(
    sxml_cli_parser *parser, sxml_byte_view argument) {
    const sxml_cli_command *command = parser->arguments->command;
    for (size_t byte_offset = 1; byte_offset < argument.byte_count;
         byte_offset += 1) {
        const uint8_t short_name = argument.bytes[byte_offset];
        if (short_name == sxml_cli_help_option_entry.short_name) {
            return SXML_CLI_PARSE_STEP_HELP_REQUESTED;
        }
        size_t option_index = 0;
        if (!sxml_cli_command_find_short_option(command, short_name,
                                                &option_index)) {
            sxml_cli_usage_error usage_error =
                sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION);
            usage_error.argument = (sxml_byte_view){
                .bytes = argument.bytes + byte_offset,
                .byte_count = 1,
            };
            usage_error.is_short_option = true;
            sxml_cli_parser_report(parser, usage_error);
            return SXML_CLI_PARSE_STEP_CONTINUE;
        }
        const sxml_cli_option *option = &command->options[option_index];
        if (option->value_kind == SXML_CLI_VALUE_KIND_NONE) {
            sxml_cli_parser_record_option(
                parser, (sxml_cli_option_occurrence){
                            .option_index = option_index,
                            .value = sxml_empty_view});
            if (parser->has_usage_error) {
                return SXML_CLI_PARSE_STEP_CONTINUE;
            }
            continue;
        }
        /* The rest of the argument is the value (POSIX guideline 6 allows
           both forms in practice through getopt), or else the next one. */
        sxml_byte_view value = sxml_view_suffix(argument, byte_offset + 1);
        if (byte_offset + 1 >= argument.byte_count &&
            !sxml_cli_parser_take_next_value(parser, option, &value)) {
            return SXML_CLI_PARSE_STEP_CONTINUE;
        }
        sxml_cli_parser_record_option(
            parser, (sxml_cli_option_occurrence){.option_index = option_index,
                                                 .value = value});
        return SXML_CLI_PARSE_STEP_CONTINUE;
    }
    return SXML_CLI_PARSE_STEP_CONTINUE;
}

static void sxml_cli_parser_append_positional(sxml_cli_parser *parser,
                                              sxml_byte_view argument) {
    sxml_cli_parser_append_entry(parser,
                                 (sxml_cli_entry){
                                     .kind = SXML_CLI_ENTRY_KIND_POSITIONAL,
                                     .option_index = 0,
                                     .value = argument,
                                 });
    if (parser->arguments->stops_at_first_positional) {
        parser->options_ended = true;
    }
}

/*
 * After a usage error the rest of the command line is only searched for a
 * help request, which wins over the error (docs/CLI.md, "Syntax").
 */
static sxml_cli_parse_step sxml_cli_parser_scan_for_help(
    sxml_cli_parser *parser, sxml_byte_view argument) {
    if (sxml_is_help_token(argument)) {
        return SXML_CLI_PARSE_STEP_HELP_REQUESTED;
    }
    const bool ends_scan =
        sxml_byte_view_equals(argument, SXML_BYTE_VIEW_FROM_LITERAL("--")) ||
        (parser->arguments->stops_at_first_positional &&
         sxml_is_positional_token(argument));
    if (ends_scan) {
        parser->options_ended = true;
    }
    return SXML_CLI_PARSE_STEP_CONTINUE;
}

static sxml_cli_parse_step sxml_cli_parser_parse_argument(
    sxml_cli_parser *parser, sxml_byte_view argument) {
    if (parser->has_usage_error) {
        return sxml_cli_parser_scan_for_help(parser, argument);
    }
    if (parser->options_ended || sxml_is_positional_token(argument)) {
        sxml_cli_parser_append_positional(parser, argument);
        return SXML_CLI_PARSE_STEP_CONTINUE;
    }
    if (sxml_byte_view_equals(argument, SXML_BYTE_VIEW_FROM_LITERAL("--"))) {
        parser->options_ended = true;
        return SXML_CLI_PARSE_STEP_CONTINUE;
    }
    if (argument.bytes[1] == '-') {
        return sxml_cli_parser_parse_long_option(parser, argument);
    }
    return sxml_cli_parser_parse_short_options(parser, argument);
}

/* Reports the first option that must be given at least once and was not. */
static void sxml_cli_parser_check_required_options(sxml_cli_parser *parser) {
    const sxml_cli_command *command = parser->arguments->command;
    for (size_t option_index = 0; option_index < command->option_count;
         option_index += 1) {
        if (command->options[option_index].multiplicity ==
                SXML_CLI_OPTION_AT_LEAST_ONCE &&
            sxml_cli_arguments_count_option(parser->arguments, option_index) ==
                0) {
            sxml_cli_usage_error usage_error =
                sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_MISSING_OPTION);
            usage_error.option = &command->options[option_index];
            sxml_cli_parser_report(parser, usage_error);
            return;
        }
    }
}

/* Checks the number of positional arguments against the declarations. */
static void sxml_cli_parser_check_positionals(sxml_cli_parser *parser) {
    const sxml_cli_command *command = parser->arguments->command;
    size_t minimum_count = 0;
    size_t maximum_count = 0;
    bool is_unbounded = false;
    for (size_t positional_index = 0;
         positional_index < command->positional_count; positional_index += 1) {
        switch (command->positionals[positional_index].multiplicity) {
        case SXML_CLI_POSITIONAL_EXACTLY_ONE:
            minimum_count += 1;
            maximum_count += 1;
            break;
        case SXML_CLI_POSITIONAL_OPTIONAL:
            maximum_count += 1;
            break;
        case SXML_CLI_POSITIONAL_ONE_OR_MORE:
            minimum_count += 1;
            is_unbounded = true;
            break;
        case SXML_CLI_POSITIONAL_ZERO_OR_MORE:
            is_unbounded = true;
            break;
        }
    }
    const size_t positional_count =
        sxml_cli_arguments_positional_count(parser->arguments);
    if (positional_count < minimum_count) {
        /* Required positionals come first (sxml_cli_program_validate), so
           the first one without an argument is required. */
        sxml_cli_usage_error usage_error =
            sxml_usage_error_of_kind(SXML_CLI_USAGE_ERROR_MISSING_POSITIONAL);
        usage_error.positional = &command->positionals[positional_count];
        sxml_cli_parser_report(parser, usage_error);
        return;
    }
    if (!is_unbounded && positional_count > maximum_count) {
        sxml_byte_view extra_argument = sxml_empty_view;
        const sxml_status status = sxml_cli_arguments_get_positional(
            parser->arguments, maximum_count, &extra_argument);
        (void)status; /* maximum_count < positional_count */
        sxml_cli_usage_error usage_error = sxml_usage_error_of_kind(
            SXML_CLI_USAGE_ERROR_UNEXPECTED_POSITIONAL);
        usage_error.argument = extra_argument;
        sxml_cli_parser_report(parser, usage_error);
    }
}

sxml_status sxml_cli_arguments_parse(sxml_cli_arguments *arguments,
                                     sxml_cli_parse_outcome *outcome) {
    arguments->entry_count = 0;
    sxml_cli_parser parser = {
        .arguments = arguments,
        .argument_index = 0,
        .options_ended = false,
        .has_usage_error = false,
        .usage_error = sxml_usage_error_of_kind(
            SXML_CLI_USAGE_ERROR_MISSING_COMMAND),
    };
    const sxml_cli_argument_list argument_list = arguments->argument_list;
    for (; parser.argument_index < argument_list.argument_count;
         parser.argument_index += 1) {
        if (parser.has_usage_error && parser.options_ended) {
            break;
        }
        const sxml_cli_parse_step step = sxml_cli_parser_parse_argument(
            &parser, argument_list.arguments[parser.argument_index]);
        if (step == SXML_CLI_PARSE_STEP_HELP_REQUESTED) {
            arguments->entry_count = 0;
            *outcome = (sxml_cli_parse_outcome){
                .kind = SXML_CLI_PARSE_OUTCOME_HELP_REQUESTED,
                .usage_error = parser.usage_error,
            };
            return SXML_STATUS_OK;
        }
    }
    if (!parser.has_usage_error) {
        sxml_cli_parser_check_required_options(&parser);
    }
    if (!parser.has_usage_error) {
        sxml_cli_parser_check_positionals(&parser);
    }
    if (parser.has_usage_error) {
        arguments->entry_count = 0;
        *outcome = (sxml_cli_parse_outcome){
            .kind = SXML_CLI_PARSE_OUTCOME_USAGE_ERROR,
            .usage_error = parser.usage_error,
        };
        return SXML_STATUS_OK;
    }
    *outcome = (sxml_cli_parse_outcome){
        .kind = SXML_CLI_PARSE_OUTCOME_PARSED,
        .usage_error = parser.usage_error,
    };
    return SXML_STATUS_OK;
}

/* -------------------------------------------------------------- Queries */

sxml_status sxml_cli_arguments_find_option(const sxml_cli_arguments *arguments,
                                           sxml_cli_option_lookup lookup,
                                           sxml_cli_option_match *match) {
    size_t option_index = 0;
    if (!sxml_cli_command_find_long_option(arguments->command,
                                           lookup.long_name, &option_index)) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
    const sxml_cli_option *option = &arguments->command->options[option_index];
    sxml_cli_option_match found = {
        .is_present = false,
        .occurrence_count = 0,
        .value = option->default_value,
    };
    for (size_t entry_index = 0; entry_index < arguments->entry_count;
         entry_index += 1) {
        const sxml_cli_entry *entry = &arguments->entries[entry_index];
        if (entry->kind != SXML_CLI_ENTRY_KIND_OPTION ||
            entry->option_index != option_index) {
            continue;
        }
        if (found.occurrence_count == lookup.occurrence_index) {
            found.is_present = true;
            found.value = entry->value;
        }
        found.occurrence_count += 1;
    }
    *match = found;
    return SXML_STATUS_OK;
}

size_t sxml_cli_arguments_positional_count(
    const sxml_cli_arguments *arguments) {
    size_t positional_count = 0;
    for (size_t entry_index = 0; entry_index < arguments->entry_count;
         entry_index += 1) {
        if (arguments->entries[entry_index].kind ==
            SXML_CLI_ENTRY_KIND_POSITIONAL) {
            positional_count += 1;
        }
    }
    return positional_count;
}

sxml_status sxml_cli_arguments_get_positional(
    const sxml_cli_arguments *arguments, size_t positional_index,
    sxml_byte_view *positional) {
    size_t seen_count = 0;
    for (size_t entry_index = 0; entry_index < arguments->entry_count;
         entry_index += 1) {
        const sxml_cli_entry *entry = &arguments->entries[entry_index];
        if (entry->kind != SXML_CLI_ENTRY_KIND_POSITIONAL) {
            continue;
        }
        if (seen_count == positional_index) {
            *positional = entry->value;
            return SXML_STATUS_OK;
        }
        seen_count += 1;
    }
    return SXML_STATUS_INDEX_OUT_OF_RANGE;
}
