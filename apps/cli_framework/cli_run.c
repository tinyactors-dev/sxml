/*
 * Dispatch and exit codes (cli_run.h), and the built-in tables: the
 * arguments before the command name and the "help" command.
 */
#include "cli_run.h"

#include "cli_arguments.h"
#include "cli_help.h"
#include "cli_suggestion.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------ Built-in tables */

static const sxml_cli_option sxml_cli_root_options[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("version"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_NONE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER(""),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("Print the version and exit"),
    },
};

static const sxml_cli_positional sxml_cli_root_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("COMMAND"),
        .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The command to run"),
    },
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("ARGUMENT"),
        .multiplicity = SXML_CLI_POSITIONAL_ZERO_OR_MORE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The command's arguments"),
    },
};

static sxml_cli_exit_code sxml_cli_run_nothing(
    const sxml_cli_invocation *invocation) {
    (void)invocation;
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* What help shows for the arguments before the command name. */
static const sxml_cli_command sxml_cli_root_command_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER(""),
    .summary = SXML_BYTE_VIEW_INITIALIZER(""),
    .description = SXML_BYTE_VIEW_INITIALIZER(""),
    .options = sxml_cli_root_options,
    .option_count = sizeof sxml_cli_root_options / sizeof sxml_cli_root_options[0],
    .positionals = sxml_cli_root_positionals,
    .positional_count =
        sizeof sxml_cli_root_positionals / sizeof sxml_cli_root_positionals[0],
    .examples = {.texts = NULL, .text_count = 0},
    .run = sxml_cli_run_nothing,
};

/*
 * What is parsed before the command name: the same options, with every
 * positional collected, so that "--version" alone and a missing command can
 * be told apart after parsing.
 */
static const sxml_cli_command sxml_cli_root_parse_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER(""),
    .summary = SXML_BYTE_VIEW_INITIALIZER(""),
    .description = SXML_BYTE_VIEW_INITIALIZER(""),
    .options = sxml_cli_root_options,
    .option_count = sizeof sxml_cli_root_options / sizeof sxml_cli_root_options[0],
    .positionals = &sxml_cli_root_positionals[1],
    .positional_count = 1,
    .examples = {.texts = NULL, .text_count = 0},
    .run = sxml_cli_run_nothing,
};

const sxml_cli_command *sxml_cli_root_command(void) {
    return &sxml_cli_root_command_entry;
}

static sxml_cli_exit_code sxml_cli_run_help_command(
    const sxml_cli_invocation *invocation);

static const sxml_cli_positional sxml_cli_help_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("COMMAND"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The command to show help for"),
    },
};

static const sxml_byte_view sxml_cli_help_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml help"),
    SXML_BYTE_VIEW_INITIALIZER("sxml help version"),
};

static const sxml_cli_command sxml_cli_help_command_entry = {
    .name = SXML_BYTE_VIEW_INITIALIZER("help"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "Show help for the program or for one command"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Without COMMAND, shows the list of commands. With COMMAND, shows the\n"
        "same help as 'COMMAND --help'."),
    .options = NULL,
    .option_count = 0,
    .positionals = sxml_cli_help_positionals,
    .positional_count = 1,
    .examples = {.texts = sxml_cli_help_examples,
                 .text_count = sizeof sxml_cli_help_examples /
                               sizeof sxml_cli_help_examples[0]},
    .run = sxml_cli_run_help_command,
};

const sxml_cli_command *sxml_cli_help_command(void) {
    return &sxml_cli_help_command_entry;
}

/* ------------------------------------------------------------ Dispatch */

static sxml_cli_usage_error sxml_cli_unknown_command_error(
    const sxml_cli_program *program, sxml_byte_view name) {
    sxml_cli_suggestion suggestion = sxml_cli_suggestion_start(name);
    sxml_cli_suggestion_consider(&suggestion, sxml_cli_help_command_entry.name);
    for (size_t command_index = 0; command_index < program->command_count;
         command_index += 1) {
        sxml_cli_suggestion_consider(&suggestion,
                                     program->commands[command_index]->name);
    }
    return (sxml_cli_usage_error){
        .kind = SXML_CLI_USAGE_ERROR_UNKNOWN_COMMAND,
        .argument = name,
        .is_short_option = false,
        .option = NULL,
        .positional = NULL,
        .suggestion = suggestion.best_candidate,
    };
}

static sxml_cli_usage_error sxml_cli_usage_error_without_details(
    sxml_cli_usage_error_kind kind) {
    return (sxml_cli_usage_error){
        .kind = kind,
        .argument = {.bytes = NULL, .byte_count = 0},
        .is_short_option = false,
        .option = NULL,
        .positional = NULL,
        .suggestion = {.bytes = NULL, .byte_count = 0},
    };
}

static sxml_cli_exit_code sxml_cli_run_help_command(
    const sxml_cli_invocation *invocation) {
    sxml_byte_view command_name = {.bytes = NULL, .byte_count = 0};
    const sxml_status status = sxml_cli_arguments_get_positional(
        invocation->arguments, 0, &command_name);
    if (status != SXML_STATUS_OK) {
        sxml_cli_write_help(invocation->standard_output,
                            (sxml_cli_help_request){
                                .program = invocation->program,
                                .command = NULL,
                            });
        return SXML_CLI_EXIT_SUCCESS;
    }
    const sxml_cli_command *command = NULL;
    if (!sxml_cli_program_find_command(invocation->program, command_name,
                                       &command)) {
        const sxml_cli_usage_error_report report = {
            .program = invocation->program,
            .command = invocation->command,
            .usage_error =
                sxml_cli_unknown_command_error(invocation->program,
                                               command_name),
        };
        sxml_cli_write_usage_error(invocation->standard_error, &report);
        return SXML_CLI_EXIT_USAGE_ERROR;
    }
    sxml_cli_write_help(invocation->standard_output,
                        (sxml_cli_help_request){
                            .program = invocation->program,
                            .command = command,
                        });
    return SXML_CLI_EXIT_SUCCESS;
}

/* The streams and memory of one run, with the writers over the streams. */
typedef struct sxml_cli_session {
    const sxml_cli_program *program;
    const sxml_cli_run_request *request;
    sxml_cli_writer standard_output;
    sxml_cli_writer standard_error;
} sxml_cli_session;

static sxml_cli_exit_code sxml_cli_report_usage_error(
    sxml_cli_session *session, const sxml_cli_command *command,
    sxml_cli_usage_error usage_error) {
    const sxml_cli_usage_error_report report = {
        .program = session->program,
        .command = command,
        .usage_error = usage_error,
    };
    sxml_cli_write_usage_error(&session->standard_error, &report);
    return SXML_CLI_EXIT_USAGE_ERROR;
}

static sxml_cli_exit_code sxml_cli_report_internal_error(
    sxml_cli_session *session, sxml_status status) {
    sxml_cli_write_diagnostic_prefix(
        &session->standard_error,
        (sxml_cli_help_request){.program = session->program, .command = NULL});
    sxml_cli_writer_write(&session->standard_error,
                          SXML_TEXT("internal error: "));
    sxml_cli_writer_write_line(&session->standard_error,
                               sxml_status_name(status));
    return SXML_CLI_EXIT_INTERNAL_ERROR;
}

/* Parses and runs command with the arguments after its name. */
static sxml_cli_exit_code sxml_cli_run_command(
    sxml_cli_session *session, const sxml_cli_command *command,
    sxml_cli_argument_list argument_list) {
    sxml_cli_arguments *arguments = NULL;
    sxml_status status = sxml_cli_arguments_create(
        session->request->allocator,
        &(sxml_cli_arguments_configuration){
            .command = command,
            .argument_list = argument_list,
            .stops_at_first_positional = false,
        },
        &arguments);
    if (status != SXML_STATUS_OK) {
        return sxml_cli_report_internal_error(session, status);
    }
    sxml_cli_parse_outcome outcome = {
        .kind = SXML_CLI_PARSE_OUTCOME_PARSED,
        .usage_error =
            sxml_cli_usage_error_without_details(SXML_CLI_USAGE_ERROR_MISSING_COMMAND),
    };
    status = sxml_cli_arguments_parse(arguments, &outcome);
    sxml_cli_exit_code exit_code = SXML_CLI_EXIT_SUCCESS;
    if (status != SXML_STATUS_OK) {
        exit_code = sxml_cli_report_internal_error(session, status);
    } else {
        switch (outcome.kind) {
        case SXML_CLI_PARSE_OUTCOME_HELP_REQUESTED:
            sxml_cli_write_help(&session->standard_output,
                                (sxml_cli_help_request){
                                    .program = session->program,
                                    .command = command,
                                });
            exit_code = SXML_CLI_EXIT_SUCCESS;
            break;
        case SXML_CLI_PARSE_OUTCOME_USAGE_ERROR:
            exit_code = sxml_cli_report_usage_error(session, command,
                                                    outcome.usage_error);
            break;
        case SXML_CLI_PARSE_OUTCOME_PARSED: {
            const sxml_cli_invocation invocation = {
                .program = session->program,
                .command = command,
                .arguments = arguments,
                .standard_output = &session->standard_output,
                .standard_error = &session->standard_error,
                .allocator = session->request->allocator,
                .version_text = session->request->version_text,
                .file_reader = session->request->file_reader,
                .file_writer = session->request->file_writer,
                .working_directory = session->request->working_directory,
                .file_system = session->request->file_system,
            };
            exit_code = command->run(&invocation);
            break;
        }
        }
    }
    sxml_cli_arguments_destroy(arguments);
    return exit_code;
}

static sxml_cli_exit_code sxml_cli_write_version(sxml_cli_session *session) {
    sxml_cli_writer_write(&session->standard_output, session->program->name);
    sxml_cli_writer_write_byte(&session->standard_output, ' ');
    sxml_cli_writer_write_line(&session->standard_output,
                               session->request->version_text);
    return SXML_CLI_EXIT_SUCCESS;
}

/* Acts on the parsed arguments before the command name. */
static sxml_cli_exit_code sxml_cli_dispatch(sxml_cli_session *session,
                                            const sxml_cli_arguments *arguments) {
    sxml_cli_option_match version_match = {
        .is_present = false,
        .occurrence_count = 0,
        .value = {.bytes = NULL, .byte_count = 0},
    };
    const sxml_status status = sxml_cli_arguments_find_option(
        arguments,
        (sxml_cli_option_lookup){.long_name = SXML_TEXT("version"),
                                 .occurrence_index = 0},
        &version_match);
    if (status != SXML_STATUS_OK) {
        return sxml_cli_report_internal_error(session, status);
    }
    sxml_byte_view command_name = {.bytes = NULL, .byte_count = 0};
    const bool has_command =
        sxml_cli_arguments_get_positional(arguments, 0, &command_name) ==
        SXML_STATUS_OK;
    if (version_match.is_present) {
        if (has_command) {
            sxml_cli_usage_error usage_error =
                sxml_cli_usage_error_without_details(
                    SXML_CLI_USAGE_ERROR_UNEXPECTED_POSITIONAL);
            usage_error.argument = command_name;
            return sxml_cli_report_usage_error(session, NULL, usage_error);
        }
        return sxml_cli_write_version(session);
    }
    if (!has_command) {
        return sxml_cli_report_usage_error(
            session, NULL,
            sxml_cli_usage_error_without_details(
                SXML_CLI_USAGE_ERROR_MISSING_COMMAND));
    }
    const sxml_cli_command *command = NULL;
    if (!sxml_cli_program_find_command(session->program, command_name,
                                       &command)) {
        return sxml_cli_report_usage_error(
            session, NULL,
            sxml_cli_unknown_command_error(session->program, command_name));
    }
    /* The command name is the first positional; in POSIX order everything
       after it is the command's. The options before it are flags, so it is
       the first argument that does not start with "-", or the one after
       "--". */
    const sxml_cli_argument_list all_arguments = session->request->argument_list;
    size_t command_argument_index = 0;
    for (;; command_argument_index += 1) {
        const sxml_byte_view argument =
            all_arguments.arguments[command_argument_index];
        if (sxml_byte_view_equals(argument, SXML_TEXT("--"))) {
            command_argument_index += 1;
            break;
        }
        if (argument.byte_count < 2 || argument.bytes[0] != '-') {
            break;
        }
    }
    return sxml_cli_run_command(
        session, command,
        (sxml_cli_argument_list){
            .arguments = all_arguments.arguments + command_argument_index + 1,
            .argument_count =
                all_arguments.argument_count - command_argument_index - 1,
        });
}

static sxml_cli_exit_code sxml_cli_run_program(sxml_cli_session *session) {
    sxml_cli_table_problem problem = {
        .description = {.bytes = NULL, .byte_count = 0},
        .command_name = {.bytes = NULL, .byte_count = 0},
        .item_name = {.bytes = NULL, .byte_count = 0},
    };
    sxml_status status = sxml_cli_program_validate(session->program, &problem);
    if (status != SXML_STATUS_OK) {
        sxml_cli_writer *standard_error = &session->standard_error;
        sxml_cli_write_diagnostic_prefix(
            standard_error,
            (sxml_cli_help_request){.program = session->program,
                                    .command = NULL});
        sxml_cli_writer_write(standard_error,
                              SXML_TEXT("internal error: invalid command table: "));
        sxml_cli_writer_write(standard_error, problem.description);
        if (problem.command_name.byte_count > 0) {
            sxml_cli_writer_write(standard_error, SXML_TEXT(" (command "));
            sxml_cli_writer_write_quoted(standard_error, problem.command_name);
            if (problem.item_name.byte_count > 0) {
                sxml_cli_writer_write(standard_error, SXML_TEXT(", "));
                sxml_cli_writer_write_quoted(standard_error, problem.item_name);
            }
            sxml_cli_writer_write_byte(standard_error, ')');
        }
        sxml_cli_writer_write_byte(standard_error, '\n');
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_cli_arguments *arguments = NULL;
    status = sxml_cli_arguments_create(
        session->request->allocator,
        &(sxml_cli_arguments_configuration){
            .command = &sxml_cli_root_parse_command,
            .argument_list = session->request->argument_list,
            .stops_at_first_positional = true,
        },
        &arguments);
    if (status != SXML_STATUS_OK) {
        return sxml_cli_report_internal_error(session, status);
    }
    sxml_cli_parse_outcome outcome = {
        .kind = SXML_CLI_PARSE_OUTCOME_PARSED,
        .usage_error =
            sxml_cli_usage_error_without_details(SXML_CLI_USAGE_ERROR_MISSING_COMMAND),
    };
    status = sxml_cli_arguments_parse(arguments, &outcome);
    sxml_cli_exit_code exit_code = SXML_CLI_EXIT_SUCCESS;
    if (status != SXML_STATUS_OK) {
        exit_code = sxml_cli_report_internal_error(session, status);
    } else {
        switch (outcome.kind) {
        case SXML_CLI_PARSE_OUTCOME_HELP_REQUESTED:
            sxml_cli_write_help(&session->standard_output,
                                (sxml_cli_help_request){
                                    .program = session->program,
                                    .command = NULL,
                                });
            exit_code = SXML_CLI_EXIT_SUCCESS;
            break;
        case SXML_CLI_PARSE_OUTCOME_USAGE_ERROR:
            exit_code =
                sxml_cli_report_usage_error(session, NULL, outcome.usage_error);
            break;
        case SXML_CLI_PARSE_OUTCOME_PARSED:
            exit_code = sxml_cli_dispatch(session, arguments);
            break;
        }
    }
    sxml_cli_arguments_destroy(arguments);
    return exit_code;
}

sxml_cli_exit_code sxml_cli_run(const sxml_cli_program *program,
                                const sxml_cli_run_request *request) {
    sxml_cli_session session = {
        .program = program,
        .request = request,
        .standard_output = sxml_cli_writer_from_stream(request->standard_output),
        .standard_error = sxml_cli_writer_from_stream(request->standard_error),
    };
    sxml_cli_exit_code exit_code = sxml_cli_run_program(&session);
    const bool output_failed =
        session.standard_output.status != SXML_STATUS_OK ||
        session.standard_error.status != SXML_STATUS_OK;
    const bool is_overridable = exit_code == SXML_CLI_EXIT_SUCCESS ||
                                exit_code == SXML_CLI_EXIT_OPERATION_FAILED;
    if (output_failed && is_overridable) {
        exit_code = SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (session.standard_output.status != SXML_STATUS_OK) {
        sxml_cli_write_diagnostic_prefix(
            &session.standard_error,
            (sxml_cli_help_request){.program = program, .command = NULL});
        sxml_cli_writer_write_line(
            &session.standard_error,
            SXML_TEXT("cannot write to standard output"));
    }
    return exit_code;
}
