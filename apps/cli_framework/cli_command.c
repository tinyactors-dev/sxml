/*
 * Command tables: validation, lookup and command-reported usage errors
 * (cli_command.h).
 */
#include "cli_command.h"

#include "cli_help.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_no_name = {.bytes = NULL, .byte_count = 0};

sxml_cli_exit_code sxml_cli_invocation_report_usage_error(
    const sxml_cli_invocation *invocation, sxml_byte_view description) {
    const sxml_cli_help_request request = {.program = invocation->program,
                                           .command = invocation->command};
    sxml_cli_write_diagnostic_prefix(invocation->standard_error, request);
    sxml_cli_writer_write_line(invocation->standard_error, description);
    sxml_cli_write_usage_line(invocation->standard_error, request);
    sxml_cli_write_help_hint(invocation->standard_error, request);
    return SXML_CLI_EXIT_USAGE_ERROR;
}

bool sxml_cli_program_find_command(const sxml_cli_program *program,
                                   sxml_byte_view name,
                                   const sxml_cli_command **command) {
    const sxml_cli_command *help_command = sxml_cli_help_command();
    if (sxml_byte_view_equals(name, help_command->name)) {
        *command = help_command;
        return true;
    }
    for (size_t command_index = 0; command_index < program->command_count;
         command_index += 1) {
        if (sxml_byte_view_equals(program->commands[command_index]->name,
                                  name)) {
            *command = program->commands[command_index];
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------- Validation */

/* Where a broken rule was found. */
typedef struct sxml_cli_table_place {
    sxml_byte_view command_name;
    sxml_byte_view item_name;
} sxml_cli_table_place;

static sxml_status sxml_cli_report_problem(sxml_cli_table_problem *problem,
                                           sxml_byte_view description,
                                           sxml_cli_table_place place) {
    *problem = (sxml_cli_table_problem){
        .description = description,
        .command_name = place.command_name,
        .item_name = place.item_name,
    };
    return SXML_STATUS_INVALID_ARGUMENT;
}

static bool sxml_is_ascii_letter_or_digit(uint8_t byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9');
}

static bool sxml_text_list_has(sxml_cli_text_list list, sxml_byte_view text) {
    for (size_t text_index = 0; text_index < list.text_count;
         text_index += 1) {
        if (sxml_byte_view_equals(list.texts[text_index], text)) {
            return true;
        }
    }
    return false;
}

static bool sxml_view_contains_byte(sxml_byte_view text, uint8_t byte) {
    for (size_t byte_index = 0; byte_index < text.byte_count;
         byte_index += 1) {
        if (text.bytes[byte_index] == byte) {
            return true;
        }
    }
    return false;
}

/* The rules for one option on its own. */
static sxml_byte_view sxml_cli_option_problem(const sxml_cli_option *option) {
    if (option->long_name.byte_count == 0 ||
        option->long_name.bytes[0] == '-' ||
        sxml_view_contains_byte(option->long_name, '=')) {
        return SXML_TEXT("option long name is empty or has '-' first or '='");
    }
    if (sxml_byte_view_equals(option->long_name, SXML_TEXT("help")) ||
        option->short_name == 'h') {
        return SXML_TEXT("--help and -h are built in");
    }
    if (option->short_name != 0 &&
        !sxml_is_ascii_letter_or_digit(option->short_name)) {
        return SXML_TEXT("short name is not an ASCII letter or digit");
    }
    if (option->summary.byte_count == 0) {
        return SXML_TEXT("option has no summary");
    }
    const bool takes_value = option->value_kind != SXML_CLI_VALUE_KIND_NONE;
    if (takes_value != (option->value_name.byte_count > 0)) {
        return SXML_TEXT("value name must be given exactly for options with a value");
    }
    const bool is_choice = option->value_kind == SXML_CLI_VALUE_KIND_CHOICE;
    if (is_choice != (option->choices.text_count > 0)) {
        return SXML_TEXT("choices must be given exactly for choice options");
    }
    if (!takes_value && option->default_value.byte_count > 0) {
        return SXML_TEXT("a flag has no default value");
    }
    if (is_choice && option->default_value.byte_count > 0 &&
        !sxml_text_list_has(option->choices, option->default_value)) {
        return SXML_TEXT("default value is not one of the choices");
    }
    return sxml_no_name;
}

static sxml_status sxml_cli_validate_options(const sxml_cli_command *command,
                                             sxml_cli_table_problem *problem) {
    for (size_t option_index = 0; option_index < command->option_count;
         option_index += 1) {
        const sxml_cli_option *option = &command->options[option_index];
        const sxml_cli_table_place place = {.command_name = command->name,
                                            .item_name = option->long_name};
        const sxml_byte_view description = sxml_cli_option_problem(option);
        if (description.byte_count > 0) {
            return sxml_cli_report_problem(problem, description, place);
        }
        for (size_t other_index = 0; other_index < option_index;
             other_index += 1) {
            const sxml_cli_option *other = &command->options[other_index];
            if (sxml_byte_view_equals(other->long_name, option->long_name)) {
                return sxml_cli_report_problem(
                    problem, SXML_TEXT("option long name is not unique"),
                    place);
            }
            if (option->short_name != 0 &&
                other->short_name == option->short_name) {
                return sxml_cli_report_problem(
                    problem, SXML_TEXT("option short name is not unique"),
                    place);
            }
        }
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_cli_validate_positionals(
    const sxml_cli_command *command, sxml_cli_table_problem *problem) {
    bool has_open_ended = false;
    for (size_t positional_index = 0;
         positional_index < command->positional_count; positional_index += 1) {
        const sxml_cli_positional *positional =
            &command->positionals[positional_index];
        const sxml_cli_table_place place = {.command_name = command->name,
                                            .item_name = positional->name};
        if (positional->name.byte_count == 0 ||
            positional->summary.byte_count == 0) {
            return sxml_cli_report_problem(
                problem, SXML_TEXT("positional has no name or no summary"),
                place);
        }
        if (has_open_ended) {
            return sxml_cli_report_problem(
                problem,
                SXML_TEXT("only the last positional may be optional or take "
                          "several arguments"),
                place);
        }
        has_open_ended =
            positional->multiplicity != SXML_CLI_POSITIONAL_EXACTLY_ONE;
    }
    return SXML_STATUS_OK;
}

static sxml_status sxml_cli_validate_command(const sxml_cli_command *command,
                                             sxml_cli_table_problem *problem) {
    const sxml_cli_table_place place = {.command_name = command->name,
                                        .item_name = sxml_no_name};
    if (command->name.byte_count == 0 || command->summary.byte_count == 0) {
        return sxml_cli_report_problem(
            problem, SXML_TEXT("command has no name or no summary"), place);
    }
    if (command->run == NULL) {
        return sxml_cli_report_problem(
            problem, SXML_TEXT("command has no function"), place);
    }
    const sxml_status status = sxml_cli_validate_options(command, problem);
    if (status != SXML_STATUS_OK) {
        return status;
    }
    return sxml_cli_validate_positionals(command, problem);
}

sxml_status sxml_cli_program_validate(const sxml_cli_program *program,
                                      sxml_cli_table_problem *problem) {
    const sxml_cli_table_place program_place = {.command_name = sxml_no_name,
                                                .item_name = sxml_no_name};
    if (program->name.byte_count == 0 || program->summary.byte_count == 0) {
        return sxml_cli_report_problem(
            problem, SXML_TEXT("program has no name or no summary"),
            program_place);
    }
    for (size_t command_index = 0; command_index < program->command_count;
         command_index += 1) {
        const sxml_cli_command *command = program->commands[command_index];
        const sxml_cli_table_place place = {.command_name = command->name,
                                            .item_name = sxml_no_name};
        if (sxml_byte_view_equals(command->name, sxml_cli_help_command()->name)) {
            return sxml_cli_report_problem(
                problem, SXML_TEXT("the help command is built in"), place);
        }
        for (size_t other_index = 0; other_index < command_index;
             other_index += 1) {
            if (sxml_byte_view_equals(program->commands[other_index]->name,
                                      command->name)) {
                return sxml_cli_report_problem(
                    problem, SXML_TEXT("command name is not unique"), place);
            }
        }
        const sxml_status status = sxml_cli_validate_command(command, problem);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

sxml_status sxml_cli_file_reader_read(sxml_cli_file_reader reader,
                                      const sxml_cli_file_read_request *request,
                                      sxml_byte_view *contents) {
    if (reader.operations == NULL || reader.operations->read_file == NULL) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return reader.operations->read_file(reader.state, request, contents);
}

bool sxml_cli_file_reader_streams(sxml_cli_file_reader reader) {
    return reader.operations != NULL && reader.operations->open_file != NULL &&
           reader.operations->close_file != NULL;
}

sxml_status sxml_cli_file_reader_open(sxml_cli_file_reader reader,
                                      const sxml_cli_file_read_request *request,
                                      sxml_byte_reader *byte_reader) {
    if (!sxml_cli_file_reader_streams(reader)) {
        return SXML_STATUS_INPUT_OUTPUT_FAILED;
    }
    return reader.operations->open_file(reader.state, request, byte_reader);
}

void sxml_cli_file_reader_close(sxml_cli_file_reader reader, sxml_byte_reader byte_reader) {
    if (sxml_cli_file_reader_streams(reader)) {
        reader.operations->close_file(reader.state, byte_reader);
    }
}
