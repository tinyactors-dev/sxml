#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cli_output.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"
#include "test_run.h"

/*
 * The run report of sxml-qt4 run (glossary: run_report): outcomes as the
 * tests finish, then counts per test set and overall.
 *
 * Text: one line per test case that did not pass,
 *
 *     fail op-numeric-add K-NumericAdd-29: assert-eq: got 3, expected 2
 *
 * (outcome names of sxml_qt4_test_outcome_name; not-run test cases are
 * only counted, as a whole run skips thousands), then one line per test
 * set that had test cases,
 *
 *     test set fn-abs: 30 test cases: 28 passed (1 wrong error), 1 failed,
 *         0 errors, 1 not run
 *
 * (on one line), and last
 *
 *     total: 198 test cases: 150 passed (2 wrong error), ...
 *
 * where "passed" counts passes and wrong errors (catalog-schema.xsd,
 * element "error": a wrong code counts as a pass).
 *
 * JSON (for the dashboard of packet 1.27), one object:
 *
 *     {"report": "sxml-qt4 run", "version": 1, "spec": "XP40",
 *      "tests": [{"test_set": "fn-abs", "test_case": "fn-abs-1",
 *                 "outcome": "pass", "reason": ""}, ...],
 *      "test_sets": [{"name": "fn-abs", "total": 30, "pass": 27,
 *                     "wrong_error": 1, "fail": 1, "error": 0,
 *                     "not_run": 1}, ...],
 *      "totals": {"total": 198, "pass": ..., "wrong_error": ...,
 *                 "fail": ..., "error": ..., "not_run": ...}}
 *
 * "tests" holds every selected test case in run order, passes included;
 * "test_sets" holds the test sets with test cases in catalog order;
 * "passed" in the text is pass + wrong_error.
 */

typedef enum sxml_qt4_report_format {
    SXML_QT4_REPORT_FORMAT_TEXT,
    SXML_QT4_REPORT_FORMAT_JSON,
} sxml_qt4_report_format;

/* Counts of each outcome, indexed by sxml_qt4_test_outcome. */
typedef struct sxml_qt4_outcome_counts {
    size_t counts[SXML_QT4_TEST_OUTCOME_COUNT];
} sxml_qt4_outcome_counts;

/* A report. A plain value: start it, add test cases, finish it, release
   it. */
typedef struct sxml_qt4_report {
    sxml_cli_writer *output;
    sxml_qt4_report_format format;
    sxml_allocator allocator;
    /* One per test set of the catalog; NULL when there are none. */
    sxml_qt4_outcome_counts *test_set_counts;
    size_t test_set_count;
    sxml_qt4_outcome_counts totals;
    size_t added_count;
} sxml_qt4_report;

typedef struct sxml_qt4_report_configuration {
    sxml_cli_writer *output;
    sxml_qt4_report_format format;
    /* The number of test sets of the catalog. */
    size_t test_set_count;
    /* For the JSON "spec" member, such as "XP40". */
    sxml_byte_view spec;
    sxml_allocator allocator;
} sxml_qt4_report_configuration;

/*
 * Starts *report: allocates its counts and writes the start of the JSON
 * object. Returns the allocator's failures, leaving *report untouched.
 */
SXML_NODISCARD sxml_status
sxml_qt4_report_start(const sxml_qt4_report_configuration *configuration,
                      sxml_qt4_report *report);

/* One finished test case. Views are borrowed for the call. */
typedef struct sxml_qt4_report_entry {
    size_t test_set_index;
    sxml_byte_view test_set_name;
    sxml_byte_view test_case_name;
    sxml_qt4_test_result result;
} sxml_qt4_report_entry;

void sxml_qt4_report_add(sxml_qt4_report *report,
                         const sxml_qt4_report_entry *entry);

/* Names test sets for the summary. */
typedef struct sxml_qt4_test_set_names {
    const void *state;
    /* The name of test set test_set_index. */
    sxml_byte_view (*get_name)(const void *state, size_t test_set_index);
} sxml_qt4_test_set_names;

/* Writes the counts per test set and the totals. */
void sxml_qt4_report_finish(sxml_qt4_report *report,
                            sxml_qt4_test_set_names names);

/* The test cases that passed, wrong errors included. */
size_t sxml_qt4_report_passed_count(const sxml_qt4_report *report);

/* Releases the counts. Accepts a report that was never started
   (test_set_counts NULL). */
void sxml_qt4_report_release(sxml_qt4_report *report);
