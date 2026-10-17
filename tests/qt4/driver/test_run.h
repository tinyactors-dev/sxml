#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "applicability.h"
#include "catalog.h"
#include "sxml/memory.h"
#include "sxml/status.h"
#include "sxml/view.h"

/*
 * Running one QT4 test case (glossary: test_outcome; docs/QT4_CATALOG.md;
 * third_party/qt4tests/guide/running.html).
 *
 * Every test case runs in isolation: a processor of its own (name pool,
 * type registry, item heap, documents), made through an allocator of its
 * own that holds at most SXML_QT4_TEST_MAXIMUM_BYTE_COUNT live bytes, with
 * at most SXML_QT4_TEST_MAXIMUM_STEP_COUNT evaluation steps (D89). After
 * the test everything is released and the allocator must have no live
 * allocation left; otherwise the outcome is an error ("leaked"). Nothing
 * of one test reaches the next.
 *
 * The environment is applied as the catalog describes it (environment.h):
 * - sources are parsed (document URI and base URI: their uri, or
 *   file:///SUITE-PATH); role "." is the context value, "$name" binds an
 *   external variable, a uri makes the document available to fn:doc;
 * - params are external variables: select evaluated with the
 *   environment's namespaces, declared as their "as" type
 *   (sxml_xpath_parse_sequence_type) or item()*;
 * - context-item select gives the context value;
 * - namespaces are statically known; the static base URI is
 *   file:///SUITE-PATH of the test-set file, the given one, or absent;
 * - decimal formats are statically known decimal formats; a collation
 *   marked default is the default collation;
 * - resources and collections are served by the resource table
 *   (resource_table.h), which also reads other file:/// URIs as suite
 *   paths through the catalog; the sources of a collection are parsed as
 *   other sources and, with a uri, are available documents too.
 *
 * Outcomes: pass; wrong error (an error assertion met by another code);
 * fail (the assertions do not hold); error (sxml failed outside the
 * specification: a limit, running out of memory, a status without an
 * error record, a leak, an environment it cannot set up); not run (the
 * test case is not applicable, or its assertion is unsupported).
 */

/* Most live bytes of one test's allocator. */
#define SXML_QT4_TEST_MAXIMUM_BYTE_COUNT ((size_t)512 * 1024 * 1024)

/* Most evaluation steps of one evaluation of a test. */
#define SXML_QT4_TEST_MAXIMUM_STEP_COUNT ((uint64_t)100 * 1000 * 1000)

typedef enum sxml_qt4_test_outcome {
    SXML_QT4_TEST_OUTCOME_PASS,
    SXML_QT4_TEST_OUTCOME_WRONG_ERROR,
    SXML_QT4_TEST_OUTCOME_FAIL,
    SXML_QT4_TEST_OUTCOME_ERROR,
    SXML_QT4_TEST_OUTCOME_NOT_RUN,
} sxml_qt4_test_outcome;

/* The number of outcomes, for tables of counts. */
enum { SXML_QT4_TEST_OUTCOME_COUNT = 5 };

/* The name of an outcome as reports write it: "pass", "wrong-error",
   "fail", "error", "not-run". */
sxml_byte_view sxml_qt4_test_outcome_name(sxml_qt4_test_outcome outcome);

typedef struct sxml_qt4_test_result {
    sxml_qt4_test_outcome outcome;
    /* Empty for a pass; otherwise in the request's arena or static. */
    sxml_byte_view reason;
    /* In the D95 denominator (applicability.h). */
    bool is_in_scope;
} sxml_qt4_test_result;

/* One test case to run. Borrowed for the call. */
typedef struct sxml_qt4_test_run_request {
    /* The catalog with the test case's test set open; reads its files. */
    sxml_qt4_catalog *catalog;
    const sxml_qt4_test_case *test_case;
    sxml_qt4_target target;
    /* Backs the test's own allocator. */
    sxml_allocator allocator;
    /* Receives the reason; must outlive the result. */
    sxml_arena *arena;
} sxml_qt4_test_run_request;

/*
 * Runs request->test_case and writes its outcome to *result. Returns only
 * the driver's own failures (the arena's, which the reason needs);
 * everything that goes wrong in the test is its outcome.
 */
SXML_NODISCARD sxml_status
sxml_qt4_run_test_case(const sxml_qt4_test_run_request *request,
                       sxml_qt4_test_result *result);
