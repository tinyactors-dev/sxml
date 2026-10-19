/*
 * sxml-qt4 run [--catalog=FILE] [--spec=XP40|XQ40] [--test-set=NAME]
 *              [--format=text|json] [TEST-LIST]
 *
 * Runs every selected QT4 test case (test_run.h), checks its assertions
 * (assertion.h) and writes the run report (report.h) to standard output.
 * Imitates command_list.c, whose selection it shares: the test cases of
 * TEST-LIST in its order, those of one test set, or, with neither, every
 * test case of the catalog.
 *
 * Problems (an unknown test set or test case, an unknown environment) go
 * to standard error as "sxml-qt4 run: FILE[:LINE]: description 'subject'"
 * and the run goes on.
 *
 * Exit code: 3 when a file cannot be read, else 4 when the driver failed
 * (out of memory), else 1 after a problem or when a test case failed or
 * ended in error, else 0. Wrong errors and test cases not run do not
 * count against the run.
 */
#include "catalog.h"
#include "cli_arguments.h"
#include "driver.h"
#include "report.h"
#include "test_list.h"
#include "test_run.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_qt4_empty_text = {.bytes = NULL,
                                                   .byte_count = 0};

/* The entries of a test list; the reasons of one test case. */
enum { SXML_QT4_RUN_ARENA_BLOCK_BYTE_COUNT = 16 * 1024 };

static const sxml_byte_view sxml_qt4_run_spec_choices[] = {
    SXML_BYTE_VIEW_INITIALIZER("XP40"),
    SXML_BYTE_VIEW_INITIALIZER("XQ40"),
};

static const sxml_byte_view sxml_qt4_run_format_choices[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option sxml_qt4_run_option_table[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("catalog"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value =
            SXML_BYTE_VIEW_INITIALIZER("third_party/qt4tests/catalog.xml"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The catalog.xml of the suite"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("spec"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("SPEC"),
        .choices = {.texts = sxml_qt4_run_spec_choices,
                    .text_count = sizeof sxml_qt4_run_spec_choices /
                                  sizeof sxml_qt4_run_spec_choices[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("XP40"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "The language and version the run targets"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("test-set"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("NAME"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value = SXML_BYTE_VIEW_INITIALIZER(""),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "Run every test case of one test set"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("format"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FORMAT"),
        .choices = {.texts = sxml_qt4_run_format_choices,
                    .text_count = sizeof sxml_qt4_run_format_choices /
                                  sizeof sxml_qt4_run_format_choices[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The form of the report"),
    },
};

static const sxml_cli_positional sxml_qt4_run_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("TEST-LIST"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File naming test cases, one '<test-set> <test-case>' per line"),
    },
};

static const sxml_byte_view sxml_qt4_run_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml-qt4 run tests/qt4/torture-slice.txt"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-qt4 run --test-set=fn-abs"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-qt4 run --format=json > report.json"),
};

/* The state of one run of the command. */
typedef struct sxml_qt4_run_state {
    const sxml_cli_invocation *invocation;
    sxml_qt4_catalog *catalog;
    sxml_qt4_target target;
    sxml_qt4_report report;
    /* The reasons of the test case being run; reset for each. */
    sxml_arena *reason_arena;
    bool has_problem;
    bool has_unreadable_file;
    /* Out of memory or a bug: the run stops. */
    bool has_failed;
} sxml_qt4_run_state;

/* ---------------------------------------------------------------- Problems */

static void sxml_qt4_run_write_problem(const sxml_qt4_run_state *run,
                                       const sxml_qt4_problem *problem) {
    sxml_cli_writer *error = run->invocation->standard_error;
    sxml_cli_writer_write(error, run->invocation->program->name);
    sxml_cli_writer_write(error, SXML_TEXT(" run: "));
    if (problem->location.source_name.byte_count != 0) {
        sxml_cli_writer_write_escaped(error, problem->location.source_name);
        if (problem->location.line_number != 0) {
            sxml_cli_writer_write_byte(error, ':');
            sxml_cli_writer_write_decimal(error, problem->location.line_number);
            if (problem->location.column_number != 0) {
                sxml_cli_writer_write_byte(error, ':');
                sxml_cli_writer_write_decimal(error,
                                              problem->location.column_number);
            }
        }
        sxml_cli_writer_write(error, SXML_TEXT(": "));
    }
    sxml_cli_writer_write_escaped(error, problem->description);
    if (problem->subject.byte_count != 0) {
        sxml_cli_writer_write_byte(error, ' ');
        sxml_cli_writer_write_quoted(error, problem->subject);
    }
    sxml_cli_writer_write_byte(error, '\n');
}

static void sxml_qt4_run_report_failure(sxml_qt4_run_state *run,
                                        sxml_status status,
                                        sxml_qt4_problem *problem) {
    if (status == SXML_STATUS_INPUT_OUTPUT_FAILED) {
        run->has_unreadable_file = true;
    } else if (status == SXML_STATUS_MALFORMED_INPUT ||
               status == SXML_STATUS_INVALID_UTF8 ||
               status == SXML_STATUS_UNSUPPORTED_FEATURE) {
        run->has_problem = true;
    } else {
        run->has_failed = true;
    }
    if (problem->description.byte_count == 0) {
        problem->description = sxml_status_name(status);
    }
    sxml_qt4_run_write_problem(run, problem);
}

static sxml_qt4_problem sxml_qt4_run_no_problem(void) {
    return (sxml_qt4_problem){.description = sxml_qt4_empty_text,
                              .subject = sxml_qt4_empty_text,
                              .location = {.source_name = sxml_qt4_empty_text,
                                           .line_number = 0,
                                           .column_number = 0,
                                           .byte_offset = 0}};
}

/* A name the selection asked for that the catalog does not have. */
typedef struct sxml_qt4_run_unknown_name {
    sxml_byte_view description;
    sxml_byte_view name;
    sxml_byte_view source_name;
    uint32_t line_number;
} sxml_qt4_run_unknown_name;

static void sxml_qt4_run_report_unknown(sxml_qt4_run_state *run,
                                        sxml_qt4_run_unknown_name unknown) {
    run->has_problem = true;
    sxml_qt4_run_write_problem(
        run,
        &(sxml_qt4_problem){.description = unknown.description,
                            .subject = unknown.name,
                            .location = {.source_name = unknown.source_name,
                                         .line_number = unknown.line_number,
                                         .column_number = 0,
                                         .byte_offset = 0}});
}

/* ------------------------------------------------------------- Selection */

static void sxml_qt4_run_one(sxml_qt4_run_state *run, size_t test_case_index) {
    sxml_arena_reset(run->reason_arena);
    sxml_qt4_problem problem = sxml_qt4_run_no_problem();
    sxml_qt4_test_case test_case = {.name = sxml_qt4_empty_text};
    sxml_status status = sxml_qt4_catalog_read_test_case(
        run->catalog,
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &problem},
        &test_case);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_run_report_failure(run, status, &problem);
        return;
    }
    sxml_qt4_test_result result = {.outcome = SXML_QT4_TEST_OUTCOME_ERROR,
                                   .reason = sxml_qt4_empty_text};
    status = sxml_qt4_run_test_case(
        &(sxml_qt4_test_run_request){.catalog = run->catalog,
                                     .test_case = &test_case,
                                     .target = run->target,
                                     .allocator = run->invocation->allocator,
                                     .arena = run->reason_arena,
                                     .file_system = run->invocation->file_system},
        &result);
    if (status != SXML_STATUS_OK) {
        problem.description = sxml_status_name(status);
        problem.subject = test_case.name;
        sxml_qt4_run_report_failure(run, status, &problem);
        return;
    }
    size_t test_set_index = 0;
    (void)sxml_qt4_catalog_get_open_test_set(run->catalog, &test_set_index);
    sxml_qt4_report_add(&run->report,
                        &(sxml_qt4_report_entry){
                            .test_set_index = test_set_index,
                            .test_set_name = test_case.test_set_name,
                            .test_case_name = test_case.name,
                            .result = result});
}

static bool sxml_qt4_run_open_test_set(sxml_qt4_run_state *run,
                                       size_t test_set_index) {
    size_t open_index = 0;
    if (sxml_qt4_catalog_get_open_test_set(run->catalog, &open_index) &&
        open_index == test_set_index) {
        return true;
    }
    sxml_qt4_problem problem = sxml_qt4_run_no_problem();
    const sxml_status status = sxml_qt4_catalog_open_test_set(
        run->catalog,
        &(sxml_qt4_test_set_request){.test_set_index = test_set_index,
                                     .problem = &problem});
    if (status != SXML_STATUS_OK) {
        sxml_qt4_run_report_failure(run, status, &problem);
        return false;
    }
    return true;
}

static void sxml_qt4_run_test_set(sxml_qt4_run_state *run,
                                  size_t test_set_index) {
    if (!sxml_qt4_run_open_test_set(run, test_set_index)) {
        return;
    }
    const size_t test_case_count =
        sxml_qt4_catalog_test_case_count(run->catalog);
    for (size_t test_case_index = 0;
         test_case_index < test_case_count && !run->has_failed;
         test_case_index += 1) {
        sxml_qt4_run_one(run, test_case_index);
    }
}

static void sxml_qt4_run_test_list(sxml_qt4_run_state *run,
                                   sxml_byte_view list_path) {
    const sxml_allocator allocator = run->invocation->allocator;
    sxml_byte_view text = sxml_qt4_empty_text;
    sxml_status status = sxml_cli_file_reader_read(
        run->invocation->file_reader,
        &(sxml_cli_file_read_request){.path = list_path,
                                      .allocator = allocator},
        &text);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_problem problem = sxml_qt4_run_no_problem();
        problem.description = SXML_TEXT("cannot read");
        problem.subject = list_path;
        sxml_qt4_run_report_failure(run, status, &problem);
        return;
    }
    sxml_arena *arena = NULL;
    status = sxml_arena_create(
        allocator,
        &(sxml_arena_configuration){
            .block_allocator = allocator,
            .block_byte_count = SXML_QT4_RUN_ARENA_BLOCK_BYTE_COUNT,
            .maximum_block_byte_count = 0,
            .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &arena);
    sxml_qt4_problem problem = sxml_qt4_run_no_problem();
    sxml_qt4_test_list test_list = {.entries = NULL, .entry_count = 0};
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_test_list_parse(
            &(sxml_qt4_test_list_request){.text = text,
                                          .source_name = list_path,
                                          .arena = arena,
                                          .problem = &problem},
            &test_list);
    }
    if (status != SXML_STATUS_OK) {
        sxml_qt4_run_report_failure(run, status, &problem);
    }
    for (size_t entry_index = 0;
         entry_index < test_list.entry_count && !run->has_failed;
         entry_index += 1) {
        const sxml_qt4_test_list_entry *entry = &test_list.entries[entry_index];
        size_t test_set_index = 0;
        if (!sxml_qt4_catalog_find_test_set(run->catalog, entry->test_set_name,
                                            &test_set_index)) {
            sxml_qt4_run_report_unknown(
                run, (sxml_qt4_run_unknown_name){
                         .description = SXML_TEXT("unknown test set"),
                         .name = entry->test_set_name,
                         .source_name = list_path,
                         .line_number = entry->line_number});
            continue;
        }
        if (!sxml_qt4_run_open_test_set(run, test_set_index)) {
            continue;
        }
        size_t test_case_index = 0;
        if (!sxml_qt4_catalog_find_test_case(
                run->catalog, entry->test_case_name, &test_case_index)) {
            sxml_qt4_run_report_unknown(
                run, (sxml_qt4_run_unknown_name){
                         .description = SXML_TEXT("unknown test case"),
                         .name = entry->test_case_name,
                         .source_name = list_path,
                         .line_number = entry->line_number});
            continue;
        }
        sxml_qt4_run_one(run, test_case_index);
    }
    sxml_arena_destroy(arena);
    sxml_allocator_release(allocator, (void *)text.bytes);
}

/* ---------------------------------------------------------------- Command */

typedef struct sxml_qt4_run_options {
    sxml_cli_option_match catalog;
    sxml_cli_option_match spec;
    sxml_cli_option_match test_set;
    sxml_cli_option_match format;
} sxml_qt4_run_options;

static sxml_status
sxml_qt4_run_find_options(const sxml_cli_invocation *invocation,
                          sxml_qt4_run_options *options) {
    struct {
        sxml_byte_view long_name;
        sxml_cli_option_match *match;
    } const lookups[] = {
        {SXML_TEXT("catalog"), &options->catalog},
        {SXML_TEXT("spec"), &options->spec},
        {SXML_TEXT("test-set"), &options->test_set},
        {SXML_TEXT("format"), &options->format},
    };
    for (size_t lookup_index = 0;
         lookup_index < sizeof lookups / sizeof lookups[0];
         lookup_index += 1) {
        const sxml_status status = sxml_cli_arguments_find_option(
            invocation->arguments,
            (sxml_cli_option_lookup){.long_name =
                                         lookups[lookup_index].long_name,
                                     .occurrence_index = 0},
            lookups[lookup_index].match);
        if (status != SXML_STATUS_OK) {
            return status;
        }
    }
    return SXML_STATUS_OK;
}

static void sxml_qt4_run_selection(sxml_qt4_run_state *run,
                                   const sxml_qt4_run_options *options) {
    sxml_byte_view list_path = sxml_qt4_empty_text;
    if (sxml_cli_arguments_get_positional(run->invocation->arguments, 0,
                                          &list_path) == SXML_STATUS_OK) {
        sxml_qt4_run_test_list(run, list_path);
        return;
    }
    if (options->test_set.is_present) {
        size_t test_set_index = 0;
        if (!sxml_qt4_catalog_find_test_set(
                run->catalog, options->test_set.value, &test_set_index)) {
            sxml_qt4_run_report_unknown(
                run, (sxml_qt4_run_unknown_name){
                         .description = SXML_TEXT("unknown test set"),
                         .name = options->test_set.value,
                         .source_name = sxml_qt4_empty_text,
                         .line_number = 0});
            return;
        }
        sxml_qt4_run_test_set(run, test_set_index);
        return;
    }
    const size_t test_set_count = sxml_qt4_catalog_test_set_count(run->catalog);
    for (size_t test_set_index = 0;
         test_set_index < test_set_count && !run->has_failed;
         test_set_index += 1) {
        sxml_qt4_run_test_set(run, test_set_index);
    }
}

static sxml_byte_view sxml_qt4_run_test_set_name(const void *state,
                                                 size_t test_set_index) {
    sxml_qt4_test_set_entry entry = {.name = sxml_qt4_empty_text,
                                     .file_path = sxml_qt4_empty_text};
    if (sxml_qt4_catalog_get_test_set_entry(state, test_set_index, &entry) !=
        SXML_STATUS_OK) {
        return sxml_qt4_empty_text;
    }
    return entry.name;
}

static sxml_cli_exit_code
sxml_qt4_run_exit_code(const sxml_qt4_run_state *run) {
    if (run->has_unreadable_file) {
        return SXML_CLI_EXIT_INPUT_OUTPUT_ERROR;
    }
    if (run->has_failed) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    const sxml_qt4_outcome_counts *totals = &run->report.totals;
    if (run->has_problem || totals->counts[SXML_QT4_TEST_OUTCOME_FAIL] != 0 ||
        totals->counts[SXML_QT4_TEST_OUTCOME_ERROR] != 0) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_SUCCESS;
}

/* Loads the catalog, runs the selection and writes the report. */
static void sxml_qt4_run_catalog(sxml_qt4_run_state *run,
                                 const sxml_qt4_run_options *options) {
    const sxml_cli_invocation *invocation = run->invocation;
    sxml_qt4_problem problem = sxml_qt4_run_no_problem();
    sxml_status status = sxml_qt4_catalog_load(run->catalog, &problem);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_run_report_failure(run, status, &problem);
        return;
    }
    status = sxml_arena_create(
        invocation->allocator,
        &(sxml_arena_configuration){
            .block_allocator = invocation->allocator,
            .block_byte_count = SXML_QT4_RUN_ARENA_BLOCK_BYTE_COUNT,
            .maximum_block_byte_count = 0,
            .initial_memory = {.bytes = NULL, .byte_count = 0}},
        &run->reason_arena);
    if (status == SXML_STATUS_OK) {
        status = sxml_qt4_report_start(
            &(sxml_qt4_report_configuration){
                .output = invocation->standard_output,
                .format = sxml_byte_view_equals(options->format.value,
                                                SXML_TEXT("json"))
                              ? SXML_QT4_REPORT_FORMAT_JSON
                              : SXML_QT4_REPORT_FORMAT_TEXT,
                .test_set_count = sxml_qt4_catalog_test_set_count(run->catalog),
                .spec = options->spec.value,
                .allocator = invocation->allocator},
            &run->report);
    }
    if (status != SXML_STATUS_OK) {
        run->has_failed = true;
        return;
    }
    sxml_qt4_run_selection(run, options);
    sxml_qt4_report_finish(
        &run->report,
        (sxml_qt4_test_set_names){.state = run->catalog,
                                  .get_name = sxml_qt4_run_test_set_name});
}

static sxml_cli_exit_code
sxml_qt4_run_run_command(const sxml_cli_invocation *invocation) {
    const sxml_cli_option_match absent = {.is_present = false,
                                          .occurrence_count = 0,
                                          .value = sxml_qt4_empty_text};
    sxml_qt4_run_options options = {.catalog = absent,
                                    .spec = absent,
                                    .test_set = absent,
                                    .format = absent};
    if (sxml_qt4_run_find_options(invocation, &options) != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (options.test_set.is_present &&
        sxml_cli_arguments_positional_count(invocation->arguments) != 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give TEST-LIST or --test-set, not both"));
    }
    sxml_qt4_catalog *catalog = NULL;
    const sxml_status status = sxml_qt4_catalog_create(
        invocation->allocator,
        &(sxml_qt4_catalog_configuration){
            .allocator = invocation->allocator,
            .file_reader = invocation->file_reader,
            .catalog_path = options.catalog.value},
        &catalog);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_INVALID_ARGUMENT
                   ? sxml_cli_invocation_report_usage_error(
                         invocation, SXML_TEXT("--catalog must not be empty"))
                   : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_qt4_run_state run = {
        .invocation = invocation,
        .catalog = catalog,
        .target = {.language = sxml_byte_view_equals(options.spec.value,
                                                     SXML_TEXT("XQ40"))
                                   ? SXML_QT4_LANGUAGE_XQUERY
                                   : SXML_QT4_LANGUAGE_XPATH,
                   .version = 40},
        .report = {.output = NULL, .test_set_counts = NULL},
        .reason_arena = NULL,
        .has_problem = false,
        .has_unreadable_file = false,
        .has_failed = false};
    sxml_qt4_run_catalog(&run, &options);
    const sxml_cli_exit_code exit_code = sxml_qt4_run_exit_code(&run);
    sxml_qt4_report_release(&run.report);
    sxml_arena_destroy(run.reason_arena);
    sxml_qt4_catalog_destroy(catalog);
    return exit_code;
}

const sxml_cli_command sxml_qt4_run_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("run"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "Run test cases and check their assertions"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Runs each selected test case with its environment (sources,\n"
        "parameters, namespaces, static base URI, decimal formats, the\n"
        "default collation, resources, collections) in a processor of its "
        "own,\n"
        "with at most 4 x 10^8 evaluation steps and 512 MiB, and checks its\n"
        "result assertions. Writes each test case that failed, ended in\n"
        "error or raised a wrong error code, then counts per test set and\n"
        "a \"total:\" line; --format=json writes every test case, counts\n"
        "per test set and per area, and how many are in the D95 scope. The\n"
        "selection is the test cases of TEST-LIST, those of the test set\n"
        "--test-set names, or with neither every test case of the catalog.\n"
        "Exits 1 when a test case failed or ended in error, 3 when a file\n"
        "cannot be read."),
    .options = sxml_qt4_run_option_table,
    .option_count =
        sizeof sxml_qt4_run_option_table / sizeof sxml_qt4_run_option_table[0],
    .positionals = sxml_qt4_run_positionals,
    .positional_count =
        sizeof sxml_qt4_run_positionals / sizeof sxml_qt4_run_positionals[0],
    .examples = {.texts = sxml_qt4_run_examples,
                 .text_count = sizeof sxml_qt4_run_examples /
                               sizeof sxml_qt4_run_examples[0]},
    .run = sxml_qt4_run_run_command,
};
