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
    if (configuration->area_names.area_name_count >
        SXML_QT4_REPORT_MAXIMUM_AREA_COUNT) {
        return SXML_STATUS_INVALID_ARGUMENT;
    }
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
            counts[test_set_index] = (sxml_qt4_outcome_counts){
                .counts = {0}, .in_scope_count = 0, .in_scope_passed_count = 0};
        }
    }
    *report = (sxml_qt4_report){.output = configuration->output,
                                .format = configuration->format,
                                .allocator = configuration->allocator,
                                .test_set_counts = memory,
                                .test_set_count = configuration->test_set_count,
                                .area_names = configuration->area_names,
                                .totals = {.counts = {0},
                                           .in_scope_count = 0,
                                           .in_scope_passed_count = 0},
                                .added_count = 0};
    if (report->format == SXML_QT4_REPORT_FORMAT_JSON) {
        /* "report": "PROGRAM run" (D157: the XSLT driver's runs). */
        sxml_cli_writer_write(report->output, SXML_TEXT("{\"report\": \""));
        sxml_cli_writer_write(report->output,
                              configuration->program_name.byte_count > 0
                                  ? configuration->program_name
                                  : SXML_TEXT("sxml-qt4"));
        sxml_cli_writer_write(report->output,
                              SXML_TEXT(" run\", \"version\": 1, \"spec\": "));
        sxml_cli_writer_write_json_string(report->output, configuration->spec);
        sxml_cli_writer_write(report->output, SXML_TEXT(",\n\"tests\": ["));
    }
    return SXML_STATUS_OK;
}

/* Counts one test case. */
static void sxml_qt4_count(sxml_qt4_outcome_counts *counts,
                           const sxml_qt4_test_result *result) {
    counts->counts[result->outcome] += 1;
    if (result->is_in_scope) {
        counts->in_scope_count += 1;
        if (result->outcome == SXML_QT4_TEST_OUTCOME_PASS ||
            result->outcome == SXML_QT4_TEST_OUTCOME_WRONG_ERROR) {
            counts->in_scope_passed_count += 1;
        }
    }
}

/* Adds counts to *into. */
static void sxml_qt4_add_counts(sxml_qt4_outcome_counts *into,
                                const sxml_qt4_outcome_counts *counts) {
    for (size_t outcome_index = 0; outcome_index < SXML_QT4_TEST_OUTCOME_COUNT;
         outcome_index += 1) {
        into->counts[outcome_index] += counts->counts[outcome_index];
    }
    into->in_scope_count += counts->in_scope_count;
    into->in_scope_passed_count += counts->in_scope_passed_count;
}

void sxml_qt4_report_add(sxml_qt4_report *report,
                         const sxml_qt4_report_entry *entry) {
    const sxml_qt4_test_outcome outcome = entry->result.outcome;
    sxml_qt4_count(&report->totals, &entry->result);
    if (entry->test_set_index < report->test_set_count) {
        sxml_qt4_count(&report->test_set_counts[entry->test_set_index],
                       &entry->result);
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
    sxml_cli_writer_write(output, SXML_TEXT(", \"in_scope\": "));
    sxml_cli_writer_write_decimal(output, counts->in_scope_count);
    sxml_cli_writer_write(output, SXML_TEXT(", \"in_scope_passed\": "));
    sxml_cli_writer_write_decimal(output, counts->in_scope_passed_count);
}

/* The areas of D94, in the order of the dashboard; a test set of none
   of them is in the area "other", which comes last. */
static const sxml_byte_view sxml_qt4_area_names[] = {
    SXML_BYTE_VIEW_INITIALIZER("fn"),     SXML_BYTE_VIEW_INITIALIZER("op"),
    SXML_BYTE_VIEW_INITIALIZER("prod"),   SXML_BYTE_VIEW_INITIALIZER("map"),
    SXML_BYTE_VIEW_INITIALIZER("array"),  SXML_BYTE_VIEW_INITIALIZER("math"),
    SXML_BYTE_VIEW_INITIALIZER("xs"),     SXML_BYTE_VIEW_INITIALIZER("misc"),
    SXML_BYTE_VIEW_INITIALIZER("app"),    SXML_BYTE_VIEW_INITIALIZER("method"),
    SXML_BYTE_VIEW_INITIALIZER("upd"),    SXML_BYTE_VIEW_INITIALIZER("ft"),
    SXML_BYTE_VIEW_INITIALIZER("bin"),    SXML_BYTE_VIEW_INITIALIZER("expath"),
};

static const sxml_byte_view sxml_qt4_other_area_name =
    SXML_BYTE_VIEW_INITIALIZER("other");

/* The areas of a report: the configured ones, else D94's. */
static sxml_qt4_area_name_view
sxml_qt4_areas_of(const sxml_qt4_report *report) {
    if (report->area_names.area_name_count != 0) {
        return report->area_names;
    }
    return (sxml_qt4_area_name_view){
        .area_names = sxml_qt4_area_names,
        .area_name_count =
            sizeof sxml_qt4_area_names / sizeof sxml_qt4_area_names[0]};
}

/* The index of the area of a test set: its name up to the first "-",
   when that is an area, else the index past the areas ("other"). */
static size_t sxml_qt4_area_of(sxml_qt4_area_name_view areas,
                               sxml_byte_view test_set_name) {
    size_t prefix_byte_count = 0;
    while (prefix_byte_count < test_set_name.byte_count &&
           test_set_name.bytes[prefix_byte_count] != '-') {
        prefix_byte_count += 1;
    }
    const sxml_byte_view prefix = {.bytes = test_set_name.bytes,
                                   .byte_count = prefix_byte_count};
    for (size_t area_index = 0; area_index < areas.area_name_count;
         area_index += 1) {
        if (sxml_byte_view_equals(prefix, areas.area_names[area_index])) {
            return area_index;
        }
    }
    return areas.area_name_count;
}

/* Writes the "areas" member: the test sets' counts summed by area. */
static void sxml_qt4_write_areas_json(const sxml_qt4_report *report,
                                      sxml_qt4_test_set_names names) {
    const sxml_qt4_area_name_view areas = sxml_qt4_areas_of(report);
    /* One more than the areas: "other". */
    sxml_qt4_outcome_counts area_counts[SXML_QT4_REPORT_MAXIMUM_AREA_COUNT + 1];
    for (size_t area_index = 0; area_index <= areas.area_name_count;
         area_index += 1) {
        area_counts[area_index] = (sxml_qt4_outcome_counts){
            .counts = {0}, .in_scope_count = 0, .in_scope_passed_count = 0};
    }
    for (size_t test_set_index = 0; test_set_index < report->test_set_count;
         test_set_index += 1) {
        const sxml_qt4_outcome_counts *counts =
            &report->test_set_counts[test_set_index];
        if (sxml_qt4_counts_total(counts) != 0) {
            sxml_qt4_add_counts(
                &area_counts[sxml_qt4_area_of(
                    areas, names.get_name(names.state, test_set_index))],
                counts);
        }
    }
    sxml_cli_writer *output = report->output;
    sxml_cli_writer_write(output, SXML_TEXT("],\n\"areas\": ["));
    size_t written_count = 0;
    for (size_t area_index = 0; area_index <= areas.area_name_count;
         area_index += 1) {
        if (sxml_qt4_counts_total(&area_counts[area_index]) == 0) {
            continue;
        }
        sxml_cli_writer_write(output, written_count == 0
                                          ? SXML_TEXT("\n{\"area\": ")
                                          : SXML_TEXT(",\n{\"area\": "));
        sxml_cli_writer_write_json_string(
            output, area_index < areas.area_name_count
                        ? areas.area_names[area_index]
                        : sxml_qt4_other_area_name);
        sxml_cli_writer_write(output, SXML_TEXT(", "));
        sxml_qt4_write_counts_json(output, &area_counts[area_index]);
        sxml_cli_writer_write_byte(output, '}');
        written_count += 1;
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
        sxml_qt4_write_areas_json(report, names);
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
