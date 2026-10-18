/*
 * sxml-xslt-test run [--catalog=FILE] [--test-set=NAME]
 *                    [--format=text|json] [TEST-LIST]
 *
 * Runs every selected XSLT 4.0 test case (xslt_test_run.h) and writes the
 * QT4 driver's run report (report.h, spec "XSLT40") to standard output.
 * Imitates tests/qt4/driver/command_run.c; the selection is
 * selection.h's. Each applicable test case is executed through
 * include/sxml/xslt.h (xslt_execution.h, packet 4.15).
 *
 * Exit code: 3 when a file cannot be read, else 4 when the driver failed
 * (out of memory), else 1 after a problem or when a test case failed or
 * ended in error, else 0.
 */
#include "cli_arguments.h"
#include "report.h"
#include "xslt_catalog.h"
#include "xslt_driver.h"
#include "selection.h"
#include "xslt_test_run.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xslt_test_empty_text = {.bytes = NULL,
                                                         .byte_count = 0};

/* The reasons of one test case. */
enum { SXML_XSLT_TEST_RUN_ARENA_BLOCK_BYTE_COUNT = 16 * 1024 };

static const sxml_byte_view sxml_xslt_test_run_format_choices[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option sxml_xslt_test_run_option_table[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("catalog"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value =
            SXML_BYTE_VIEW_INITIALIZER("third_party/xslt40-test/catalog.xml"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The catalog.xml of the suite"),
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
        .choices = {.texts = sxml_xslt_test_run_format_choices,
                    .text_count = sizeof sxml_xslt_test_run_format_choices /
                                  sizeof sxml_xslt_test_run_format_choices[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The form of the report"),
    },
};

static const sxml_cli_positional sxml_xslt_test_run_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("TEST-LIST"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File naming test cases, one '<test-set> <test-case>' per line"),
    },
};

static const sxml_byte_view sxml_xslt_test_run_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml-xslt-test run tests/xslt/slice.txt"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-xslt-test run --test-set=apply-templates"),
    SXML_BYTE_VIEW_INITIALIZER(
        "sxml-xslt-test run --format=json > report.json"),
};

/* The state of one run of the command. */
typedef struct sxml_xslt_test_run_state {
    sxml_qt4_report report;
    /* The reasons of the test case being run; reset for each. */
    sxml_arena *reason_arena;
} sxml_xslt_test_run_state;

static void sxml_xslt_test_run_one(void *state,
                                   sxml_qt4_selection *selection,
                                   size_t test_case_index) {
    sxml_xslt_test_run_state *run = state;
    sxml_arena_reset(run->reason_arena);
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_xslt_test_case test_case = {.name = sxml_xslt_test_empty_text};
    sxml_status status = sxml_xslt_test_catalog_read_test_case(
        sxml_xslt_test_selection_catalog(selection),
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &problem},
        &test_case);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    sxml_qt4_test_result result = {.outcome = SXML_QT4_TEST_OUTCOME_ERROR,
                                   .reason = sxml_xslt_test_empty_text,
                                   .is_in_scope = false};
    status = sxml_xslt_test_run_test_case(
        &(sxml_xslt_test_run_request){
            .catalog = sxml_xslt_test_selection_catalog(selection),
            .test_case = &test_case,
            .allocator = selection->invocation->allocator,
            .arena = run->reason_arena},
        &result);
    if (status != SXML_STATUS_OK) {
        problem.description = sxml_status_name(status);
        problem.subject = test_case.name;
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    size_t test_set_index = 0;
    (void)sxml_xslt_test_catalog_get_open_test_set(sxml_xslt_test_selection_catalog(selection),
                                                   &test_set_index);
    sxml_qt4_report_add(&run->report,
                        &(sxml_qt4_report_entry){
                            .test_set_index = test_set_index,
                            .test_set_name = test_case.test_set_name,
                            .test_case_name = test_case.name,
                            .result = result});
}

/* The options of one invocation. */
typedef struct sxml_xslt_test_run_options {
    sxml_cli_option_match catalog;
    sxml_cli_option_match test_set;
    sxml_cli_option_match format;
} sxml_xslt_test_run_options;

static sxml_status
sxml_xslt_test_run_find_options(const sxml_cli_invocation *invocation,
                                sxml_xslt_test_run_options *options) {
    const struct {
        sxml_byte_view long_name;
        sxml_cli_option_match *match;
    } lookups[] = {
        {SXML_TEXT("catalog"), &options->catalog},
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

/* Loads the catalog, runs the selection and writes the report. */
static void sxml_xslt_test_run_catalog(sxml_qt4_selection *selection,
                                       const sxml_xslt_test_run_options *options,
                                       sxml_xslt_test_run_state *run) {
    const sxml_cli_invocation *invocation = selection->invocation;
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_status status =
        sxml_xslt_test_catalog_load(sxml_xslt_test_selection_catalog(selection), &problem);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    status = sxml_arena_create(
        invocation->allocator,
        &(sxml_arena_configuration){
            .block_allocator = invocation->allocator,
            .block_byte_count = SXML_XSLT_TEST_RUN_ARENA_BLOCK_BYTE_COUNT,
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
                .test_set_count =
                    sxml_xslt_test_catalog_test_set_count(sxml_xslt_test_selection_catalog(selection)),
                .spec = SXML_TEXT("XSLT40"),
                .program_name = SXML_TEXT("sxml-xslt-test"),
                .allocator = invocation->allocator},
            &run->report);
    }
    if (status != SXML_STATUS_OK) {
        selection->has_failed = true;
        return;
    }
    sxml_qt4_selection_walk(
        selection,
        &(sxml_qt4_selection_request){
            .has_test_set_name = options->test_set.is_present,
            .test_set_name = options->test_set.value,
            .visitor = {.state = run, .visit = sxml_xslt_test_run_one}});
    sxml_qt4_report_finish(
        &run->report,
        (sxml_qt4_test_set_names){
            .state = &selection->catalog,
            .get_name = sxml_qt4_test_catalog_test_set_name});
}

static sxml_cli_exit_code
sxml_xslt_test_run_exit_code(const sxml_qt4_selection *selection,
                             const sxml_xslt_test_run_state *run) {
    const sxml_cli_exit_code exit_code =
        sxml_qt4_selection_exit_code(selection);
    if (exit_code != SXML_CLI_EXIT_SUCCESS) {
        return exit_code;
    }
    const sxml_qt4_outcome_counts *totals = &run->report.totals;
    if (totals->counts[SXML_QT4_TEST_OUTCOME_FAIL] != 0 ||
        totals->counts[SXML_QT4_TEST_OUTCOME_ERROR] != 0) {
        return SXML_CLI_EXIT_OPERATION_FAILED;
    }
    return SXML_CLI_EXIT_SUCCESS;
}

static sxml_cli_exit_code
sxml_xslt_test_run_run_command(const sxml_cli_invocation *invocation) {
    const sxml_cli_option_match absent = {.is_present = false,
                                          .occurrence_count = 0,
                                          .value = sxml_xslt_test_empty_text};
    sxml_xslt_test_run_options options = {
        .catalog = absent, .test_set = absent, .format = absent};
    if (sxml_xslt_test_run_find_options(invocation, &options) !=
        SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (options.test_set.is_present &&
        sxml_cli_arguments_positional_count(invocation->arguments) != 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give TEST-LIST or --test-set, not both"));
    }
    sxml_xslt_test_catalog *catalog = NULL;
    const sxml_status status = sxml_xslt_test_catalog_create(
        invocation->allocator,
        &(sxml_xslt_test_catalog_configuration){
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
    sxml_qt4_selection selection = {.invocation = invocation,
                                          .command_name = SXML_TEXT("run"),
                                          .catalog = sxml_xslt_test_catalog_as_test_catalog(catalog),
                                          .has_problem = false,
                                          .has_unreadable_file = false,
                                          .has_failed = false};
    sxml_xslt_test_run_state run = {
        .report = {.output = NULL, .test_set_counts = NULL},
        .reason_arena = NULL};
    sxml_xslt_test_run_catalog(&selection, &options, &run);
    const sxml_cli_exit_code exit_code =
        sxml_xslt_test_run_exit_code(&selection, &run);
    sxml_qt4_report_release(&run.report);
    sxml_arena_destroy(run.reason_arena);
    sxml_xslt_test_catalog_destroy(catalog);
    return exit_code;
}

const sxml_cli_command sxml_xslt_test_run_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("run"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Run test cases and report them"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Plans each selected test case's transformation (principal\n"
        "stylesheet or package, secondary modules, static and stylesheet\n"
        "parameters, global context item, invocation, delivery) and checks\n"
        "that its assertions are known, runs it through sxml's XSLT API in\n"
        "a processor of its own and checks its assertions. Writes each test\n"
        "case that failed\n"
        "or ended in error, counts per test set and a \"total:\" line;\n"
        "--format=json writes every test case and how many are in the D95\n"
        "scope. The selection is the test cases of TEST-LIST, those of the\n"
        "test set --test-set names, or with neither every test case of the\n"
        "catalog. Exits 1 when a test case failed or ended in error, 3 when\n"
        "a file cannot be read."),
    .options = sxml_xslt_test_run_option_table,
    .option_count = sizeof sxml_xslt_test_run_option_table /
                    sizeof sxml_xslt_test_run_option_table[0],
    .positionals = sxml_xslt_test_run_positionals,
    .positional_count = sizeof sxml_xslt_test_run_positionals /
                        sizeof sxml_xslt_test_run_positionals[0],
    .examples = {.texts = sxml_xslt_test_run_examples,
                 .text_count = sizeof sxml_xslt_test_run_examples /
                               sizeof sxml_xslt_test_run_examples[0]},
    .run = sxml_xslt_test_run_run_command,
};
