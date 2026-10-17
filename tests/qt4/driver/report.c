/*
 * The run report (report.h). Imitates the output functions of
 * tests/qt4/driver/command_list.c.
 */
#include "report.h"

#include <stdalign.h>

#define SXML_TEXT(literal) SXML_BYTE_VIEW_FROM_LITERAL(literal)

/* The JSON member names of the outcomes, in sxml_qt4_test_outcome
   order. */
static const sxml_byte_view sxml_qt4_count_names[SXML_QT4_TEST_OUTCOME_COUNT] = {
    SXML_BYTE_VIEW_INITIALIZER("pass"),
    SXML_BYTE_VIEW_INITIALIZER("wrong_error"),
    SXML_BYTE_VIEW_INITIALIZER("fail"),
    SXML_BYTE_VIEW_INITIALIZER("error"),
    SXML_BYTE_VIEW_INITIALIZER("not_run"),
};

sxml_status
sxml_qt4_report_start(const sxml_qt4_report_configuration *configuration,
                      sxml_qt4_report *report) {
    void *memory = NULL;
    if (configuration->test_set_count != 0) {
        size_t byte_count = 0;
        sxml_status status =
            sxml_size_multiply(configuration->test_set_count,
                               sizeof(sxml_qt4_outcome_counts), &byte_count);
        if (status == SXML_STATUS_OK) {
            status = sxml_allocator_allocate(
                configuration->allocator,
                (sxml_allocation_request){
                    .byte_count = byte_count,
                    .alignment = alignof(sxml_qt4_outcome_counts)},
                &memory);
        }
        if (status != SXML_STATUS_OK) {
            return status;
        }
        sxml_qt4_outcome_counts *counts = memory;
        for (size_t test_set_index = 0;
             test_set_index < configuration->test_set_count;
             test_set_index += 1) {
            counts[test_set_index] = (sxml_qt4_outcome_counts){.counts = {0}};
        }
    }
    *report = (sxml_qt4_report){.output = configuration->output,
                                .format = configuration->format,
                                .allocator = configuration->allocator,
                                .test_set_counts = memory,
                                .test_set_count = configuration->test_set_count,
                                .totals = {.counts = {0}},
                                .added_count = 0};
    if (report->format == SXML_QT4_REPORT_FORMAT_JSON) {
        sxml_cli_writer_write(report->output,
                              SXML_TEXT("{\"report\": \"sxml-qt4 run\", "
                                        "\"version\": 1, \"spec\": "));
        sxml_cli_writer_write_json_string(report->output, configuration->spec);
        sxml_cli_writer_write(report->output, SXML_TEXT(",\n\"tests\": ["));
    }
    return SXML_STATUS_OK;
}

void sxml_qt4_report_add(sxml_qt4_report *report,
                         const sxml_qt4_report_entry *entry) {
    const sxml_qt4_test_outcome outcome = entry->result.outcome;
    report->totals.counts[outcome] += 1;
    if (entry->test_set_index < report->test_set_count) {
        report->test_set_counts[entry->test_set_index].counts[outcome] += 1;
    }
    sxml_cli_writer *output = report->output;
    if (report->format == SXML_QT4_REPORT_FORMAT_JSON) {
        sxml_cli_writer_write(output, report->added_count == 0
                                          ? SXML_TEXT("\n")
                                          : SXML_TEXT(",\n"));
        sxml_cli_writer_write(output, SXML_TEXT("{\"test_set\": "));
        sxml_cli_writer_write_json_string(output, entry->test_set_name);
        sxml_cli_writer_write(output, SXML_TEXT(", \"test_case\": "));
        sxml_cli_writer_write_json_string(output, entry->test_case_name);
        sxml_cli_writer_write(output, SXML_TEXT(", \"outcome\": "));
        sxml_cli_writer_write_json_string(output,
                                          sxml_qt4_test_outcome_name(outcome));
        sxml_cli_writer_write(output, SXML_TEXT(", \"reason\": "));
        sxml_cli_writer_write_json_string(output, entry->result.reason);
        sxml_cli_writer_write_byte(output, '}');
    } else if (outcome != SXML_QT4_TEST_OUTCOME_PASS &&
               outcome != SXML_QT4_TEST_OUTCOME_NOT_RUN) {
        sxml_cli_writer_write(output, sxml_qt4_test_outcome_name(outcome));
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write_escaped(output, entry->test_set_name);
        sxml_cli_writer_write_byte(output, ' ');
        sxml_cli_writer_write_escaped(output, entry->test_case_name);
        sxml_cli_writer_write(output, SXML_TEXT(": "));
        sxml_cli_writer_write_escaped(output, entry->result.reason);
        sxml_cli_writer_write_byte(output, '\n');
    }
    report->added_count += 1;
}

static size_t sxml_qt4_counts_total(const sxml_qt4_outcome_counts *counts) {
    size_t total = 0;
    for (size_t outcome_index = 0; outcome_index < SXML_QT4_TEST_OUTCOME_COUNT;
         outcome_index += 1) {
        total += counts->counts[outcome_index];
    }
    return total;
}

/* "N test cases: P passed (W wrong error), F failed, E errors, R not
   run". */
static void sxml_qt4_write_counts_text(sxml_cli_writer *output,
                                       const sxml_qt4_outcome_counts *counts) {
    const size_t total = sxml_qt4_counts_total(counts);
    sxml_cli_writer_write_decimal(output, total);
    sxml_cli_writer_write(output, total == 1 ? SXML_TEXT(" test case: ")
                                             : SXML_TEXT(" test cases: "));
    sxml_cli_writer_write_decimal(
        output, counts->counts[SXML_QT4_TEST_OUTCOME_PASS] +
                    counts->counts[SXML_QT4_TEST_OUTCOME_WRONG_ERROR]);
    sxml_cli_writer_write(output, SXML_TEXT(" passed ("));
    sxml_cli_writer_write_decimal(
        output, counts->counts[SXML_QT4_TEST_OUTCOME_WRONG_ERROR]);
    sxml_cli_writer_write(output, SXML_TEXT(" wrong error), "));
    sxml_cli_writer_write_decimal(output,
                                  counts->counts[SXML_QT4_TEST_OUTCOME_FAIL]);
    sxml_cli_writer_write(output, SXML_TEXT(" failed, "));
    sxml_cli_writer_write_decimal(output,
                                  counts->counts[SXML_QT4_TEST_OUTCOME_ERROR]);
    sxml_cli_writer_write(output, SXML_TEXT(" errors, "));
    sxml_cli_writer_write_decimal(
        output, counts->counts[SXML_QT4_TEST_OUTCOME_NOT_RUN]);
    sxml_cli_writer_write_line(output, SXML_TEXT(" not run"));
}

static void sxml_qt4_write_counts_json(sxml_cli_writer *output,
                                       const sxml_qt4_outcome_counts *counts) {
    sxml_cli_writer_write(output, SXML_TEXT("\"total\": "));
    sxml_cli_writer_write_decimal(output, sxml_qt4_counts_total(counts));
    for (size_t outcome_index = 0; outcome_index < SXML_QT4_TEST_OUTCOME_COUNT;
         outcome_index += 1) {
        sxml_cli_writer_write(output, SXML_TEXT(", \""));
        sxml_cli_writer_write(output, sxml_qt4_count_names[outcome_index]);
        sxml_cli_writer_write(output, SXML_TEXT("\": "));
        sxml_cli_writer_write_decimal(output, counts->counts[outcome_index]);
    }
}

void sxml_qt4_report_finish(sxml_qt4_report *report,
                            sxml_qt4_test_set_names names) {
    sxml_cli_writer *output = report->output;
    const bool is_json = report->format == SXML_QT4_REPORT_FORMAT_JSON;
    if (is_json) {
        sxml_cli_writer_write(output, SXML_TEXT("],\n\"test_sets\": ["));
    }
    size_t written_count = 0;
    for (size_t test_set_index = 0; test_set_index < report->test_set_count;
         test_set_index += 1) {
        const sxml_qt4_outcome_counts *counts =
            &report->test_set_counts[test_set_index];
        if (sxml_qt4_counts_total(counts) == 0) {
            continue;
        }
        const sxml_byte_view name = names.get_name(names.state, test_set_index);
        if (is_json) {
            sxml_cli_writer_write(output, written_count == 0
                                              ? SXML_TEXT("\n{\"name\": ")
                                              : SXML_TEXT(",\n{\"name\": "));
            sxml_cli_writer_write_json_string(output, name);
            sxml_cli_writer_write(output, SXML_TEXT(", "));
            sxml_qt4_write_counts_json(output, counts);
            sxml_cli_writer_write_byte(output, '}');
        } else {
            sxml_cli_writer_write(output, SXML_TEXT("test set "));
            sxml_cli_writer_write_escaped(output, name);
            sxml_cli_writer_write(output, SXML_TEXT(": "));
            sxml_qt4_write_counts_text(output, counts);
        }
        written_count += 1;
    }
    if (is_json) {
        sxml_cli_writer_write(output, SXML_TEXT("],\n\"totals\": {"));
        sxml_qt4_write_counts_json(output, &report->totals);
        sxml_cli_writer_write_line(output, SXML_TEXT("}}"));
    } else {
        sxml_cli_writer_write(output, SXML_TEXT("total: "));
        sxml_qt4_write_counts_text(output, &report->totals);
    }
}

size_t sxml_qt4_report_passed_count(const sxml_qt4_report *report) {
    return report->totals.counts[SXML_QT4_TEST_OUTCOME_PASS] +
           report->totals.counts[SXML_QT4_TEST_OUTCOME_WRONG_ERROR];
}

void sxml_qt4_report_release(sxml_qt4_report *report) {
    sxml_allocator_release(report->allocator, report->test_set_counts);
    report->test_set_counts = NULL;
    report->test_set_count = 0;
}
