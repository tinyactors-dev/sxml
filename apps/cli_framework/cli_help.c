/*
 * Help texts and usage errors (cli_help.h). Layout after the Command Line
 * Interface Guidelines (https://clig.dev, "Help"): usage line first, then
 * description, options, positional arguments and examples.
 */
#include "cli_help.h"

#include <stdbool.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* Spaces between the left column and the summary in help tables. */
enum { SXML_CLI_COLUMN_GAP = 2, SXML_CLI_INDENT = 2 };

/* The command whose table describes the request: the root for a program. */
static const sxml_cli_command *sxml_cli_help_request_command(
    sxml_cli_help_request request) {
    return request.command != NULL ? request.command : sxml_cli_root_command();
}

/* "sxml" or "sxml version". */
static void sxml_cli_write_command_path(sxml_cli_writer *writer,
                                        sxml_cli_help_request request) {
    sxml_cli_writer_write(writer, request.program->name);
    if (request.command != NULL) {
        sxml_cli_writer_write_byte(writer, ' ');
        sxml_cli_writer_write(writer, request.command->name);
    }
}

void sxml_cli_write_diagnostic_prefix(sxml_cli_writer *writer,
                                      sxml_cli_help_request request) {
    sxml_cli_write_command_path(writer, request);
    sxml_cli_writer_write(writer, SXML_TEXT(": "));
}

void sxml_cli_write_help_hint(sxml_cli_writer *writer,
                              sxml_cli_help_request request) {
    sxml_cli_writer_write(writer, SXML_TEXT("Try '"));
    sxml_cli_write_command_path(writer, request);
    sxml_cli_writer_write_line(writer, SXML_TEXT(" --help'."));
}

/* ----------------------------------------------------------- Usage line */

static void sxml_cli_write_option_usage(sxml_cli_writer *writer,
                                        const sxml_cli_option *option) {
    sxml_cli_writer_write(writer, SXML_TEXT(" [--"));
    sxml_cli_writer_write(writer, option->long_name);
    if (option->value_kind != SXML_CLI_VALUE_KIND_NONE) {
        sxml_cli_writer_write_byte(writer, '=');
        sxml_cli_writer_write(writer, option->value_name);
    }
    sxml_cli_writer_write_byte(writer, ']');
    if (option->multiplicity == SXML_CLI_OPTION_REPEATABLE) {
        sxml_cli_writer_write(writer, SXML_TEXT("..."));
    }
}

static void sxml_cli_write_positional_usage(
    sxml_cli_writer *writer, const sxml_cli_positional *positional) {
    sxml_cli_writer_write_byte(writer, ' ');
    switch (positional->multiplicity) {
    case SXML_CLI_POSITIONAL_EXACTLY_ONE:
        sxml_cli_writer_write(writer, positional->name);
        break;
    case SXML_CLI_POSITIONAL_OPTIONAL:
        sxml_cli_writer_write_byte(writer, '[');
        sxml_cli_writer_write(writer, positional->name);
        sxml_cli_writer_write_byte(writer, ']');
        break;
    case SXML_CLI_POSITIONAL_ONE_OR_MORE:
        sxml_cli_writer_write(writer, positional->name);
        sxml_cli_writer_write(writer, SXML_TEXT("..."));
        break;
    case SXML_CLI_POSITIONAL_ZERO_OR_MORE:
        sxml_cli_writer_write_byte(writer, '[');
        sxml_cli_writer_write(writer, positional->name);
        sxml_cli_writer_write(writer, SXML_TEXT("...]"));
        break;
    }
}

void sxml_cli_write_usage_line(sxml_cli_writer *writer,
                               sxml_cli_help_request request) {
    const sxml_cli_command *command = sxml_cli_help_request_command(request);
    sxml_cli_writer_write(writer, SXML_TEXT("Usage: "));
    sxml_cli_write_command_path(writer, request);
    for (size_t option_index = 0; option_index < command->option_count;
         option_index += 1) {
        sxml_cli_write_option_usage(writer, &command->options[option_index]);
    }
    for (size_t positional_index = 0;
         positional_index < command->positional_count; positional_index += 1) {
        sxml_cli_write_positional_usage(
            writer, &command->positionals[positional_index]);
    }
    sxml_cli_writer_write_byte(writer, '\n');
}

/* ------------------------------------------------------------ Help text */

/* Bytes of "-h, --help" or "    --format=FORMAT" in the option table. */
static size_t sxml_cli_option_column_byte_count(const sxml_cli_option *option) {
    size_t byte_count = 4 + 2 + option->long_name.byte_count;
    if (option->value_kind != SXML_CLI_VALUE_KIND_NONE) {
        byte_count += 1 + option->value_name.byte_count;
    }
    return byte_count;
}

/* Writes the parenthesized notes after an option's summary. */
static void sxml_cli_write_option_notes(sxml_cli_writer *writer,
                                        const sxml_cli_option *option) {
    bool has_note = false;
    if (option->value_kind == SXML_CLI_VALUE_KIND_CHOICE) {
        sxml_cli_writer_write(writer, SXML_TEXT(" (one of: "));
        for (size_t choice_index = 0; choice_index < option->choices.text_count;
             choice_index += 1) {
            if (choice_index > 0) {
                sxml_cli_writer_write(writer, SXML_TEXT(", "));
            }
            sxml_cli_writer_write(writer, option->choices.texts[choice_index]);
        }
        has_note = true;
    }
    if (option->default_value.byte_count > 0) {
        sxml_cli_writer_write(writer,
                              has_note ? SXML_TEXT("; ") : SXML_TEXT(" ("));
        sxml_cli_writer_write(writer, SXML_TEXT("default: "));
        sxml_cli_writer_write(writer, option->default_value);
        has_note = true;
    }
    if (option->multiplicity == SXML_CLI_OPTION_REPEATABLE) {
        sxml_cli_writer_write(writer,
                              has_note ? SXML_TEXT("; ") : SXML_TEXT(" ("));
        sxml_cli_writer_write(writer, SXML_TEXT("repeatable"));
        has_note = true;
    }
    if (has_note) {
        sxml_cli_writer_write_byte(writer, ')');
    }
}

/* One row of the option table, padded to column_byte_count. */
typedef struct sxml_cli_option_row {
    const sxml_cli_option *option;
    size_t column_byte_count;
} sxml_cli_option_row;

static void sxml_cli_write_option_row(sxml_cli_writer *writer,
                                      sxml_cli_option_row row) {
    const sxml_cli_option *option = row.option;
    sxml_cli_writer_write_spaces(writer, SXML_CLI_INDENT);
    if (option->short_name != 0) {
        sxml_cli_writer_write_byte(writer, '-');
        sxml_cli_writer_write_byte(writer, option->short_name);
        sxml_cli_writer_write(writer, SXML_TEXT(", "));
    } else {
        sxml_cli_writer_write_spaces(writer, 4);
    }
    sxml_cli_writer_write(writer, SXML_TEXT("--"));
    sxml_cli_writer_write(writer, option->long_name);
    if (option->value_kind != SXML_CLI_VALUE_KIND_NONE) {
        sxml_cli_writer_write_byte(writer, '=');
        sxml_cli_writer_write(writer, option->value_name);
    }
    sxml_cli_writer_write_spaces(
        writer, row.column_byte_count -
                    sxml_cli_option_column_byte_count(option) +
                    SXML_CLI_COLUMN_GAP);
    sxml_cli_writer_write(writer, option->summary);
    sxml_cli_write_option_notes(writer, option);
    sxml_cli_writer_write_byte(writer, '\n');
}

static void sxml_cli_write_options_section(sxml_cli_writer *writer,
                                           const sxml_cli_command *command) {
    const sxml_cli_option *help_option = sxml_cli_help_option();
    size_t column_byte_count = sxml_cli_option_column_byte_count(help_option);
    for (size_t option_index = 0; option_index < command->option_count;
         option_index += 1) {
        const size_t byte_count = sxml_cli_option_column_byte_count(
            &command->options[option_index]);
        if (byte_count > column_byte_count) {
            column_byte_count = byte_count;
        }
    }
    sxml_cli_writer_write(writer, SXML_TEXT("\nOptions:\n"));
    sxml_cli_write_option_row(
        writer, (sxml_cli_option_row){.option = help_option,
                                      .column_byte_count = column_byte_count});
    for (size_t option_index = 0; option_index < command->option_count;
         option_index += 1) {
        sxml_cli_write_option_row(
            writer,
            (sxml_cli_option_row){.option = &command->options[option_index],
                                  .column_byte_count = column_byte_count});
    }
}

/* One row of a two-column table of names and summaries. */
typedef struct sxml_cli_name_row {
    sxml_byte_view name;
    sxml_byte_view summary;
    size_t column_byte_count;
} sxml_cli_name_row;

static void sxml_cli_write_name_row(sxml_cli_writer *writer,
                                    sxml_cli_name_row row) {
    sxml_cli_writer_write_spaces(writer, SXML_CLI_INDENT);
    sxml_cli_writer_write(writer, row.name);
    sxml_cli_writer_write_spaces(writer, row.column_byte_count -
                                             row.name.byte_count +
                                             SXML_CLI_COLUMN_GAP);
    sxml_cli_writer_write_line(writer, row.summary);
}

static void sxml_cli_write_positionals_section(
    sxml_cli_writer *writer, const sxml_cli_command *command) {
    if (command->positional_count == 0) {
        return;
    }
    size_t column_byte_count = 0;
    for (size_t positional_index = 0;
         positional_index < command->positional_count; positional_index += 1) {
        const size_t byte_count =
            command->positionals[positional_index].name.byte_count;
        if (byte_count > column_byte_count) {
            column_byte_count = byte_count;
        }
    }
    sxml_cli_writer_write(writer, SXML_TEXT("\nArguments:\n"));
    for (size_t positional_index = 0;
         positional_index < command->positional_count; positional_index += 1) {
        const sxml_cli_positional *positional =
            &command->positionals[positional_index];
        sxml_cli_write_name_row(
            writer, (sxml_cli_name_row){
                        .name = positional->name,
                        .summary = positional->summary,
                        .column_byte_count = column_byte_count,
                    });
    }
}

static void sxml_cli_write_examples_section(sxml_cli_writer *writer,
                                            sxml_cli_text_list examples) {
    if (examples.text_count == 0) {
        return;
    }
    sxml_cli_writer_write(writer, SXML_TEXT("\nExamples:\n"));
    for (size_t example_index = 0; example_index < examples.text_count;
         example_index += 1) {
        sxml_cli_writer_write_spaces(writer, SXML_CLI_INDENT);
        sxml_cli_writer_write_line(writer, examples.texts[example_index]);
    }
}

static void sxml_cli_write_commands_section(sxml_cli_writer *writer,
                                            const sxml_cli_program *program) {
    const sxml_cli_command *help_command = sxml_cli_help_command();
    size_t column_byte_count = help_command->name.byte_count;
    for (size_t command_index = 0; command_index < program->command_count;
         command_index += 1) {
        const size_t byte_count =
            program->commands[command_index]->name.byte_count;
        if (byte_count > column_byte_count) {
            column_byte_count = byte_count;
        }
    }
    sxml_cli_writer_write(writer, SXML_TEXT("\nCommands:\n"));
    sxml_cli_write_name_row(writer, (sxml_cli_name_row){
                                        .name = help_command->name,
                                        .summary = help_command->summary,
                                        .column_byte_count = column_byte_count,
                                    });
    for (size_t command_index = 0; command_index < program->command_count;
         command_index += 1) {
        const sxml_cli_command *command = program->commands[command_index];
        sxml_cli_write_name_row(writer, (sxml_cli_name_row){
                                            .name = command->name,
                                            .summary = command->summary,
                                            .column_byte_count =
                                                column_byte_count,
                                        });
    }
}

static void sxml_cli_write_program_help(sxml_cli_writer *writer,
                                        const sxml_cli_program *program) {
    const sxml_cli_help_request request = {.program = program,
                                           .command = NULL};
    sxml_cli_writer_write(writer, program->name);
    sxml_cli_writer_write(writer, SXML_TEXT(" - "));
    sxml_cli_writer_write_line(writer, program->summary);
    sxml_cli_writer_write_byte(writer, '\n');
    sxml_cli_write_usage_line(writer, request);
    if (program->description.byte_count > 0) {
        sxml_cli_writer_write_byte(writer, '\n');
        sxml_cli_writer_write_line(writer, program->description);
    }
    sxml_cli_write_commands_section(writer, program);
    sxml_cli_write_options_section(writer, sxml_cli_root_command());
    sxml_cli_write_examples_section(writer, program->examples);
    sxml_cli_writer_write(writer, SXML_TEXT("\nRun '"));
    sxml_cli_writer_write(writer, program->name);
    sxml_cli_writer_write_line(writer,
                               SXML_TEXT(" help COMMAND' for help on one command."));
    if (program->bug_report_location.byte_count > 0) {
        sxml_cli_writer_write(writer, SXML_TEXT("Report bugs at "));
        sxml_cli_writer_write_line(writer, program->bug_report_location);
    }
}

static void sxml_cli_write_command_help(sxml_cli_writer *writer,
                                        sxml_cli_help_request request) {
    const sxml_cli_command *command = request.command;
    sxml_cli_write_command_path(writer, request);
    sxml_cli_writer_write(writer, SXML_TEXT(" - "));
    sxml_cli_writer_write_line(writer, command->summary);
    sxml_cli_writer_write_byte(writer, '\n');
    sxml_cli_write_usage_line(writer, request);
    if (command->description.byte_count > 0) {
        sxml_cli_writer_write_byte(writer, '\n');
        sxml_cli_writer_write_line(writer, command->description);
    }
    sxml_cli_write_options_section(writer, command);
    sxml_cli_write_positionals_section(writer, command);
    sxml_cli_write_examples_section(writer, command->examples);
}

void sxml_cli_write_help(sxml_cli_writer *writer,
                         sxml_cli_help_request request) {
    if (request.command == NULL) {
        sxml_cli_write_program_help(writer, request.program);
        return;
    }
    sxml_cli_write_command_help(writer, request);
}

/* ---------------------------------------------------------- Usage error */

static void sxml_cli_write_option_name(sxml_cli_writer *writer,
                                       const sxml_cli_option *option) {
    sxml_cli_writer_write(writer, SXML_TEXT("'--"));
    sxml_cli_writer_write(writer, option->long_name);
    sxml_cli_writer_write_byte(writer, '\'');
}

static void sxml_cli_write_choices(sxml_cli_writer *writer,
                                   const sxml_cli_option *option) {
    sxml_cli_writer_write(writer, SXML_TEXT(" (one of: "));
    for (size_t choice_index = 0; choice_index < option->choices.text_count;
         choice_index += 1) {
        if (choice_index > 0) {
            sxml_cli_writer_write(writer, SXML_TEXT(", "));
        }
        sxml_cli_writer_write(writer, option->choices.texts[choice_index]);
    }
    sxml_cli_writer_write_byte(writer, ')');
}

/* Writes what is wrong, without prefix, suggestion or line feed. */
static void sxml_cli_write_usage_error_description(
    sxml_cli_writer *writer, const sxml_cli_usage_error *usage_error) {
    switch (usage_error->kind) {
    case SXML_CLI_USAGE_ERROR_MISSING_COMMAND:
        sxml_cli_writer_write(writer, SXML_TEXT("no command given"));
        break;
    case SXML_CLI_USAGE_ERROR_UNKNOWN_COMMAND:
        sxml_cli_writer_write(writer, SXML_TEXT("unknown command "));
        sxml_cli_writer_write_quoted(writer, usage_error->argument);
        break;
    case SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION:
        sxml_cli_writer_write(writer, usage_error->is_short_option
                                          ? SXML_TEXT("unknown option '-")
                                          : SXML_TEXT("unknown option '--"));
        sxml_cli_writer_write_escaped(writer, usage_error->argument);
        sxml_cli_writer_write_byte(writer, '\'');
        break;
    case SXML_CLI_USAGE_ERROR_MISSING_VALUE:
        sxml_cli_writer_write(writer, SXML_TEXT("option "));
        sxml_cli_write_option_name(writer, usage_error->option);
        sxml_cli_writer_write(writer, SXML_TEXT(" needs a value ("));
        sxml_cli_writer_write(writer, usage_error->option->value_name);
        sxml_cli_writer_write_byte(writer, ')');
        break;
    case SXML_CLI_USAGE_ERROR_UNEXPECTED_VALUE:
        sxml_cli_writer_write(writer, SXML_TEXT("option "));
        sxml_cli_write_option_name(writer, usage_error->option);
        sxml_cli_writer_write(writer, SXML_TEXT(" takes no value"));
        break;
    case SXML_CLI_USAGE_ERROR_INVALID_CHOICE:
        sxml_cli_writer_write(writer, SXML_TEXT("invalid value "));
        sxml_cli_writer_write_quoted(writer, usage_error->argument);
        sxml_cli_writer_write(writer, SXML_TEXT(" for option "));
        sxml_cli_write_option_name(writer, usage_error->option);
        sxml_cli_write_choices(writer, usage_error->option);
        break;
    case SXML_CLI_USAGE_ERROR_REPEATED_OPTION:
        sxml_cli_writer_write(writer, SXML_TEXT("option "));
        sxml_cli_write_option_name(writer, usage_error->option);
        sxml_cli_writer_write(writer, SXML_TEXT(" given more than once"));
        break;
    case SXML_CLI_USAGE_ERROR_MISSING_POSITIONAL:
        sxml_cli_writer_write(writer, SXML_TEXT("missing argument "));
        sxml_cli_writer_write(writer, usage_error->positional->name);
        break;
    case SXML_CLI_USAGE_ERROR_UNEXPECTED_POSITIONAL:
        sxml_cli_writer_write(writer, SXML_TEXT("unexpected argument "));
        sxml_cli_writer_write_quoted(writer, usage_error->argument);
        break;
    }
}

void sxml_cli_write_usage_error(sxml_cli_writer *writer,
                                const sxml_cli_usage_error_report *report) {
    const sxml_cli_help_request request = {.program = report->program,
                                           .command = report->command};
    const sxml_cli_usage_error *usage_error = &report->usage_error;
    sxml_cli_write_diagnostic_prefix(writer, request);
    sxml_cli_write_usage_error_description(writer, usage_error);
    if (usage_error->suggestion.byte_count > 0) {
        sxml_cli_writer_write(writer, SXML_TEXT("; did you mean '"));
        if (usage_error->kind == SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION) {
            sxml_cli_writer_write(writer, SXML_TEXT("--"));
        }
        sxml_cli_writer_write(writer, usage_error->suggestion);
        sxml_cli_writer_write(writer, SXML_TEXT("'?"));
    }
    sxml_cli_writer_write_byte(writer, '\n');
    sxml_cli_write_usage_line(writer, request);
    sxml_cli_write_help_hint(writer, request);
}
