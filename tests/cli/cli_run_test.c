/*
 * Tests for running a program: dispatch, help, usage errors and exit codes
 * (apps/cli_framework/cli_run.h, cli_help.h, cli_command.h), and the table
 * of the sxml program (apps/sxml/program.c).
 */
#include "cli_arguments.h"
#include "cli_help.h"
#include "cli_run.h"
#include "program.h"
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdint.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* ------------------------------------------------------- Test program */

static bool has_flag(const sxml_cli_invocation *invocation,
                     sxml_byte_view long_name) {
    sxml_cli_option_match match = {.is_present = false,
                                   .occurrence_count = 0,
                                   .value = TEXT("")};
    const sxml_status status = sxml_cli_arguments_find_option(
        invocation->arguments,
        (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
        &match);
    return status == SXML_STATUS_OK && match.is_present;
}

static sxml_cli_exit_code test_run_echo(const sxml_cli_invocation *invocation) {
    if (has_flag(invocation, TEXT("misuse"))) {
        return sxml_cli_invocation_report_usage_error(
            invocation, TEXT("give TEXT or --misuse, not both"));
    }
    sxml_byte_view text = TEXT("");
    if (sxml_cli_arguments_get_positional(invocation->arguments, 0, &text) !=
        SXML_STATUS_OK) {
        text = TEXT("(nothing)");
    }
    sxml_cli_writer_write_line(invocation->standard_output, text);
    return has_flag(invocation, TEXT("fail")) ? SXML_CLI_EXIT_OPERATION_FAILED
                                               : SXML_CLI_EXIT_SUCCESS;
}

static const sxml_cli_option test_echo_options[] = {
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("fail"),
     .short_name = 'f',
     .value_kind = SXML_CLI_VALUE_KIND_NONE,
     .value_name = SXML_BYTE_VIEW_INITIALIZER(""),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Exit with code 1")},
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("misuse"),
     .short_name = 0,
     .value_kind = SXML_CLI_VALUE_KIND_NONE,
     .value_name = SXML_BYTE_VIEW_INITIALIZER(""),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Report a usage error")},
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("tag"),
     .short_name = 0,
     .value_kind = SXML_CLI_VALUE_KIND_TEXT,
     .value_name = SXML_BYTE_VIEW_INITIALIZER("TAG"),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_REPEATABLE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Add a tag")},
};

static const sxml_cli_positional test_echo_positionals[] = {
    {.name = SXML_BYTE_VIEW_INITIALIZER("TEXT"),
     .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
     .summary = SXML_BYTE_VIEW_INITIALIZER("What to write")},
};

static const sxml_byte_view test_echo_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("tool echo hello"),
};

static const sxml_cli_command test_echo_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("echo"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Write TEXT"),
    .description = SXML_BYTE_VIEW_INITIALIZER("Writes TEXT and a line feed."),
    .options = test_echo_options,
    .option_count = SXML_TEST_COUNT(test_echo_options),
    .positionals = test_echo_positionals,
    .positional_count = SXML_TEST_COUNT(test_echo_positionals),
    .examples = {.texts = test_echo_examples,
                 .text_count = SXML_TEST_COUNT(test_echo_examples)},
    .run = test_run_echo,
};

static const sxml_cli_command *const test_commands[] = {&test_echo_command};

static const sxml_byte_view test_program_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("tool echo hello"),
};

static const sxml_cli_program test_program = {
    .name = SXML_BYTE_VIEW_INITIALIZER("tool"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("a test program"),
    .description = SXML_BYTE_VIEW_INITIALIZER("Writes texts."),
    .commands = test_commands,
    .command_count = SXML_TEST_COUNT(test_commands),
    .examples = {.texts = test_program_examples,
                 .text_count = SXML_TEST_COUNT(test_program_examples)},
    .bug_report_location = SXML_BYTE_VIEW_INITIALIZER("https://example.org/bugs"),
};

/* ------------------------------------------------------------- Harness */

enum {
    SXML_TEST_STREAM_CAPACITY = 2048,
    SXML_TEST_STORAGE_BYTE_COUNT = 2304,
    SXML_TEST_MAXIMUM_ARGUMENT_COUNT = 8
};

typedef struct captured_stream {
    alignas(16) uint8_t storage[SXML_TEST_STORAGE_BYTE_COUNT];
    sxml_cli_memory_stream *memory_stream;
} captured_stream;

/* One run of a program with captured output and counted memory. */
typedef struct program_run {
    captured_stream standard_output;
    captured_stream standard_error;
    sxml_cli_exit_code exit_code;
    uint64_t live_allocation_count;
} program_run;

/* What to run: a program, its arguments (NULL-terminated), and failures. */
typedef struct run_request {
    const sxml_cli_program *program;
    const char *const *texts;
    /* Capacity of the captured standard output; 0 makes every write fail. */
    size_t standard_output_capacity;
    uint64_t failing_allocation_number;
} run_request;

static bool capture_stream(captured_stream *stream, size_t capacity) {
    return sxml_cli_memory_stream_initialize(
               (sxml_memory_view){.bytes = stream->storage,
                                  .byte_count = sizeof stream->storage},
               &(sxml_cli_memory_stream_configuration){
                   .capacity_byte_count = capacity},
               &stream->memory_stream) == SXML_STATUS_OK;
}

static bool run_program(program_run *run, run_request request) {
    sxml_byte_view argument_views[SXML_TEST_MAXIMUM_ARGUMENT_COUNT];
    size_t argument_count = 0;
    while (request.texts[argument_count] != NULL &&
           argument_count < SXML_TEST_MAXIMUM_ARGUMENT_COUNT) {
        argument_views[argument_count] =
            sxml_byte_view_from_c_string(request.texts[argument_count]);
        argument_count += 1;
    }
    if (!capture_stream(&run->standard_output,
                        request.standard_output_capacity) ||
        !capture_stream(&run->standard_error, SXML_TEST_STREAM_CAPACITY)) {
        return false;
    }
    sxml_counting_allocator *counting_allocator = NULL;
    if (sxml_counting_allocator_create(
            sxml_default_allocator(),
            &(sxml_counting_allocator_configuration){
                .backing_allocator = sxml_default_allocator(),
                .failing_allocation_number = request.failing_allocation_number},
            &counting_allocator) != SXML_STATUS_OK) {
        return false;
    }
    run->exit_code = sxml_cli_run(
        request.program,
        &(sxml_cli_run_request){
            .argument_list = {.arguments = argument_views,
                              .argument_count = argument_count},
            .standard_output = sxml_cli_memory_stream_as_output_stream(
                run->standard_output.memory_stream),
            .standard_error = sxml_cli_memory_stream_as_output_stream(
                run->standard_error.memory_stream),
            .allocator = sxml_counting_allocator_as_allocator(counting_allocator),
            .version_text = TEXT("1.2.3"),
        });
    run->live_allocation_count =
        sxml_counting_allocator_read_counts(counting_allocator)
            .live_allocation_count;
    sxml_counting_allocator_destroy(counting_allocator);
    return true;
}

static bool run_test_program(program_run *run, const char *const *texts) {
    return run_program(run, (run_request){
                                .program = &test_program,
                                .texts = texts,
                                .standard_output_capacity = SXML_TEST_STREAM_CAPACITY,
                                .failing_allocation_number = 0,
                            });
}

static bool output_is(const captured_stream *stream, sxml_byte_view expected) {
    return sxml_byte_view_equals(
        sxml_cli_memory_stream_contents(stream->memory_stream), expected);
}

/* ---------------------------------------------------------------- Help */

static const sxml_byte_view expected_program_help = SXML_BYTE_VIEW_INITIALIZER(
    "tool - a test program\n"
    "\n"
    "Usage: tool [--version] COMMAND [ARGUMENT...]\n"
    "\n"
    "Writes texts.\n"
    "\n"
    "Commands:\n"
    "  help  Show help for the program or for one command\n"
    "  echo  Write TEXT\n"
    "\n"
    "Options:\n"
    "  -h, --help     Show this help and exit\n"
    "      --version  Print the version and exit\n"
    "\n"
    "Examples:\n"
    "  tool echo hello\n"
    "\n"
    "Run 'tool help COMMAND' for help on one command.\n"
    "Report bugs at https://example.org/bugs\n");

static const sxml_byte_view expected_echo_help = SXML_BYTE_VIEW_INITIALIZER(
    "tool echo - Write TEXT\n"
    "\n"
    "Usage: tool echo [--fail] [--misuse] [--tag=TAG]... [TEXT]\n"
    "\n"
    "Writes TEXT and a line feed.\n"
    "\n"
    "Options:\n"
    "  -h, --help     Show this help and exit\n"
    "  -f, --fail     Exit with code 1\n"
    "      --misuse   Report a usage error\n"
    "      --tag=TAG  Add a tag (repeatable)\n"
    "\n"
    "Arguments:\n"
    "  TEXT  What to write\n"
    "\n"
    "Examples:\n"
    "  tool echo hello\n");

static void test_program_help_every_way(sxml_test_state *state) {
    static const char *const long_help[] = {"--help", NULL};
    static const char *const short_help[] = {"-h", NULL};
    static const char *const help_command[] = {"help", NULL};
    static const char *const *const command_lines[] = {long_help, short_help,
                                                       help_command};
    for (size_t line_index = 0; line_index < SXML_TEST_COUNT(command_lines);
         line_index += 1) {
        program_run run;
        SXML_TEST_REQUIRE(state,
                          run_test_program(&run, command_lines[line_index]));
        SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_SUCCESS);
        SXML_TEST_EXPECT(state,
                         output_is(&run.standard_output, expected_program_help));
        SXML_TEST_EXPECT(state, output_is(&run.standard_error, TEXT("")));
        SXML_TEST_EXPECT(state, run.live_allocation_count == 0);
    }
}

static void test_command_help_every_way(sxml_test_state *state) {
    static const char *const option[] = {"echo", "--help", NULL};
    static const char *const after_error[] = {"echo", "--bogus", "x", "-h",
                                              NULL};
    static const char *const help_command[] = {"help", "echo", NULL};
    static const char *const *const command_lines[] = {option, after_error,
                                                       help_command};
    for (size_t line_index = 0; line_index < SXML_TEST_COUNT(command_lines);
         line_index += 1) {
        program_run run;
        SXML_TEST_REQUIRE(state,
                          run_test_program(&run, command_lines[line_index]));
        SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_SUCCESS);
        SXML_TEST_EXPECT(state,
                         output_is(&run.standard_output, expected_echo_help));
        SXML_TEST_EXPECT(state, output_is(&run.standard_error, TEXT("")));
    }
}

static void test_version_option(sxml_test_state *state) {
    program_run run;
    static const char *const texts[] = {"--version", NULL};
    SXML_TEST_REQUIRE(state, run_test_program(&run, texts));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_SUCCESS);
    SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("tool 1.2.3\n")));
}

/* ------------------------------------------------------------ Dispatch */

static void test_command_runs_with_its_arguments(sxml_test_state *state) {
    program_run run;
    static const char *const texts[] = {"echo", "--tag=a", "hello", NULL};
    SXML_TEST_REQUIRE(state, run_test_program(&run, texts));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_SUCCESS);
    SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("hello\n")));
    SXML_TEST_EXPECT(state, output_is(&run.standard_error, TEXT("")));
    SXML_TEST_EXPECT(state, run.live_allocation_count == 0);
}

static void test_options_after_command_belong_to_it(sxml_test_state *state) {
    program_run run;
    /* "--version" after the command name is echo's, which has none. */
    static const char *const texts[] = {"echo", "--version", NULL};
    SXML_TEST_REQUIRE(state, run_test_program(&run, texts));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_USAGE_ERROR);
    SXML_TEST_EXPECT(state,
                     output_is(&run.standard_error,
                               TEXT("tool echo: unknown option '--version'\n"
                                    "Usage: tool echo [--fail] [--misuse] "
                                    "[--tag=TAG]... [TEXT]\n"
                                    "Try 'tool echo --help'.\n")));
}

static void test_operation_failure_exit_code(sxml_test_state *state) {
    program_run run;
    static const char *const texts[] = {"echo", "-f", "x", NULL};
    SXML_TEST_REQUIRE(state, run_test_program(&run, texts));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_OPERATION_FAILED);
    SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("x\n")));
}

static void test_command_reports_usage_error(sxml_test_state *state) {
    program_run run;
    static const char *const texts[] = {"echo", "--misuse", NULL};
    SXML_TEST_REQUIRE(state, run_test_program(&run, texts));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_USAGE_ERROR);
    SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("")));
    SXML_TEST_EXPECT(state,
                     output_is(&run.standard_error,
                               TEXT("tool echo: give TEXT or --misuse, not both\n"
                                    "Usage: tool echo [--fail] [--misuse] "
                                    "[--tag=TAG]... [TEXT]\n"
                                    "Try 'tool echo --help'.\n")));
}

/* -------------------------------------------------------- Usage errors */

typedef struct usage_error_case {
    const char *const *texts;
    sxml_byte_view expected_standard_error;
} usage_error_case;

static void test_usage_errors(sxml_test_state *state) {
    static const char *const nothing[] = {NULL};
    static const char *const typo[] = {"ecoh", NULL};
    static const char *const program_option[] = {"--verison", NULL};
    static const char *const version_and_command[] = {"--version", "echo",
                                                      NULL};
    static const char *const help_typo[] = {"help", "ehco", NULL};
    static const char *const control[] = {"ec\x1bho", NULL};
    static const char *const extra[] = {"echo", "a", "b", NULL};
    const usage_error_case cases[] = {
        {.texts = nothing,
         .expected_standard_error =
             TEXT("tool: no command given\n"
                  "Usage: tool [--version] COMMAND [ARGUMENT...]\n"
                  "Try 'tool --help'.\n")},
        {.texts = typo,
         .expected_standard_error =
             TEXT("tool: unknown command 'ecoh'; did you mean 'echo'?\n"
                  "Usage: tool [--version] COMMAND [ARGUMENT...]\n"
                  "Try 'tool --help'.\n")},
        {.texts = program_option,
         .expected_standard_error =
             TEXT("tool: unknown option '--verison'; did you mean "
                  "'--version'?\n"
                  "Usage: tool [--version] COMMAND [ARGUMENT...]\n"
                  "Try 'tool --help'.\n")},
        {.texts = version_and_command,
         .expected_standard_error =
             TEXT("tool: unexpected argument 'echo'\n"
                  "Usage: tool [--version] COMMAND [ARGUMENT...]\n"
                  "Try 'tool --help'.\n")},
        {.texts = help_typo,
         .expected_standard_error =
             TEXT("tool help: unknown command 'ehco'; did you mean 'echo'?\n"
                  "Usage: tool help [COMMAND]\n"
                  "Try 'tool help --help'.\n")},
        {.texts = control,
         .expected_standard_error =
             TEXT("tool: unknown command 'ec\\x1Bho'; did you mean 'echo'?\n"
                  "Usage: tool [--version] COMMAND [ARGUMENT...]\n"
                  "Try 'tool --help'.\n")},
        {.texts = extra,
         .expected_standard_error =
             TEXT("tool echo: unexpected argument 'b'\n"
                  "Usage: tool echo [--fail] [--misuse] [--tag=TAG]... [TEXT]\n"
                  "Try 'tool echo --help'.\n")},
    };
    for (size_t case_index = 0; case_index < SXML_TEST_COUNT(cases);
         case_index += 1) {
        program_run run;
        SXML_TEST_REQUIRE(state, run_test_program(&run, cases[case_index].texts));
        SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_USAGE_ERROR);
        SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("")));
        SXML_TEST_EXPECT(state,
                         output_is(&run.standard_error,
                                   cases[case_index].expected_standard_error));
        SXML_TEST_EXPECT(state, run.live_allocation_count == 0);
    }
}

/* ---------------------------------------------- Failures of the program */

static void test_failed_output_write_is_an_input_output_error(
    sxml_test_state *state) {
    program_run run;
    static const char *const texts[] = {"echo", "hello", NULL};
    SXML_TEST_REQUIRE(state, run_program(&run, (run_request){
                                                   .program = &test_program,
                                                   .texts = texts,
                                                   .standard_output_capacity = 0,
                                                   .failing_allocation_number = 0,
                                               }));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_INPUT_OUTPUT_ERROR);
    SXML_TEST_EXPECT(state,
                     output_is(&run.standard_error,
                               TEXT("tool: cannot write to standard output\n")));
}

static void test_out_of_memory_is_an_internal_error(sxml_test_state *state) {
    static const char *const texts[] = {"echo", "hello", NULL};
    /* Allocation 1 is the arguments before the command, 2 the command's. */
    for (uint64_t failing_number = 1; failing_number <= 2;
         failing_number += 1) {
        program_run run;
        SXML_TEST_REQUIRE(state,
                          run_program(&run, (run_request){
                                                .program = &test_program,
                                                .texts = texts,
                                                .standard_output_capacity =
                                                    SXML_TEST_STREAM_CAPACITY,
                                                .failing_allocation_number =
                                                    failing_number,
                                            }));
        SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_INTERNAL_ERROR);
        SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("")));
        SXML_TEST_EXPECT(state,
                         output_is(&run.standard_error,
                                   TEXT("tool: internal error: "
                                        "SXML_STATUS_OUT_OF_MEMORY\n")));
        SXML_TEST_EXPECT(state, run.live_allocation_count == 0);
    }
}

static const sxml_cli_command test_help_named_command = {
        .name = SXML_BYTE_VIEW_INITIALIZER("help"),
        .summary = SXML_BYTE_VIEW_INITIALIZER("A second help"),
        .description = SXML_BYTE_VIEW_INITIALIZER(""),
        .options = NULL,
        .option_count = 0,
        .positionals = NULL,
        .positional_count = 0,
        .examples = {.texts = NULL, .text_count = 0},
        .run = test_run_echo,
};

static const sxml_cli_command *const test_help_named_commands[] = {
    &test_help_named_command,
};

static void test_invalid_table_is_an_internal_error(sxml_test_state *state) {
    const sxml_cli_program broken_program = {
        .name = TEXT("tool"),
        .summary = TEXT("a broken program"),
        .description = TEXT(""),
        .commands = test_help_named_commands,
        .command_count = 1,
        .examples = {.texts = NULL, .text_count = 0},
        .bug_report_location = TEXT(""),
    };
    program_run run;
    static const char *const texts[] = {"help", NULL};
    SXML_TEST_REQUIRE(state, run_program(&run, (run_request){
                                                   .program = &broken_program,
                                                   .texts = texts,
                                                   .standard_output_capacity =
                                                       SXML_TEST_STREAM_CAPACITY,
                                                   .failing_allocation_number = 0,
                                               }));
    SXML_TEST_EXPECT(state, run.exit_code == SXML_CLI_EXIT_INTERNAL_ERROR);
    SXML_TEST_EXPECT(state, output_is(&run.standard_output, TEXT("")));
    SXML_TEST_EXPECT(state,
                     output_is(&run.standard_error,
                               TEXT("tool: internal error: invalid command "
                                    "table: the help command is built in "
                                    "(command 'help')\n")));
}

/* ----------------------------------------------------------- Validation */

static sxml_byte_view validation_problem(const sxml_cli_command *command) {
    const sxml_cli_command *const commands[] = {command};
    const sxml_cli_program program = {
        .name = TEXT("tool"),
        .summary = TEXT("a test program"),
        .description = TEXT(""),
        .commands = commands,
        .command_count = 1,
        .examples = {.texts = NULL, .text_count = 0},
        .bug_report_location = TEXT(""),
    };
    sxml_cli_table_problem problem = {.description = TEXT(""),
                                      .command_name = TEXT(""),
                                      .item_name = TEXT("")};
    if (sxml_cli_program_validate(&program, &problem) == SXML_STATUS_OK) {
        return TEXT("");
    }
    return problem.description;
}

static sxml_cli_command command_with_options(const sxml_cli_option *options,
                                             size_t option_count) {
    sxml_cli_command command = test_echo_command;
    command.options = options;
    command.option_count = option_count;
    return command;
}

static void test_validation_finds_broken_options(sxml_test_state *state) {
    sxml_cli_option options[2] = {test_echo_options[0], test_echo_options[1]};
    sxml_cli_command command = command_with_options(options, 2);
    SXML_TEST_EXPECT(state, validation_problem(&command).byte_count == 0);

    options[1].long_name = TEXT("fail");
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                validation_problem(&command),
                                TEXT("option long name is not unique")));
    options[1] = test_echo_options[1];
    options[1].short_name = 'f';
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                validation_problem(&command),
                                TEXT("option short name is not unique")));
    options[1].short_name = 'h';
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                validation_problem(&command),
                                TEXT("--help and -h are built in")));
    options[1].short_name = '-';
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         validation_problem(&command),
                         TEXT("short name is not an ASCII letter or digit")));
    options[1] = test_echo_options[1];
    options[1].long_name = TEXT("a=b");
    SXML_TEST_EXPECT(state, validation_problem(&command).byte_count > 0);
    options[1] = test_echo_options[1];
    options[1].value_name = TEXT("VALUE");
    SXML_TEST_EXPECT(state, validation_problem(&command).byte_count > 0);
    options[1] = test_echo_options[1];
    options[1].value_kind = SXML_CLI_VALUE_KIND_CHOICE;
    options[1].value_name = TEXT("CHOICE");
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(
                         validation_problem(&command),
                         TEXT("choices must be given exactly for choice options")));
    static const sxml_byte_view choices[] = {SXML_BYTE_VIEW_INITIALIZER("a")};
    options[1].choices =
        (sxml_cli_text_list){.texts = choices, .text_count = 1};
    options[1].default_value = TEXT("b");
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                validation_problem(&command),
                                TEXT("default value is not one of the choices")));
    options[1].default_value = TEXT("a");
    SXML_TEST_EXPECT(state, validation_problem(&command).byte_count == 0);
}

static void test_validation_finds_broken_positionals(sxml_test_state *state) {
    const sxml_cli_positional positionals[] = {
        {.name = TEXT("A"),
         .multiplicity = SXML_CLI_POSITIONAL_ZERO_OR_MORE,
         .summary = TEXT("first")},
        {.name = TEXT("B"),
         .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
         .summary = TEXT("second")},
    };
    sxml_cli_command command = test_echo_command;
    command.positionals = positionals;
    command.positional_count = 2;
    SXML_TEST_EXPECT(state, validation_problem(&command).byte_count > 0);
    command.positional_count = 1;
    SXML_TEST_EXPECT(state, validation_problem(&command).byte_count == 0);
    command.run = NULL;
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(validation_problem(&command),
                                                  TEXT("command has no function")));
}

static void test_sxml_program_table_is_valid(sxml_test_state *state) {
    sxml_cli_table_problem problem = {.description = TEXT(""),
                                      .command_name = TEXT(""),
                                      .item_name = TEXT("")};
    SXML_TEST_EXPECT(state, sxml_cli_program_validate(sxml_program(),
                                                      &problem) ==
                                SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, problem.description.byte_count == 0);
}

static const sxml_test_case cli_run_test_cases[] = {
    {.name = "program_help_every_way", .function = test_program_help_every_way},
    {.name = "command_help_every_way", .function = test_command_help_every_way},
    {.name = "version_option", .function = test_version_option},
    {.name = "command_runs_with_its_arguments",
     .function = test_command_runs_with_its_arguments},
    {.name = "options_after_command_belong_to_it",
     .function = test_options_after_command_belong_to_it},
    {.name = "operation_failure_exit_code",
     .function = test_operation_failure_exit_code},
    {.name = "command_reports_usage_error",
     .function = test_command_reports_usage_error},
    {.name = "usage_errors", .function = test_usage_errors},
    {.name = "failed_output_write_is_an_input_output_error",
     .function = test_failed_output_write_is_an_input_output_error},
    {.name = "out_of_memory_is_an_internal_error",
     .function = test_out_of_memory_is_an_internal_error},
    {.name = "invalid_table_is_an_internal_error",
     .function = test_invalid_table_is_an_internal_error},
    {.name = "validation_finds_broken_options",
     .function = test_validation_finds_broken_options},
    {.name = "validation_finds_broken_positionals",
     .function = test_validation_finds_broken_positionals},
    {.name = "sxml_program_table_is_valid",
     .function = test_sxml_program_table_is_valid},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "cli_run",
            .cases = cli_run_test_cases,
            .case_count = SXML_TEST_COUNT(cli_run_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
