/*
 * Tests for parsing command lines against a command table
 * (apps/cli_framework/cli_arguments.h).
 */
#include "cli_arguments.h"
#include "sxml/memory.h"
#include "test_runner.h"

#include <stdalign.h>
#include <stdint.h>

#define TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view test_formats[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option test_options[] = {
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("quiet"),
     .short_name = 'q',
     .value_kind = SXML_CLI_VALUE_KIND_NONE,
     .value_name = SXML_BYTE_VIEW_INITIALIZER(""),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Write nothing")},
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("verbose"),
     .short_name = 'v',
     .value_kind = SXML_CLI_VALUE_KIND_NONE,
     .value_name = SXML_BYTE_VIEW_INITIALIZER(""),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_REPEATABLE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Write more")},
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("output"),
     .short_name = 'o',
     .value_kind = SXML_CLI_VALUE_KIND_TEXT,
     .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Write to FILE")},
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("format"),
     .short_name = 0,
     .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
     .value_name = SXML_BYTE_VIEW_INITIALIZER("FORMAT"),
     .choices = {.texts = test_formats, .text_count = 2},
     .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
     .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Output format")},
    {.long_name = SXML_BYTE_VIEW_INITIALIZER("param"),
     .short_name = 0,
     .value_kind = SXML_CLI_VALUE_KIND_TEXT,
     .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME=VALUE"),
     .choices = {.texts = NULL, .text_count = 0},
     .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
     .multiplicity = SXML_CLI_OPTION_REPEATABLE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("Bind a parameter")},
};

static const sxml_cli_positional test_positionals[] = {
    {.name = SXML_BYTE_VIEW_INITIALIZER("INPUT"),
     .multiplicity = SXML_CLI_POSITIONAL_EXACTLY_ONE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("The input")},
    {.name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
     .multiplicity = SXML_CLI_POSITIONAL_ZERO_OR_MORE,
     .summary = SXML_BYTE_VIEW_INITIALIZER("More files")},
};

static sxml_cli_exit_code test_run_nothing(
    const sxml_cli_invocation *invocation) {
    (void)invocation;
    return SXML_CLI_EXIT_SUCCESS;
}

/* "test [OPTIONS] INPUT [FILE...]" */
static const sxml_cli_command test_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("test"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("A test command"),
    .description = SXML_BYTE_VIEW_INITIALIZER(""),
    .options = test_options,
    .option_count = SXML_TEST_COUNT(test_options),
    .positionals = test_positionals,
    .positional_count = SXML_TEST_COUNT(test_positionals),
    .examples = {.texts = NULL, .text_count = 0},
    .run = test_run_nothing,
};

/* "fixed [OPTIONS] INPUT": no open-ended positional. */
static const sxml_cli_command test_fixed_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("fixed"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("A test command"),
    .description = SXML_BYTE_VIEW_INITIALIZER(""),
    .options = test_options,
    .option_count = SXML_TEST_COUNT(test_options),
    .positionals = test_positionals,
    .positional_count = 1,
    .examples = {.texts = NULL, .text_count = 0},
    .run = test_run_nothing,
};

enum { SXML_TEST_STORAGE_BYTE_COUNT = 4096, SXML_TEST_MAXIMUM_ARGUMENT_COUNT = 16 };

/* One parse, in caller memory. */
typedef struct parsed_command_line {
    alignas(16) uint8_t storage[SXML_TEST_STORAGE_BYTE_COUNT];
    sxml_byte_view argument_views[SXML_TEST_MAXIMUM_ARGUMENT_COUNT];
    sxml_cli_arguments *arguments;
    sxml_cli_parse_outcome outcome;
} parsed_command_line;

/* The command line to parse: a NULL-terminated array of C strings. */
typedef struct test_command_line {
    const sxml_cli_command *command;
    const char *const *texts;
    bool stops_at_first_positional;
} test_command_line;

static bool parse_command_line(parsed_command_line *parsed,
                               test_command_line command_line) {
    size_t argument_count = 0;
    while (command_line.texts[argument_count] != NULL &&
           argument_count < SXML_TEST_MAXIMUM_ARGUMENT_COUNT) {
        parsed->argument_views[argument_count] =
            sxml_byte_view_from_c_string(command_line.texts[argument_count]);
        argument_count += 1;
    }
    sxml_status status = sxml_cli_arguments_initialize(
        (sxml_memory_view){.bytes = parsed->storage,
                           .byte_count = sizeof parsed->storage},
        &(sxml_cli_arguments_configuration){
            .command = command_line.command,
            .argument_list = {.arguments = parsed->argument_views,
                              .argument_count = argument_count},
            .stops_at_first_positional = command_line.stops_at_first_positional,
        },
        &parsed->arguments);
    if (status != SXML_STATUS_OK) {
        return false;
    }
    status = sxml_cli_arguments_parse(parsed->arguments, &parsed->outcome);
    return status == SXML_STATUS_OK;
}

static bool parse_test_command(parsed_command_line *parsed,
                               const char *const *texts) {
    return parse_command_line(parsed,
                              (test_command_line){.command = &test_command,
                                                  .texts = texts,
                                                  .stops_at_first_positional =
                                                      false});
}

/* The value of an option occurrence, or "<absent>". */
typedef struct option_query {
    const char *long_name;
    size_t occurrence_index;
} option_query;

static sxml_cli_option_match find_option(const parsed_command_line *parsed,
                                         option_query query) {
    sxml_cli_option_match match = {
        .is_present = false,
        .occurrence_count = SIZE_MAX,
        .value = TEXT("<lookup failed>"),
    };
    const sxml_status status = sxml_cli_arguments_find_option(
        parsed->arguments,
        (sxml_cli_option_lookup){
            .long_name = sxml_byte_view_from_c_string(query.long_name),
            .occurrence_index = query.occurrence_index},
        &match);
    (void)status;
    return match;
}

static bool option_value_is(const parsed_command_line *parsed,
                            const char *long_name, sxml_byte_view expected) {
    const sxml_cli_option_match match = find_option(
        parsed, (option_query){.long_name = long_name, .occurrence_index = 0});
    return match.is_present && sxml_byte_view_equals(match.value, expected);
}

static bool option_is_absent(const parsed_command_line *parsed,
                             const char *long_name) {
    const sxml_cli_option_match match = find_option(
        parsed, (option_query){.long_name = long_name, .occurrence_index = 0});
    return !match.is_present && match.occurrence_count == 0;
}

static bool positional_is(const parsed_command_line *parsed,
                          size_t positional_index, sxml_byte_view expected) {
    sxml_byte_view positional = TEXT("<missing>");
    const sxml_status status = sxml_cli_arguments_get_positional(
        parsed->arguments, positional_index, &positional);
    return status == SXML_STATUS_OK &&
           sxml_byte_view_equals(positional, expected);
}

static bool is_parsed(const parsed_command_line *parsed) {
    return parsed->outcome.kind == SXML_CLI_PARSE_OUTCOME_PARSED;
}

static bool is_usage_error(const parsed_command_line *parsed,
                           sxml_cli_usage_error_kind kind) {
    return parsed->outcome.kind == SXML_CLI_PARSE_OUTCOME_USAGE_ERROR &&
           parsed->outcome.usage_error.kind == kind;
}

static bool is_help_request(const parsed_command_line *parsed) {
    return parsed->outcome.kind == SXML_CLI_PARSE_OUTCOME_HELP_REQUESTED;
}

/* ---------------------------------------------------------- Valid input */

static void test_long_option_forms(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--quiet", "--output=out.xml",
                                        "--format", "json", "in.xml", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "quiet", TEXT("")));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "output", TEXT("out.xml")));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "format", TEXT("json")));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 0, TEXT("in.xml")));
    SXML_TEST_EXPECT(state,
                     sxml_cli_arguments_positional_count(parsed.arguments) == 1);
}

static void test_value_may_start_with_dash(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--output", "-q", "-o", "--format",
                                        "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    /* "--output -q" takes "-q" as the value; "-o --format" repeats --output,
       so the parse fails on that, not on the dashes. */
    SXML_TEST_EXPECT(state,
                     is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_REPEATED_OPTION));
    static const char *const single[] = {"--output", "-q", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, single));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "output", TEXT("-q")));
    SXML_TEST_EXPECT(state, option_is_absent(&parsed, "quiet"));
}

static void test_empty_inline_value(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--output=", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "output", TEXT("")));
}

static void test_short_option_forms(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const attached[] = {"-qvofile.xml", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, attached));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "quiet", TEXT("")));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "verbose", TEXT("")));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "output", TEXT("file.xml")));

    static const char *const separate[] = {"-o", "file.xml", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, separate));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "output", TEXT("file.xml")));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 0, TEXT("in")));
}

static void test_repeatable_option_keeps_every_value(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--param=a=1", "in", "-vv",
                                        "--param", "b=2", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    const sxml_cli_option_match first = find_option(
        &parsed, (option_query){.long_name = "param", .occurrence_index = 0});
    const sxml_cli_option_match second = find_option(
        &parsed, (option_query){.long_name = "param", .occurrence_index = 1});
    const sxml_cli_option_match third = find_option(
        &parsed, (option_query){.long_name = "param", .occurrence_index = 2});
    SXML_TEST_EXPECT(state, first.is_present && first.occurrence_count == 2);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(first.value, TEXT("a=1")));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(second.value, TEXT("b=2")));
    SXML_TEST_EXPECT(state, !third.is_present && third.value.byte_count == 0);
    const sxml_cli_option_match verbose = find_option(
        &parsed, (option_query){.long_name = "verbose", .occurrence_index = 0});
    SXML_TEST_EXPECT(state, verbose.occurrence_count == 2);
}

static void test_absent_option_gives_default(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    const sxml_cli_option_match format = find_option(
        &parsed, (option_query){.long_name = "format", .occurrence_index = 0});
    SXML_TEST_EXPECT(state, !format.is_present);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(format.value, TEXT("text")));
    SXML_TEST_EXPECT(state, option_is_absent(&parsed, "output"));
}

static void test_undeclared_option_lookup_fails(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    sxml_cli_option_match match = {.is_present = true,
                                   .occurrence_count = 9,
                                   .value = TEXT("untouched")};
    SXML_TEST_EXPECT(state, sxml_cli_arguments_find_option(
                                parsed.arguments,
                                (sxml_cli_option_lookup){
                                    .long_name = TEXT("formt"),
                                    .occurrence_index = 0},
                                &match) == SXML_STATUS_INVALID_ARGUMENT);
    SXML_TEST_EXPECT(state, match.occurrence_count == 9);
    sxml_byte_view positional = TEXT("untouched");
    SXML_TEST_EXPECT(state, sxml_cli_arguments_get_positional(
                                parsed.arguments, 1, &positional) ==
                                SXML_STATUS_INDEX_OUT_OF_RANGE);
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(positional, TEXT("untouched")));
}

static void test_double_dash_ends_options(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-q", "--", "--format=xml", "-v",
                                        "--", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "quiet", TEXT("")));
    SXML_TEST_EXPECT(state, option_is_absent(&parsed, "verbose"));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 0, TEXT("--format=xml")));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 1, TEXT("-v")));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 2, TEXT("--")));
}

static void test_dash_and_empty_are_positional(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-", "", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 0, TEXT("-")));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 1, TEXT("")));
}

static void test_options_may_follow_positionals(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"in", "more", "-q", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "quiet", TEXT("")));
    SXML_TEST_EXPECT(state,
                     sxml_cli_arguments_positional_count(parsed.arguments) == 2);
}

static void test_posix_order_stops_at_first_positional(
    sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-q", "in", "-v", "--help", NULL};
    SXML_TEST_REQUIRE(state,
                      parse_command_line(&parsed,
                                         (test_command_line){
                                             .command = &test_command,
                                             .texts = texts,
                                             .stops_at_first_positional = true,
                                         }));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "quiet", TEXT("")));
    SXML_TEST_EXPECT(state, option_is_absent(&parsed, "verbose"));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 1, TEXT("-v")));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 2, TEXT("--help")));
}

static void test_parsing_again_starts_over(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-q", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, sxml_cli_arguments_parse(parsed.arguments,
                                                      &parsed.outcome) ==
                                 SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    const sxml_cli_option_match quiet = find_option(
        &parsed, (option_query){.long_name = "quiet", .occurrence_index = 0});
    SXML_TEST_EXPECT(state, quiet.occurrence_count == 1);
}

/* ---------------------------------------------------------------- Help */

static void test_help_is_found_anywhere(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const long_help[] = {"in", "--help", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, long_help));
    SXML_TEST_EXPECT(state, is_help_request(&parsed));
    static const char *const clustered[] = {"-qh", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, clustered));
    SXML_TEST_EXPECT(state, is_help_request(&parsed));
    /* No INPUT: help still wins over the missing positional. */
    static const char *const alone[] = {"-h", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, alone));
    SXML_TEST_EXPECT(state, is_help_request(&parsed));
}

static void test_help_wins_over_earlier_errors(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--bogus", "--format=xml", "-h", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_EXPECT(state, is_help_request(&parsed));
}

static void test_help_after_double_dash_is_positional(
    sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--", "--help", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, positional_is(&parsed, 0, TEXT("--help")));
    static const char *const after_error[] = {"--bogus", "--", "--help", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, after_error));
    SXML_TEST_EXPECT(state,
                     is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION));
}

static void test_help_as_option_value_is_a_value(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--output", "--help", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(state, is_parsed(&parsed));
    SXML_TEST_EXPECT(state, option_value_is(&parsed, "output", TEXT("--help")));
}

static void test_help_takes_no_value(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--help=yes", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_UNEXPECTED_VALUE));
    SXML_TEST_EXPECT(state, parsed.outcome.usage_error.option ==
                                sxml_cli_help_option());
}

/* --------------------------------------------------------- Usage errors */

static void test_unknown_long_option_is_suggested(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--fromat=json", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION));
    const sxml_cli_usage_error *usage_error = &parsed.outcome.usage_error;
    SXML_TEST_EXPECT(state,
                     sxml_byte_view_equals(usage_error->argument, TEXT("fromat")));
    SXML_TEST_EXPECT(state, !usage_error->is_short_option);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(usage_error->suggestion,
                                                  TEXT("format")));
}

static void test_abbreviations_are_not_accepted(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--out=x", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                parsed.outcome.usage_error.suggestion,
                                TEXT("output")));
}

static void test_unknown_short_option(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-qx", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_UNKNOWN_OPTION));
    SXML_TEST_EXPECT(state, parsed.outcome.usage_error.is_short_option);
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                parsed.outcome.usage_error.argument, TEXT("x")));
}

static void test_missing_value(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const long_form[] = {"in", "--output", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, long_form));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_MISSING_VALUE));
    SXML_TEST_EXPECT(state,
                     parsed.outcome.usage_error.option == &test_options[2]);
    static const char *const short_form[] = {"in", "-qo", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, short_form));
    SXML_TEST_EXPECT(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_MISSING_VALUE));
}

static void test_flag_with_value(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--quiet=yes", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_UNEXPECTED_VALUE));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                parsed.outcome.usage_error.argument, TEXT("yes")));
}

static void test_invalid_choice_is_suggested(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--format=JSON", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_INVALID_CHOICE));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                parsed.outcome.usage_error.suggestion,
                                TEXT("json")));
    static const char *const empty[] = {"--format=", "in", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, empty));
    SXML_TEST_EXPECT(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_INVALID_CHOICE));
}

static void test_repeated_single_option(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-q", "in", "--quiet", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_REPEATED_OPTION));
    SXML_TEST_EXPECT(state,
                     parsed.outcome.usage_error.option == &test_options[0]);
}

static void test_missing_positional(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"-q", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_REQUIRE(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_MISSING_POSITIONAL));
    SXML_TEST_EXPECT(state,
                     parsed.outcome.usage_error.positional == &test_positionals[0]);
}

static void test_unexpected_positional(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"in", "extra", "more", NULL};
    SXML_TEST_REQUIRE(state,
                      parse_command_line(&parsed,
                                         (test_command_line){
                                             .command = &test_fixed_command,
                                             .texts = texts,
                                             .stops_at_first_positional = false,
                                         }));
    SXML_TEST_REQUIRE(state, is_usage_error(
                                 &parsed,
                                 SXML_CLI_USAGE_ERROR_UNEXPECTED_POSITIONAL));
    SXML_TEST_EXPECT(state, sxml_byte_view_equals(
                                parsed.outcome.usage_error.argument,
                                TEXT("extra")));
}

static void test_first_error_is_reported(sxml_test_state *state) {
    parsed_command_line parsed;
    static const char *const texts[] = {"--format=xml", "--bogus", NULL};
    SXML_TEST_REQUIRE(state, parse_test_command(&parsed, texts));
    SXML_TEST_EXPECT(
        state, is_usage_error(&parsed, SXML_CLI_USAGE_ERROR_INVALID_CHOICE));
}

/* ------------------------------------------------------------ Lifecycle */

static void test_requirements_grow_with_arguments(sxml_test_state *state) {
    const sxml_byte_view short_list[] = {TEXT("a")};
    const sxml_byte_view long_list[] = {TEXT("a"), TEXT("-qvqvqvqv")};
    sxml_memory_requirements small = {.byte_count = 0, .alignment = 1};
    sxml_memory_requirements large = {.byte_count = 0, .alignment = 1};
    SXML_TEST_REQUIRE(state,
                      sxml_cli_arguments_memory_requirements(
                          &(sxml_cli_arguments_configuration){
                              .command = &test_command,
                              .argument_list = {.arguments = short_list,
                                                .argument_count = 1},
                              .stops_at_first_positional = false},
                          &small) == SXML_STATUS_OK);
    SXML_TEST_REQUIRE(state,
                      sxml_cli_arguments_memory_requirements(
                          &(sxml_cli_arguments_configuration){
                              .command = &test_command,
                              .argument_list = {.arguments = long_list,
                                                .argument_count = 2},
                              .stops_at_first_positional = false},
                          &large) == SXML_STATUS_OK);
    SXML_TEST_EXPECT(state, large.byte_count > small.byte_count);
}

static void test_initialize_rejects_small_memory(sxml_test_state *state) {
    alignas(16) uint8_t storage[8];
    const sxml_byte_view list[] = {TEXT("in")};
    sxml_cli_arguments *arguments = NULL;
    SXML_TEST_EXPECT(state, sxml_cli_arguments_initialize(
                                (sxml_memory_view){.bytes = storage,
                                                   .byte_count = sizeof storage},
                                &(sxml_cli_arguments_configuration){
                                    .command = &test_command,
                                    .argument_list = {.arguments = list,
                                                      .argument_count = 1},
                                    .stops_at_first_positional = false},
                                &arguments) == SXML_STATUS_INSUFFICIENT_MEMORY);
    SXML_TEST_EXPECT(state, arguments == NULL);
}

static void test_create_and_destroy_release_everything(
    sxml_test_state *state) {
    const sxml_byte_view list[] = {TEXT("-qv"), TEXT("in")};
    const sxml_cli_arguments_configuration configuration = {
        .command = &test_command,
        .argument_list = {.arguments = list, .argument_count = 2},
        .stops_at_first_positional = false,
    };
    /* failing_allocation_number 1 fails the only allocation; 0 fails none. */
    for (uint64_t failing_number = 0; failing_number <= 1;
         failing_number += 1) {
        sxml_counting_allocator *counting_allocator = NULL;
        SXML_TEST_REQUIRE(state,
                          sxml_counting_allocator_create(
                              sxml_default_allocator(),
                              &(sxml_counting_allocator_configuration){
                                  .backing_allocator = sxml_default_allocator(),
                                  .failing_allocation_number = failing_number},
                              &counting_allocator) == SXML_STATUS_OK);
        sxml_cli_arguments *arguments = NULL;
        const sxml_status status = sxml_cli_arguments_create(
            sxml_counting_allocator_as_allocator(counting_allocator),
            &configuration, &arguments);
        if (failing_number == 1) {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OUT_OF_MEMORY);
            SXML_TEST_EXPECT(state, arguments == NULL);
        } else {
            SXML_TEST_EXPECT(state, status == SXML_STATUS_OK);
            sxml_cli_parse_outcome outcome;
            SXML_TEST_EXPECT(state, sxml_cli_arguments_parse(arguments,
                                                             &outcome) ==
                                        SXML_STATUS_OK);
            SXML_TEST_EXPECT(state,
                             outcome.kind == SXML_CLI_PARSE_OUTCOME_PARSED);
        }
        sxml_cli_arguments_destroy(arguments);
        SXML_TEST_EXPECT(
            state, sxml_counting_allocator_read_counts(counting_allocator)
                           .live_allocation_count == 0);
        sxml_counting_allocator_destroy(counting_allocator);
    }
    sxml_cli_arguments_destroy(NULL);
    sxml_cli_arguments_finalize(NULL);
}

static const sxml_test_case cli_arguments_test_cases[] = {
    {.name = "long_option_forms", .function = test_long_option_forms},
    {.name = "value_may_start_with_dash",
     .function = test_value_may_start_with_dash},
    {.name = "empty_inline_value", .function = test_empty_inline_value},
    {.name = "short_option_forms", .function = test_short_option_forms},
    {.name = "repeatable_option_keeps_every_value",
     .function = test_repeatable_option_keeps_every_value},
    {.name = "absent_option_gives_default",
     .function = test_absent_option_gives_default},
    {.name = "undeclared_option_lookup_fails",
     .function = test_undeclared_option_lookup_fails},
    {.name = "double_dash_ends_options",
     .function = test_double_dash_ends_options},
    {.name = "dash_and_empty_are_positional",
     .function = test_dash_and_empty_are_positional},
    {.name = "options_may_follow_positionals",
     .function = test_options_may_follow_positionals},
    {.name = "posix_order_stops_at_first_positional",
     .function = test_posix_order_stops_at_first_positional},
    {.name = "parsing_again_starts_over",
     .function = test_parsing_again_starts_over},
    {.name = "help_is_found_anywhere", .function = test_help_is_found_anywhere},
    {.name = "help_wins_over_earlier_errors",
     .function = test_help_wins_over_earlier_errors},
    {.name = "help_after_double_dash_is_positional",
     .function = test_help_after_double_dash_is_positional},
    {.name = "help_as_option_value_is_a_value",
     .function = test_help_as_option_value_is_a_value},
    {.name = "help_takes_no_value", .function = test_help_takes_no_value},
    {.name = "unknown_long_option_is_suggested",
     .function = test_unknown_long_option_is_suggested},
    {.name = "abbreviations_are_not_accepted",
     .function = test_abbreviations_are_not_accepted},
    {.name = "unknown_short_option", .function = test_unknown_short_option},
    {.name = "missing_value", .function = test_missing_value},
    {.name = "flag_with_value", .function = test_flag_with_value},
    {.name = "invalid_choice_is_suggested",
     .function = test_invalid_choice_is_suggested},
    {.name = "repeated_single_option",
     .function = test_repeated_single_option},
    {.name = "missing_positional", .function = test_missing_positional},
    {.name = "unexpected_positional", .function = test_unexpected_positional},
    {.name = "first_error_is_reported",
     .function = test_first_error_is_reported},
    {.name = "requirements_grow_with_arguments",
     .function = test_requirements_grow_with_arguments},
    {.name = "initialize_rejects_small_memory",
     .function = test_initialize_rejects_small_memory},
    {.name = "create_and_destroy_release_everything",
     .function = test_create_and_destroy_release_everything},
};

int main(int argument_count, char **arguments) {
    return sxml_test_run_suite(
        &(sxml_test_suite){
            .name = "cli_arguments",
            .cases = cli_arguments_test_cases,
            .case_count = SXML_TEST_COUNT(cli_arguments_test_cases),
        },
        (sxml_test_command_line){
            .argument_count = argument_count,
            .arguments = arguments,
        });
}
