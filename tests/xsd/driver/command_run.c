/*
 * sxml-xsd-test run [--catalog=FILE] [--test-set=NAME]
 *                   [--format=text|json] [TEST-LIST]
 *
 * Runs every selected XSD 1.1 test (xsd_test_run.h) and writes the QT4
 * driver's run report (report.h, spec "XSD11", areas the contributors of
 * the test sets) to standard output. Imitates
 * tests/xslt/driver/command_run.c; the selection is selection.h's.
 *
 * Exit code: 3 when a file cannot be read, else 4 when the driver failed
 * (out of memory), else 1 after a problem or when a test case failed or
 * ended in error, else 0.
 */
#include "cli_arguments.h"
#include "report.h"
#include "selection.h"
#include "xsd_catalog.h"
#include "xsd_driver.h"
#include "xsd_test_run.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xsd_test_empty_text = {.bytes = NULL,
                                                        .byte_count = 0};

/* The contributor areas of the report: the test-set name prefixes
   (xsd_catalog.h, "Names"), in the order of the dashboard
   (tools/conformance-report.py). */
static const sxml_byte_view sxml_xsd_test_area_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("nist"),   SXML_BYTE_VIEW_INITIALIZER("ms"),
    SXML_BYTE_VIEW_INITIALIZER("sun"),    SXML_BYTE_VIEW_INITIALIZER("saxon"),
    SXML_BYTE_VIEW_INITIALIZER("ibm"),    SXML_BYTE_VIEW_INITIALIZER("wg"),
    SXML_BYTE_VIEW_INITIALIZER("oracle"), SXML_BYTE_VIEW_INITIALIZER("boeing"),
    SXML_BYTE_VIEW_INITIALIZER("common"),
};

/* The reasons of one test case. */
enum { SXML_XSD_TEST_RUN_ARENA_BLOCK_BYTE_COUNT = 16 * 1024 };

static const sxml_byte_view sxml_xsd_test_run_format_choices[] = {
    SXML_BYTE_VIEW_INITIALIZER("text"),
    SXML_BYTE_VIEW_INITIALIZER("json"),
};

static const sxml_cli_option sxml_xsd_test_run_option_table[] = {
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("catalog"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_TEXT,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FILE"),
        .choices = {.texts = NULL, .text_count = 0},
        .default_value =
            SXML_BYTE_VIEW_INITIALIZER("third_party/xsdtests/suite.xml"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The suite.xml of the suite"),
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
            "Run every test of one test set, such as saxon-Assert"),
    },
    {
        .long_name = SXML_BYTE_VIEW_INITIALIZER("format"),
        .short_name = 0,
        .value_kind = SXML_CLI_VALUE_KIND_CHOICE,
        .value_name = SXML_BYTE_VIEW_INITIALIZER("FORMAT"),
        .choices = {.texts = sxml_xsd_test_run_format_choices,
                    .text_count = sizeof sxml_xsd_test_run_format_choices /
                                  sizeof sxml_xsd_test_run_format_choices[0]},
        .default_value = SXML_BYTE_VIEW_INITIALIZER("text"),
        .multiplicity = SXML_CLI_OPTION_AT_MOST_ONCE,
        .summary = SXML_BYTE_VIEW_INITIALIZER("The form of the report"),
    },
};

static const sxml_cli_positional sxml_xsd_test_run_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("TEST-LIST"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File naming tests, one '<test-set> <group>/<test>' per line"),
    },
};

static const sxml_byte_view sxml_xsd_test_run_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml-xsd-test run tests/xsd/slice.txt"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-xsd-test run --test-set=saxon-Assert"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-xsd-test run --format=json > report.json"),
};

/* The state of one run of the command. */
typedef struct sxml_xsd_test_run_state {
    sxml_qt4_report report;
    /* The reasons of the test case being run; reset for each. */
    sxml_arena *reason_arena;
} sxml_xsd_test_run_state;

static void sxml_xsd_test_run_one(void *state, sxml_qt4_selection *selection,
                                  size_t test_case_index) {
    sxml_xsd_test_run_state *run = state;
    sxml_arena_reset(run->reason_arena);
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_xsd_test_case test_case = {.name = sxml_xsd_test_empty_text};
    sxml_status status = sxml_xsd_test_catalog_read_test_case(
        sxml_xsd_test_selection_catalog(selection),
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &problem},
        &test_case);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    sxml_qt4_test_result result = {.outcome = SXML_QT4_TEST_OUTCOME_ERROR,
                                   .reason = sxml_xsd_test_empty_text,
                                   .is_in_scope = false};
    status = sxml_xsd_test_run_test_case(
        &(sxml_xsd_test_run_request){
            .catalog = sxml_xsd_test_selection_catalog(selection),
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
    (void)sxml_xsd_test_catalog_get_open_test_set(
        sxml_xsd_test_selection_catalog(selection), &test_set_index);
    sxml_qt4_report_add(
        &run->report,
        &(sxml_qt4_report_entry){.test_set_index = test_set_index,
                                 .test_set_name = test_case.test_set_name,
                                 .test_case_name = test_case.name,
                                 .result = result});
}

/* The options of one invocation. */
typedef struct sxml_xsd_test_run_options {
    sxml_cli_option_match catalog;
    sxml_cli_option_match test_set;
    sxml_cli_option_match format;
} sxml_xsd_test_run_options;

static sxml_status
sxml_xsd_test_run_find_options(const sxml_cli_invocation *invocation,
                               sxml_xsd_test_run_options *options) {
    const struct {
        sxml_byte_view long_name;
        sxml_cli_option_match *match;
    } lookups[] = {
        {SXML_TEXT("catalog"), &options->catalog},
        {SXML_TEXT("test-set"), &options->test_set},
        {SXML_TEXT("format"), &options->format},
    };
    for (size_t lookup_index = 0;
         lookup_index < sizeof lookups / sizeof lookups[0]; lookup_index += 1) {
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
static void sxml_xsd_test_run_catalog(sxml_qt4_selection *selection,
                                      const sxml_xsd_test_run_options *options,
                                      sxml_xsd_test_run_state *run) {
    const sxml_cli_invocation *invocation = selection->invocation;
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_status status = sxml_xsd_test_catalog_load(
        sxml_xsd_test_selection_catalog(selection), &problem);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    status = sxml_arena_create(
        invocation->allocator,
        &(sxml_arena_configuration){
            .block_allocator = invocation->allocator,
            .block_byte_count = SXML_XSD_TEST_RUN_ARENA_BLOCK_BYTE_COUNT,
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
                .test_set_count = sxml_xsd_test_catalog_test_set_count(
                    sxml_xsd_test_selection_catalog(selection)),
                .spec = SXML_TEXT("XSD11"),
                .program_name = SXML_TEXT("sxml-xsd-test"),
                .area_names = {.area_names = sxml_xsd_test_area_names,
                               .area_name_count =
                                   sizeof sxml_xsd_test_area_names /
                                   sizeof sxml_xsd_test_area_names[0]},
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
            .visitor = {.state = run, .visit = sxml_xsd_test_run_one}});
    sxml_qt4_report_finish(
        &run->report, (sxml_qt4_test_set_names){
                          .state = &selection->catalog,
                          .get_name = sxml_qt4_test_catalog_test_set_name});
}

static sxml_cli_exit_code
sxml_xsd_test_run_exit_code(const sxml_qt4_selection *selection,
                            const sxml_xsd_test_run_state *run) {
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
sxml_xsd_test_run_run_command(const sxml_cli_invocation *invocation) {
    const sxml_cli_option_match absent = {.is_present = false,
                                          .occurrence_count = 0,
                                          .value = sxml_xsd_test_empty_text};
    sxml_xsd_test_run_options options = {
        .catalog = absent, .test_set = absent, .format = absent};
    if (sxml_xsd_test_run_find_options(invocation, &options) !=
        SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (options.test_set.is_present &&
        sxml_cli_arguments_positional_count(invocation->arguments) != 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give TEST-LIST or --test-set, not both"));
    }
    sxml_xsd_test_catalog *catalog = NULL;
    const sxml_status status =
        sxml_xsd_test_catalog_create(invocation->allocator,
                                     &(sxml_xsd_test_catalog_configuration){
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
    sxml_qt4_selection selection = {
        .invocation = invocation,
        .command_name = SXML_TEXT("run"),
        .catalog = sxml_xsd_test_catalog_as_test_catalog(catalog),
        .has_problem = false,
        .has_unreadable_file = false,
        .has_failed = false};
    sxml_xsd_test_run_state run = {
        .report = {.output = NULL, .test_set_counts = NULL},
        .reason_arena = NULL};
    sxml_xsd_test_run_catalog(&selection, &options, &run);
    const sxml_cli_exit_code exit_code =
        sxml_xsd_test_run_exit_code(&selection, &run);
    sxml_qt4_report_release(&run.report);
    sxml_arena_destroy(run.reason_arena);
    sxml_xsd_test_catalog_destroy(catalog);
    return exit_code;
}

const sxml_cli_command sxml_xsd_test_run_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("run"),
    .summary = SXML_BYTE_VIEW_INITIALIZER("Run test cases and report them"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Runs each selected test that applies to an XSD 1.1 processor in a\n"
        "processor of its own: reads its documents and, once sxml compiles\n"
        "schemas (packet 5.4) and validates (packet 5.7), compiles the\n"
        "schema of a schema test or validates the instance of an instance\n"
        "test against its group's schema and compares the outcome with the\n"
        "expected one; until then such a test is not run, \"XSD not\n"
        "implemented\". Writes each test that failed or ended in error,\n"
        "counts per test set and a \"total:\" line; --format=json writes\n"
        "every test, counts per contributor area and how many are in scope.\n"
        "The selection is the tests of TEST-LIST, those of the test set\n"
        "--test-set names, or with neither every test of the suite. Exits 1\n"
        "when a test failed or ended in error, 3 when a file cannot be\n"
        "read."),
    .options = sxml_xsd_test_run_option_table,
    .option_count = sizeof sxml_xsd_test_run_option_table /
                    sizeof sxml_xsd_test_run_option_table[0],
    .positionals = sxml_xsd_test_run_positionals,
    .positional_count = sizeof sxml_xsd_test_run_positionals /
                        sizeof sxml_xsd_test_run_positionals[0],
    .examples = {.texts = sxml_xsd_test_run_examples,
                 .text_count = sizeof sxml_xsd_test_run_examples /
                               sizeof sxml_xsd_test_run_examples[0]},
    .run = sxml_xsd_test_run_run_command,
};
