/*
 * sxml-xsd-test list [--catalog=FILE] [--test-set=NAME] [TEST-LIST]
 *
 * Writes every selected XSD 1.1 test with its documents, its expected
 * outcomes and whether it runs (xsd_applicability.h). Imitates
 * tests/xslt/driver/command_list.c.
 *
 * Output, on standard output, one block per test:
 *
 *     saxon-Assert assert001/assert001.v1.xml
 *       instance test saxonData/Assert/assert001.v1.xml
 *       schema saxonData/Assert/assert001.xsd
 *       version 1.1 / 1.1 / -
 *       status accepted
 *       expected valid
 *       skip: version 1.0
 *
 * ("schema" lists the schema documents an instance test is validated
 * against, "built-in components" for a group without a schema test;
 * "version" gives the version attributes of the test set, the test group
 * and the test, separated by "/"; "expected" each expected element, with its
 * version in parentheses; "skip:" only when the test does not apply), then "N
 * test cases: A applicable, S skipped; D in scope". "-" stands for an empty
 * value. Problems go to standard error (selection.h) and the listing goes
 * on: an unknown test set or test case, a reference without xlink:href, a
 * test without a document.
 *
 * Exit code: 3 when a file cannot be read, else 4 when sxml failed (out of
 * memory), else 1 after a problem, else 0.
 */
#include "cli_arguments.h"
#include "selection.h"
#include "xsd_applicability.h"
#include "xsd_catalog.h"
#include "xsd_driver.h"

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

static const sxml_byte_view sxml_xsd_test_empty_text = {.bytes = NULL,
                                                        .byte_count = 0};

static const sxml_cli_option sxml_xsd_test_list_option_table[] = {
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
            "Select every test of one test set, such as saxon-Assert"),
    },
};

static const sxml_cli_positional sxml_xsd_test_list_positionals[] = {
    {
        .name = SXML_BYTE_VIEW_INITIALIZER("TEST-LIST"),
        .multiplicity = SXML_CLI_POSITIONAL_OPTIONAL,
        .summary = SXML_BYTE_VIEW_INITIALIZER(
            "File naming tests, one '<test-set> <group>/<test>' per line"),
    },
};

static const sxml_byte_view sxml_xsd_test_list_examples[] = {
    SXML_BYTE_VIEW_INITIALIZER("sxml-xsd-test list tests/xsd/slice.txt"),
    SXML_BYTE_VIEW_INITIALIZER("sxml-xsd-test list --test-set=saxon-Assert"),
};

/* The state of one run of the command. */
typedef struct sxml_xsd_test_list_state {
    size_t listed_count;
    size_t applicable_count;
    size_t in_scope_count;
} sxml_xsd_test_list_state;

/* ----------------------------------------------------------------- Output */

/* Writes " value", or " -" for an empty value. */
static void sxml_xsd_test_write_value(sxml_cli_writer *output,
                                      sxml_byte_view value) {
    sxml_cli_writer_write_byte(output, ' ');
    if (value.byte_count == 0) {
        sxml_cli_writer_write_byte(output, '-');
    } else {
        sxml_cli_writer_write_escaped(output, value);
    }
}

/* Writes "  label DOCUMENT..." on one line. */
typedef struct sxml_xsd_test_document_line {
    sxml_byte_view label;
    sxml_xsd_test_document_view documents;
} sxml_xsd_test_document_line;

static void sxml_xsd_test_write_documents(sxml_cli_writer *output,
                                          sxml_xsd_test_document_line line) {
    sxml_cli_writer_write(output, SXML_TEXT("  "));
    sxml_cli_writer_write(output, line.label);
    for (size_t document_index = 0;
         document_index < line.documents.file_path_count; document_index += 1) {
        sxml_xsd_test_write_value(output,
                                  line.documents.file_paths[document_index]);
    }
    sxml_cli_writer_write_byte(output, '\n');
}

static void sxml_xsd_test_write_test_case(sxml_cli_writer *output,
                                          const sxml_xsd_test_case *test_case) {
    sxml_cli_writer_write_escaped(output, test_case->test_set_name);
    sxml_cli_writer_write_byte(output, ' ');
    sxml_cli_writer_write_escaped(output, test_case->name);
    sxml_cli_writer_write_byte(output, '\n');
    if (test_case->kind == SXML_XSD_TEST_KIND_SCHEMA) {
        sxml_xsd_test_write_documents(
            output,
            (sxml_xsd_test_document_line){.label = SXML_TEXT("schema test"),
                                          .documents = test_case->documents});
    } else {
        sxml_xsd_test_write_documents(
            output,
            (sxml_xsd_test_document_line){.label = SXML_TEXT("instance test"),
                                          .documents = test_case->documents});
        if (test_case->has_schema_test) {
            sxml_xsd_test_write_documents(
                output, (sxml_xsd_test_document_line){
                            .label = SXML_TEXT("schema"),
                            .documents = test_case->schema_documents});
        } else {
            sxml_cli_writer_write_line(
                output, SXML_TEXT("  schema built-in components"));
        }
    }
    sxml_cli_writer_write(output, SXML_TEXT("  version"));
    sxml_xsd_test_write_value(output, test_case->test_set_version);
    sxml_cli_writer_write(output, SXML_TEXT(" /"));
    sxml_xsd_test_write_value(output, test_case->group_version);
    sxml_cli_writer_write(output, SXML_TEXT(" /"));
    sxml_xsd_test_write_value(output, test_case->test_version);
    sxml_cli_writer_write(output, SXML_TEXT("\n  status"));
    sxml_xsd_test_write_value(output, test_case->status);
    sxml_cli_writer_write_byte(output, '\n');
    const sxml_xsd_test_expectation_view expectations = test_case->expectations;
    for (size_t expectation_index = 0;
         expectation_index < expectations.expectation_count;
         expectation_index += 1) {
        const sxml_xsd_test_expectation *expectation =
            &expectations.expectations[expectation_index];
        sxml_cli_writer_write(output, SXML_TEXT("  expected"));
        sxml_xsd_test_write_value(output, expectation->validity);
        if (expectation->version.byte_count != 0) {
            sxml_cli_writer_write(output, SXML_TEXT(" ("));
            sxml_cli_writer_write_escaped(output, expectation->version);
            sxml_cli_writer_write_byte(output, ')');
        }
        sxml_cli_writer_write_byte(output, '\n');
    }
}

static void
sxml_xsd_test_write_decision(sxml_cli_writer *output,
                             const sxml_xsd_test_decision *decision) {
    if (decision->applicability.is_applicable) {
        return;
    }
    sxml_cli_writer_write(output, SXML_TEXT("  skip: "));
    sxml_cli_writer_write_escaped(output, decision->applicability.reason);
    if (decision->applicability.detail.byte_count != 0) {
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write_escaped(output, decision->applicability.detail);
    }
    sxml_cli_writer_write_byte(output, '\n');
}

/* -------------------------------------------------------------- Selection */

static void sxml_xsd_test_list_one(void *state, sxml_qt4_selection *selection,
                                   size_t test_case_index) {
    sxml_xsd_test_list_state *list_state = state;
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    sxml_xsd_test_case test_case = {.name = sxml_xsd_test_empty_text};
    const sxml_status status = sxml_xsd_test_catalog_read_test_case(
        sxml_xsd_test_selection_catalog(selection),
        &(sxml_qt4_test_case_request){.test_case_index = test_case_index,
                                      .problem = &problem},
        &test_case);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(selection, status, &problem);
        return;
    }
    sxml_xsd_test_decision decision;
    sxml_xsd_test_decide(&test_case, &decision);
    sxml_cli_writer *output = selection->invocation->standard_output;
    sxml_xsd_test_write_test_case(output, &test_case);
    sxml_xsd_test_write_decision(output, &decision);
    list_state->listed_count += 1;
    if (decision.applicability.is_applicable) {
        list_state->applicable_count += 1;
    }
    if (decision.applicability.is_in_scope) {
        list_state->in_scope_count += 1;
    }
}

/* ---------------------------------------------------------------- Command */

static sxml_status
sxml_xsd_test_find_option(const sxml_cli_invocation *invocation,
                          sxml_byte_view long_name,
                          sxml_cli_option_match *match) {
    return sxml_cli_arguments_find_option(
        invocation->arguments,
        (sxml_cli_option_lookup){.long_name = long_name, .occurrence_index = 0},
        match);
}

static void sxml_xsd_test_write_counts(sxml_cli_writer *output,
                                       const sxml_xsd_test_list_state *state) {
    sxml_cli_writer_write_decimal(output, state->listed_count);
    sxml_cli_writer_write(output, state->listed_count == 1
                                      ? SXML_TEXT(" test case: ")
                                      : SXML_TEXT(" test cases: "));
    sxml_cli_writer_write_decimal(output, state->applicable_count);
    sxml_cli_writer_write(output, SXML_TEXT(" applicable, "));
    sxml_cli_writer_write_decimal(output, state->listed_count -
                                              state->applicable_count);
    sxml_cli_writer_write(output, SXML_TEXT(" skipped; "));
    sxml_cli_writer_write_decimal(output, state->in_scope_count);
    sxml_cli_writer_write_line(output, SXML_TEXT(" in scope"));
}

static sxml_cli_exit_code
sxml_xsd_test_run_list_command(const sxml_cli_invocation *invocation) {
    const sxml_cli_option_match absent = {.is_present = false,
                                          .occurrence_count = 0,
                                          .value = sxml_xsd_test_empty_text};
    sxml_cli_option_match catalog_option = absent;
    sxml_cli_option_match test_set_option = absent;
    sxml_status status = sxml_xsd_test_find_option(
        invocation, SXML_TEXT("catalog"), &catalog_option);
    if (status == SXML_STATUS_OK) {
        status = sxml_xsd_test_find_option(invocation, SXML_TEXT("test-set"),
                                           &test_set_option);
    }
    if (status != SXML_STATUS_OK) {
        return SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    if (test_set_option.is_present &&
        sxml_cli_arguments_positional_count(invocation->arguments) != 0) {
        return sxml_cli_invocation_report_usage_error(
            invocation, SXML_TEXT("give TEST-LIST or --test-set, not both"));
    }
    sxml_xsd_test_catalog *catalog = NULL;
    status =
        sxml_xsd_test_catalog_create(invocation->allocator,
                                     &(sxml_xsd_test_catalog_configuration){
                                         .allocator = invocation->allocator,
                                         .file_reader = invocation->file_reader,
                                         .catalog_path = catalog_option.value},
                                     &catalog);
    if (status != SXML_STATUS_OK) {
        return status == SXML_STATUS_INVALID_ARGUMENT
                   ? sxml_cli_invocation_report_usage_error(
                         invocation, SXML_TEXT("--catalog must not be empty"))
                   : SXML_CLI_EXIT_INTERNAL_ERROR;
    }
    sxml_qt4_selection selection = {
        .invocation = invocation,
        .command_name = SXML_TEXT("list"),
        .catalog = sxml_xsd_test_catalog_as_test_catalog(catalog),
        .has_problem = false,
        .has_unreadable_file = false,
        .has_failed = false};
    sxml_xsd_test_list_state state = {
        .listed_count = 0, .applicable_count = 0, .in_scope_count = 0};
    sxml_qt4_problem problem = sxml_qt4_selection_no_problem();
    status = sxml_xsd_test_catalog_load(catalog, &problem);
    if (status != SXML_STATUS_OK) {
        sxml_qt4_selection_report_failure(&selection, status, &problem);
    } else {
        sxml_qt4_selection_walk(
            &selection,
            &(sxml_qt4_selection_request){
                .has_test_set_name = test_set_option.is_present,
                .test_set_name = test_set_option.value,
                .visitor = {.state = &state, .visit = sxml_xsd_test_list_one}});
        sxml_xsd_test_write_counts(invocation->standard_output, &state);
    }
    sxml_xsd_test_catalog_destroy(catalog);
    return sxml_qt4_selection_exit_code(&selection);
}

const sxml_cli_command sxml_xsd_test_list_command = {
    .name = SXML_BYTE_VIEW_INITIALIZER("list"),
    .summary = SXML_BYTE_VIEW_INITIALIZER(
        "List tests with their documents, expected outcomes and applicability"),
    .description = SXML_BYTE_VIEW_INITIALIZER(
        "Reads the XSD test suite's suite.xml and test sets with sxml's XML\n"
        "parser and writes each selected test: whether it is a schema test\n"
        "or an instance test, its documents, the schema documents an\n"
        "instance test is validated against, the version attributes of\n"
        "its test set, test group and test, its status, its expected\n"
        "outcomes, and, when it does not apply to an XSD 1.1 processor,\n"
        "\"skip:\" and the reason. The last line counts the tests, those\n"
        "that apply, and those in scope. The selection is the tests of\n"
        "TEST-LIST, those of the test set --test-set names, or with neither\n"
        "every test of the suite. Exits 1 when a test set or test does not\n"
        "exist or a test is malformed, 3 when a file cannot be read."),
    .options = sxml_xsd_test_list_option_table,
    .option_count = sizeof sxml_xsd_test_list_option_table /
                    sizeof sxml_xsd_test_list_option_table[0],
    .positionals = sxml_xsd_test_list_positionals,
    .positional_count = sizeof sxml_xsd_test_list_positionals /
                        sizeof sxml_xsd_test_list_positionals[0],
    .examples = {.texts = sxml_xsd_test_list_examples,
                 .text_count = sizeof sxml_xsd_test_list_examples /
                               sizeof sxml_xsd_test_list_examples[0]},
    .run = sxml_xsd_test_run_list_command,
};
